#!/usr/bin/env python3
"""Offline, verified backups for WARC Studio's database and archive files."""

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import sys
import tempfile
from datetime import datetime, timezone

if os.name == "nt":
    import msvcrt
else:
    import fcntl


DATABASE = "warc-studio.sqlite3"
MANIFEST = "backup-manifest.json"
IGNORED = {DATABASE + "-wal", DATABASE + "-shm", DATABASE + "-journal"}


@contextlib.contextmanager
def data_lock(root: Path):
    lock_path = Path(str(root) + ".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+b") as lock:
        try:
            if os.name == "nt":
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise RuntimeError("WARC Studio is running or another backup is in progress") from exc
        try:
            yield
        finally:
            if os.name == "nt":
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_UN)


def files_in(root: Path):
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(directory) / name
            if path.is_symlink():
                raise RuntimeError(f"Symbolic links are not allowed in backups: {path}")
        for name in files:
            path = Path(directory) / name
            if not path.is_file():
                raise RuntimeError(f"Unexpected data entry: {path}")
            yield path


def digest(path: Path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def validate_database(root: Path):
    db_path = root / DATABASE
    if not db_path.is_file() or db_path.is_symlink():
        raise RuntimeError("Backup is missing the SQLite database")
    with contextlib.closing(sqlite3.connect(db_path.as_uri() + "?mode=ro", uri=True)) as db:
        if db.execute("PRAGMA quick_check").fetchone()[0] != "ok":
            raise RuntimeError("SQLite integrity check failed")
        if db.execute("PRAGMA foreign_key_check").fetchone():
            raise RuntimeError("SQLite foreign key check failed")
        for stored, expected_sha in db.execute(
            "SELECT file_path, sha256 FROM capture WHERE status = 'archived'"
        ):
            if not stored:
                raise RuntimeError("Archived capture has no archive file path")
            relative = Path(stored)
            if relative.is_absolute() or relative.parts[:1] != ("archives",) or ".." in relative.parts:
                raise RuntimeError(f"Unsafe archive path in database: {stored}")
            archive = root / relative
            if not archive.is_file() or archive.is_symlink():
                raise RuntimeError(f"Archived capture file is missing: {stored}")
            if expected_sha and digest(archive).lower() != expected_sha.lower():
                raise RuntimeError(f"Archived capture checksum differs from database: {stored}")


def manifest_for(root: Path):
    entries = {}
    for path in files_in(root):
        relative = path.relative_to(root).as_posix()
        if relative != MANIFEST:
            entries[relative] = {"size": path.stat().st_size, "sha256": digest(path)}
    return {"format": 1, "created_utc": datetime.now(timezone.utc).isoformat(), "files": entries}


def validate_backup(root: Path):
    manifest_path = root / MANIFEST
    if not manifest_path.is_file() or manifest_path.is_symlink():
        raise RuntimeError("Backup manifest is missing")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("format") != 1 or not isinstance(manifest.get("files"), dict):
        raise RuntimeError("Unsupported backup format")
    actual = {p.relative_to(root).as_posix() for p in files_in(root)} - {MANIFEST}
    if actual != set(manifest["files"]):
        raise RuntimeError("Backup file list differs from its manifest")
    for relative, expected in manifest["files"].items():
        path = root / relative
        if path.stat().st_size != expected["size"] or digest(path) != expected["sha256"]:
            raise RuntimeError(f"Backup file is damaged: {relative}")
    validate_database(root)


def copy_data(source: Path, destination: Path, *, snapshot_database: bool):
    for path in files_in(source):
        relative = path.relative_to(source)
        if relative.name in IGNORED or relative == Path(MANIFEST) or relative == Path(DATABASE):
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    if snapshot_database:
        with contextlib.closing(sqlite3.connect((source / DATABASE).as_uri() + "?mode=ro", uri=True)) as src:
            with contextlib.closing(sqlite3.connect(destination / DATABASE)) as dst:
                src.backup(dst)
    else:
        shutil.copy2(source / DATABASE, destination / DATABASE)


def require_separate(source: Path, target: Path):
    if source == target or source in target.parents or target in source.parents:
        raise RuntimeError("Backup and data directories must be separate")


def backup(data: Path, output: Path):
    require_separate(data, output)
    if output.exists() or output.is_symlink():
        raise RuntimeError(f"Backup destination already exists: {output}")
    with data_lock(data):
        validate_database(data)
        output.parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix=f".{output.name}.partial-", dir=output.parent))
        try:
            copy_data(data, stage, snapshot_database=True)
            validate_database(stage)
            (stage / MANIFEST).write_text(json.dumps(manifest_for(stage), indent=2) + "\n", encoding="utf-8")
            validate_backup(stage)
            stage.rename(output)
        finally:
            if stage.exists():
                shutil.rmtree(stage)
    print(f"Backup created and verified: {output}")


def restore(source: Path, data: Path):
    require_separate(source, data)
    with data_lock(data):
        validate_backup(source)
        data.parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix=f".{data.name}.restore-", dir=data.parent))
        try:
            copy_data(source, stage, snapshot_database=False)
            validate_database(stage)
            previous = None
            if data.exists():
                previous = data.with_name(data.name + ".pre-restore-" +
                                          datetime.now(timezone.utc).strftime("%Y%m%d%H%M%S") +
                                          f"-{os.getpid()}")
                data.rename(previous)
            try:
                stage.rename(data)
            except Exception:
                if previous is not None:
                    previous.rename(data)
                raise
        finally:
            if stage.exists():
                shutil.rmtree(stage)
    print(f"Backup restored: {data}")
    if previous is not None:
        print(f"Previous data retained at: {previous}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("backup", "restore"):
        action = sub.add_parser(command)
        action.add_argument("path", type=Path, help="backup destination or source directory")
        action.add_argument("--data-dir", type=Path, default=Path("data"))
    args = parser.parse_args()
    data = args.data_dir.resolve()
    path = args.path.resolve()
    try:
        if args.command == "backup":
            backup(data, path)
        else:
            restore(path, data)
    except (OSError, RuntimeError, sqlite3.Error, ValueError, KeyError) as exc:
        parser.exit(1, f"Backup error: {exc}\n")


if __name__ == "__main__":
    main()
