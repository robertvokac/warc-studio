"""Optional Chrome smoke check for the page rendered inside ReplayWeb.page."""

import base64
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import time
from urllib.parse import urlsplit
from urllib.request import urlopen


class DevTools:
    """Small WebSocket/CDP client; avoids adding a browser-test dependency."""

    def __init__(self, url):
        parsed = urlsplit(url)
        self.sock = socket.create_connection((parsed.hostname, parsed.port), timeout=10)
        self.sock.settimeout(10)
        key = base64.b64encode(os.urandom(16)).decode()
        path = parsed.path + ("?" + parsed.query if parsed.query else "")
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {parsed.netloc}\r\n"
                           f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                           f"Origin: http://localhost\r\n\r\n").encode())
        response = b""
        while b"\r\n\r\n" not in response:
            response += self.sock.recv(4096)
        if b" 101 " not in response.split(b"\r\n", 1)[0]:
            raise RuntimeError("Chrome rejected the DevTools WebSocket")
        self.buffer = response.split(b"\r\n\r\n", 1)[1]
        self.next_id = 0
        self.contexts = {}

    def close(self):
        self.sock.close()

    def exact(self, count):
        while len(self.buffer) < count:
            chunk = self.sock.recv(max(4096, count - len(self.buffer)))
            if not chunk:
                raise RuntimeError("Chrome closed the DevTools connection")
            self.buffer += chunk
        result, self.buffer = self.buffer[:count], self.buffer[count:]
        return result

    def send(self, payload, opcode=1):
        mask = os.urandom(4)
        size = len(payload)
        header = bytes([0x80 | opcode])
        if size < 126:
            header += bytes([0x80 | size])
        elif size < 65536:
            header += bytes([0x80 | 126]) + struct.pack("!H", size)
        else:
            header += bytes([0x80 | 127]) + struct.pack("!Q", size)
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def receive(self):
        while True:
            first, second = self.exact(2)
            length = second & 0x7F
            if length == 126:
                length = struct.unpack("!H", self.exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self.exact(8))[0]
            mask = self.exact(4) if second & 0x80 else None
            payload = self.exact(length)
            if mask:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            opcode = first & 0x0F
            if opcode == 9:
                self.send(payload, opcode=10)
            elif opcode == 8:
                raise RuntimeError("Chrome closed the DevTools WebSocket")
            elif opcode == 1:
                return json.loads(payload)

    def call(self, method, params=None):
        self.next_id += 1
        request_id = self.next_id
        self.send(json.dumps({"id": request_id, "method": method, "params": params or {}}).encode())
        while True:
            message = self.receive()
            event = message.get("method")
            if event == "Runtime.executionContextCreated":
                context = message["params"]["context"]
                auxiliary = context.get("auxData", {})
                if auxiliary.get("isDefault") and auxiliary.get("frameId"):
                    self.contexts[auxiliary["frameId"]] = context["id"]
            elif event == "Runtime.executionContextDestroyed":
                destroyed = message["params"]["executionContextId"]
                self.contexts = {frame: ident for frame, ident in self.contexts.items() if ident != destroyed}
            elif event == "Runtime.executionContextsCleared":
                self.contexts.clear()
            if message.get("id") == request_id:
                if "error" in message:
                    raise RuntimeError(f"Chrome DevTools error: {message['error']}")
                return message["result"]


def replay_frame(tree):
    frame = tree["frame"]
    if "/replay/w/" in frame.get("url", ""):
        return frame["id"]
    for child in tree.get("childFrames", []):
        found = replay_frame(child)
        if found:
            return found
    return None


def check_browser_replay(url, expected_text, profile_root):
    chrome = shutil.which("google-chrome") or shutil.which("chromium") or shutil.which("chromium-browser")
    if chrome is None or os.name != "posix":
        print("Chrome unavailable; browser rendering check skipped")
        return
    profile = Path(profile_root) / "chrome-profile"
    profile.mkdir()
    process = subprocess.Popen(
        [chrome, "--headless", "--no-sandbox", "--disable-gpu", "--disable-dev-shm-usage",
         "--no-first-run", "--remote-debugging-port=0", "--remote-allow-origins=*",
         f"--user-data-dir={profile}", "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    client = None
    try:
        active_port = profile / "DevToolsActivePort"
        deadline = time.monotonic() + 30
        while not active_port.exists() and time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError("Chrome exited before opening DevTools")
            time.sleep(0.1)
        if not active_port.exists():
            raise AssertionError("Chrome did not open DevTools")
        port = int(active_port.read_text().splitlines()[0])
        with urlopen(f"http://127.0.0.1:{port}/json/list", timeout=10) as response:
            targets = json.load(response)
        page = next(target for target in targets if target["type"] == "page")
        client = DevTools(page["webSocketDebuggerUrl"])
        client.call("Runtime.enable")
        client.call("Page.navigate", {"url": url})
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            tree = client.call("Page.getFrameTree")["frameTree"]
            frame = replay_frame(tree)
            context = client.contexts.get(frame)
            if context is not None:
                try:
                    result = client.call("Runtime.evaluate", {
                        "expression": "document.body ? document.body.innerText : ''",
                        "contextId": context,
                        "returnByValue": True,
                    })
                except RuntimeError as exc:
                    if "context" in str(exc).lower():
                        continue  # the replay frame navigated while it was being inspected
                    raise
                if expected_text in result.get("result", {}).get("value", ""):
                    print("Chrome rendered the archived page inside ReplayWeb.page")
                    return
            time.sleep(0.25)
        raise AssertionError("Chrome did not render the archived page inside ReplayWeb.page")
    finally:
        if client:
            client.close()
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
