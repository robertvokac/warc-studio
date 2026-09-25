#!/usr/bin/env python3
"""End-to-end crawl, WARC upload/replay routes, concurrency, and backup restore."""

import gzip
import io
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

from browser_replay import check_browser_replay


APP = Path(sys.argv[1]).resolve()
BACKUP_TOOL = Path(__file__).resolve().parents[1] / "tools" / "backup.py"


class Fixture(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/page":
            body = (b"<!doctype html><html><head><title>Fixture Capture</title>"
                    b'<link rel="stylesheet" href="/site.css"></head>'
                    b'<body><h1>Archived page</h1><img src="/pixel.png"></body></html>')
            content_type = "text/html"
        elif self.path == "/site.css":
            body, content_type = b"h1 { color: teal; }", "text/css"
        elif self.path == "/pixel.png":
            body, content_type = b"\x89PNG\r\n\x1a\nfixture", "image/png"
        elif self.path == "/large":
            body, content_type = b"x" * (2 * 1024 * 1024), "text/html"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(url, body=None, headers=None):
    req = urllib.request.Request(url, data=body, headers=headers or {})
    with urllib.request.urlopen(req, timeout=10) as response:
        return response.status, dict(response.headers), response.read(), response.url


def wait_for(predicate, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.1)
    raise AssertionError("Timed out waiting for application state")


def capture_row(db_path, source):
    if not db_path.exists():
        return None
    with sqlite3.connect(db_path) as db:
        return db.execute(
            "SELECT id, status, file_path, url, title, timestamp FROM capture "
            "WHERE source = ? ORDER BY id DESC LIMIT 1", (source,)
        ).fetchone()


def raw_headers(port, content_length):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall((f"POST /upload HTTP/1.1\r\nHost: localhost:{port}\r\n"
                  f"Content-Length: {content_length}\r\nConnection: close\r\n\r\n").encode())
    return sock


def run():
    with tempfile.TemporaryDirectory(prefix="warc-studio-integration-") as temporary:
        root = Path(temporary)
        data = root / "data"
        backup = root / "backup"
        port = free_port()
        replay_port = free_port()
        while replay_port == port:
            replay_port = free_port()
        fixture = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
        import threading
        fixture_thread = threading.Thread(target=fixture.serve_forever, daemon=True)
        fixture_thread.start()
        url = f"http://127.0.0.1:{fixture.server_port}/page"
        env = os.environ | {
            "WARC_STUDIO_DATA_DIR": str(data),
            "WARC_STUDIO_PORT": str(port),
            "WARC_STUDIO_REPLAY_PORT": str(replay_port),
            "WARC_STUDIO_MAX_REQUEST_MB": "1",
            "WARC_STUDIO_MAX_CONCURRENT_UPLOADS": "1",
            "WARC_STUDIO_MAX_BUFFER_MB": "1",
        }
        server = subprocess.Popen([str(APP)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        base = f"http://localhost:{port}"
        replay_base = f"http://127.0.0.1:{replay_port}"
        try:
            wait_for(lambda: server.poll() is None and available(base + "/health"))
            page = request(base + "/upload")[2].decode()
            assert 'id="upload-progress"' in page and "request.upload.onprogress" in page

            oversized = raw_headers(port, 1024 * 1024 + 1)
            assert b"413" in oversized.recv(4096)
            oversized.close()

            first = raw_headers(port, 100)
            time.sleep(0.1)
            second = raw_headers(port, 100)
            assert b"503" in second.recv(4096)
            second.close()
            first.close()

            body = urllib.parse.urlencode({"url": url, "capture_depth": "0"}).encode()
            request(base + "/save", body, {"Content-Type": "application/x-www-form-urlencoded"})
            row = wait_for(lambda: archived_row(data / "warc-studio.sqlite3", "crawl"))
            capture_id, _, stored, captured_url, title, timestamp = row
            assert captured_url == url and stored.startswith("archives/")
            archive = data / stored
            raw = gzip.open(archive, "rb").read()
            assert b"WARC/1.1" in raw and b"WARC-Type: response" in raw
            assert b"Archived page" in raw and b"h1 { color: teal; }" in raw
            assert b"/pixel.png" in raw

            replay_response = request(base + f"/capture/{capture_id}/replay")
            assert replay_response[3].startswith(replay_base)
            replay = replay_response[2].decode()
            assert "<replay-web-page" in replay and f"/{stored}" in replay
            assert captured_url.replace("&", "&amp;") in replay
            code, headers, chunk, _ = request(replay_base + "/" + stored, headers={"Range": "bytes=0-31"})
            assert code == 206 and chunk == archive.read_bytes()[:32]
            assert headers["Content-Range"].startswith("bytes 0-31/")
            assert request(replay_base + "/replay/ui.js")[0] == 200
            assert request(replay_base + "/replay/sw.js")[0] == 200
            assert http_status(base + "/" + stored) == 404
            assert http_status(base + "/replay/ui.js") == 404
            assert http_status(base + "/static/ui.js") == 404
            assert http_status(replay_base + "/health") == 404
            assert http_status(replay_base + "/save", b"url=https%3A%2F%2Fexample.org") in (403, 404, 405)
            assert http_status(base + "/save", b"url=https%3A%2F%2Fexample.org",
                               {"Origin": replay_base}) == 403
            check_browser_replay(base + f"/capture/{capture_id}/replay", "Archived page", root)

            boundary = "warc-studio-test-boundary"
            upload = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
                      f"filename=\"fixture.warc.gz\"\r\nContent-Type: application/octet-stream\r\n\r\n").encode()
            upload += archive.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
            request(base + "/upload", upload, {"Content-Type": f"multipart/form-data; boundary={boundary}"})
            uploaded = archived_row(data / "warc-studio.sqlite3", "upload")
            assert uploaded and uploaded[3] == url and uploaded[4] == "Fixture Capture"
            assert request(base + f"/capture/{uploaded[0]}/replay")[0] == 200

            malformed = (f"--{boundary}\r\nContent-Disposition: form-data; filename=\"bad.warc\""
                         f"\r\n\r\ninvalid\r\n--{boundary}--\r\n").encode()
            request(base + "/upload", malformed,
                    {"Content-Type": f"multipart/form-data; boundary={boundary}"})
            assert request(base + "/health")[0] == 200
            assert not capture_row(data / "warc-studio.sqlite3", "invalid-test")

            wacz_bytes = io.BytesIO()
            with zipfile.ZipFile(wacz_bytes, "w", zipfile.ZIP_DEFLATED) as wacz:
                wacz.writestr("datapackage.json", '{"resources": ['
                              '{"path":"pages/pages.jsonl"},'
                              '{"path":"indexes/index.cdxj"},'
                              '{"path":"archive/test.warc.gz"}]}')
                wacz.writestr("indexes/index.cdxj", "index")
                wacz.writestr("archive/test.warc.gz", archive.read_bytes())
                wacz.writestr("pages/pages.jsonl", "x" * (2 * 1024 * 1024) +
                              '\n{"url":"https://example.org/"}\n')
            bomb = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
                    f"filename=\"bomb.wacz\"\r\n\r\n").encode()
            bomb += wacz_bytes.getvalue() + f"\r\n--{boundary}--\r\n".encode()
            request(base + "/upload", bomb, {"Content-Type": f"multipart/form-data; boundary={boundary}"})
            assert request(base + "/health")[0] == 200

            invalid = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
                       f"filename=\"invalid.warc\"\r\n\r\nnot-a-warc\r\n"
                       f"--{boundary}\r\nContent-Disposition: form-data; name=\"url\"\r\n\r\n"
                       f"https://example.org/\r\n--{boundary}--\r\n").encode()
            request(base + "/upload", invalid,
                    {"Content-Type": f"multipart/form-data; boundary={boundary}"})
            with sqlite3.connect(data / "warc-studio.sqlite3") as db:
                assert db.execute("SELECT count(*) FROM capture WHERE source = 'upload'").fetchone()[0] == 1
                db.execute("CREATE TRIGGER reject_delete BEFORE DELETE ON capture "
                           f"WHEN OLD.id = {uploaded[0]} BEGIN SELECT RAISE(ABORT, 'test rollback'); END")
            uploaded_archive = data / uploaded[2]
            request(base + f"/capture/{uploaded[0]}/delete", b"")
            assert uploaded_archive.is_file() and capture_row(data / "warc-studio.sqlite3", "upload")
            with sqlite3.connect(data / "warc-studio.sqlite3") as db:
                db.execute("DROP TRIGGER reject_delete")
            request(base + f"/capture/{uploaded[0]}/delete", b"")
            assert not uploaded_archive.exists()
            assert capture_row(data / "warc-studio.sqlite3", "upload") is None
            assert http_status(base + "/web/%GG") != 500
            request(base + "/save", b"url=https%GG", {"Content-Type": "application/x-www-form-urlencoded"})
            assert request(base + "/health")[0] == 200

            large_url = f"http://127.0.0.1:{fixture.server_port}/large"
            request(base + "/save", urllib.parse.urlencode({"url": large_url}).encode(),
                    {"Content-Type": "application/x-www-form-urlencoded"})
            wait_for(lambda: capture_row(data / "warc-studio.sqlite3", "crawl")
                     and capture_row(data / "warc-studio.sqlite3", "crawl")[1] == "failed")

            blocked = subprocess.run(
                [sys.executable, BACKUP_TOOL, "backup", backup, "--data-dir", data],
                capture_output=True, text=True)
            assert blocked.returncode != 0 and "running" in blocked.stderr
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)
            fixture.shutdown()
            fixture.server_close()

        archive = data / stored
        staged = Path(str(archive) + f".pending-delete-{capture_id}")
        archive.rename(staged)
        restarted = subprocess.Popen([str(APP)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            wait_for(lambda: restarted.poll() is None and available(base + "/health"))
            with sqlite3.connect(data / "warc-studio.sqlite3") as db:
                original_row = db.execute("SELECT id, file_path FROM capture WHERE id = ?", (capture_id,)).fetchone()
            if not archive.is_file() or staged.exists():
                restarted.terminate()
                restarted.wait(timeout=10)
                raise AssertionError((list(archive.parent.iterdir()), original_row, archive,
                                      restarted.stderr.read().decode()))
        finally:
            restarted.terminate()
            restarted.wait(timeout=10)

        subprocess.run([sys.executable, BACKUP_TOOL, "backup", backup, "--data-dir", data], check=True)
        source_archive = data / stored
        original_bytes = source_archive.read_bytes()
        source_archive.write_bytes(bytes([original_bytes[0] ^ 1]) + original_bytes[1:])
        mismatch = subprocess.run([sys.executable, BACKUP_TOOL, "backup", root / "mismatch",
                                   "--data-dir", data], capture_output=True, text=True)
        assert mismatch.returncode != 0 and "checksum" in mismatch.stderr
        source_archive.write_bytes(original_bytes)
        damaged = backup / stored
        with damaged.open("ab") as out:
            out.write(b"damage")
        bad = subprocess.run([sys.executable, BACKUP_TOOL, "restore", backup, "--data-dir", data],
                             capture_output=True, text=True)
        assert bad.returncode != 0 and "damaged" in bad.stderr
        with damaged.open("rb+") as out:
            out.truncate(out.seek(0, 2) - len(b"damage"))
        (data / stored).unlink()
        incomplete = subprocess.run([sys.executable, BACKUP_TOOL, "backup", root / "incomplete",
                                     "--data-dir", data], capture_output=True, text=True)
        assert incomplete.returncode != 0 and "missing" in incomplete.stderr
        subprocess.run([sys.executable, BACKUP_TOOL, "restore", backup, "--data-dir", data], check=True)
        assert (data / stored).is_file()
        with sqlite3.connect(data / "warc-studio.sqlite3") as db:
            assert db.execute("SELECT status FROM capture WHERE id = ?", (capture_id,)).fetchone() == ("archived",)
        print("Integration: crawl, WARC upload/replay, upload limit, backup/restore passed")


def http_status(url, body=None, headers=None):
    try:
        return request(url, body, headers)[0]
    except urllib.error.HTTPError as exc:
        return exc.code


def available(url):
    try:
        return request(url)[0] == 200
    except (OSError, urllib.error.URLError):
        return False


def archived_row(db, source):
    row = capture_row(db, source)
    if row and row[1] == "archived":
        return row
    if row and row[1] == "failed":
        raise AssertionError(f"Capture failed: {row}")
    return None


if __name__ == "__main__":
    run()
