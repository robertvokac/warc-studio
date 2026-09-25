#!/usr/bin/env python3
"""End-to-end crawl, WARC upload/replay routes, concurrency, and backup restore."""

import gzip
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
        fixture = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
        import threading
        fixture_thread = threading.Thread(target=fixture.serve_forever, daemon=True)
        fixture_thread.start()
        url = f"http://127.0.0.1:{fixture.server_port}/page"
        env = os.environ | {
            "WARC_STUDIO_DATA_DIR": str(data),
            "WARC_STUDIO_PORT": str(port),
            "WARC_STUDIO_MAX_REQUEST_MB": "1",
            "WARC_STUDIO_MAX_CONCURRENT_UPLOADS": "1",
        }
        server = subprocess.Popen([str(APP)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        base = f"http://localhost:{port}"
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

            replay = request(base + f"/capture/{capture_id}/replay")[2].decode()
            assert "<replay-web-page" in replay and f"/{stored}" in replay
            assert captured_url.replace("&", "&amp;") in replay
            code, headers, chunk, _ = request(base + "/" + stored, headers={"Range": "bytes=0-31"})
            assert code == 206 and chunk == archive.read_bytes()[:32]
            assert headers["Content-Range"].startswith("bytes 0-31/")
            assert request(base + "/replay/ui.js")[0] == 200
            assert request(base + "/replay/sw.js")[0] == 200

            boundary = "warc-studio-test-boundary"
            upload = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
                      f"filename=\"fixture.warc.gz\"\r\nContent-Type: application/octet-stream\r\n\r\n").encode()
            upload += archive.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
            request(base + "/upload", upload, {"Content-Type": f"multipart/form-data; boundary={boundary}"})
            uploaded = archived_row(data / "warc-studio.sqlite3", "upload")
            assert uploaded and uploaded[3] == url and uploaded[4] == "Fixture Capture"
            assert request(base + f"/capture/{uploaded[0]}/replay")[0] == 200

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

        subprocess.run([sys.executable, BACKUP_TOOL, "backup", backup, "--data-dir", data], check=True)
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
        assert archived_row(data / "warc-studio.sqlite3", "crawl")
        print("Integration: crawl, WARC upload/replay, upload limit, backup/restore passed")


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
