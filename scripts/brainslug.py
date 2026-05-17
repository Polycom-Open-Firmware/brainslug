"""brainslug.py — Python object map for the brainslug debug-probe API.

One module to replace the hand-rolled curl/WS framing in catch_uboot.py,
brainslug-ota, harness/brainslug_serial.py, onboard.sh, etc.

Design goals
------------
* Stdlib only on the default path (vanilla Debian python3 — no pip).
* Async-friendly. Hot path is the per-UART WebSocket (full-duplex), exposed
  as an asyncio object. Sync paths wrap the asyncio loop or use plain
  urllib.request for one-shot REST calls.
* Optional `websockets` / `httpx` paths if installed — light feature
  detection only, never required.
* Clean errors. `BrainslugError` hierarchy. HTTP non-2xx -> HttpError with
  status + body. WS close -> WsClosed. Reboot mid-call -> Rebooting (the
  caller chooses whether to wait).
* Discoverable. `Brainslug.find()` walks a small list of known hosts
  (LAB.md: 192.168.10.95, 10.99.0.35, env BRAINSLUG_HOST, the hostname
  `brainslug.local`) and returns the first that answers /info. No mDNS
  responder runs on the slug today — fall back to plain DNS / static IPs.

Coverage (from brainslug/main/http_api.c + camera.c + audio.c)
--------------------------------------------------------------
GET  /                       -> embedded index.html (not wrapped here)
GET  /info                   -> info()
GET  /gpio?pin=N             -> gpio_read(pin)
POST /gpio                   -> gpio_write(pin, mode=?, level=?)
GET  /net                    -> net()
POST /net                    -> net_set(mode, ip, netmask, gw, dns, hostname)
POST /ota                    -> ota(blob)
POST /reboot                 -> reboot()
GET  /uart/N/config          -> uart(N).config()
POST /uart/N/config          -> uart(N).reconfigure(**fields)
POST /uart/N/write           -> uart(N).write(bytes)
GET  /uart/N/read            -> uart(N).read(max=, timeout_ms=)
GET  /uart/N/ws              -> async with uart(N).ws() as ws: ...
POST /uart/N/swap            -> uart(N).swap_pins()
POST /uart/N/break?ms=N      -> uart(N).send_break(ms=)
POST /uart/N/invert?rx=&tx=  -> uart(N).invert(rx=, tx=)
GET  /camera/info            -> camera_info()
GET  /camera/snapshot        -> camera_snapshot() -> JPEG bytes
GET  /camera/stream          -> (multipart/x-mixed-replace; not wrapped — out of scope)
POST /camera/controls        -> camera_controls(**kw)
GET  /audio/ws               -> async with audio_ws() as ws: ... (FFT frames; not wrapped — out of scope here)
"""

from __future__ import annotations

import asyncio
import base64
import contextlib
import json
import os
import socket
import struct
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import AsyncIterator, Iterable, Optional
from urllib.parse import urlparse


# ----- known hosts ----------------------------------------------------------

# Slugs the lab actually has on the network. Order = preference. Override
# via BRAINSLUG_HOST or by passing host= explicitly.
DEFAULT_HOSTS = (
    "10.99.0.35",        # test VLAN brainslug (current TC8 UART)
    "192.168.10.95",     # production VLAN brainslug
    "brainslug.local",   # plain DNS (no mDNS responder yet)
)


# ----- errors ---------------------------------------------------------------

class BrainslugError(Exception):
    """Base."""

class HttpError(BrainslugError):
    def __init__(self, status: int, path: str, body: bytes = b""):
        super().__init__(f"HTTP {status} {path}: {body[:200]!r}")
        self.status, self.path, self.body = status, path, body

class NotFoundError(BrainslugError):
    """find() exhausted the host list."""

class Rebooting(BrainslugError):
    """The slug acknowledged a reboot and dropped the conn."""

class WsClosed(BrainslugError):
    """Peer closed the WebSocket."""

class WrongBoard(BrainslugError):
    """`board=` field on /info didn't match the expected variant."""


# ----- frames / dataclasses -------------------------------------------------

@dataclass
class UartConfig:
    port: int
    baud: int
    data: int
    parity: int
    stop: int
    tx_gpio: int
    rx_gpio: int

    @classmethod
    def from_json(cls, j: dict) -> "UartConfig":
        return cls(
            port=j["port"], baud=j["baud"], data=j["data"], parity=j["parity"],
            stop=j["stop"], tx_gpio=j["tx_gpio"], rx_gpio=j["rx_gpio"],
        )


@dataclass
class Info:
    app: str
    version: str
    board: str           # "wesp32" | "s3-poe-eth-cam"
    uart_ports: int
    camera: bool
    running_partition: str
    idf: str
    build: str

    @classmethod
    def from_json(cls, j: dict) -> "Info":
        return cls(
            app=j.get("app", ""),
            version=j.get("version", ""),
            board=j.get("board", ""),
            uart_ports=int(j.get("uart_ports", 0)),
            camera=bool(j.get("camera", False)),
            running_partition=j.get("running_partition", ""),
            idf=j.get("idf", ""),
            build=j.get("build", ""),
        )


# ----- low-level HTTP -------------------------------------------------------

def _request(url: str, *, method: str = "GET", body: bytes | None = None,
             content_type: Optional[str] = None, timeout: float = 5.0
             ) -> tuple[int, dict, bytes]:
    headers = {}
    if body is not None and content_type:
        headers["Content-Type"] = content_type
    req = urllib.request.Request(url, data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers or {}), e.read() or b""
    except (urllib.error.URLError, TimeoutError, socket.timeout, ConnectionError) as e:
        # Reboot / network drop. Surface as transport error; caller may catch.
        raise BrainslugError(f"{method} {url}: {e}") from e


def _json_call(base: str, path: str, *, method: str = "GET",
               body: Optional[dict] = None, timeout: float = 5.0) -> dict:
    payload = json.dumps(body).encode() if body is not None else None
    ct = "application/json" if payload is not None else None
    status, _hdrs, raw = _request(base + path, method=method, body=payload,
                                  content_type=ct, timeout=timeout)
    if not 200 <= status < 300:
        raise HttpError(status, path, raw)
    if not raw:
        return {}
    try:
        return json.loads(raw)
    except json.JSONDecodeError as e:
        raise BrainslugError(f"non-JSON from {path}: {raw[:200]!r}") from e


# ----- stdlib WebSocket client (RFC 6455, minimal) --------------------------
#
# Frame format:
#   byte0: FIN(1) | RSV(3) | opcode(4)
#   byte1: MASK(1) | len(7) — if 126: u16 follows; if 127: u64
#   [mask 4B if masked] [payload]
#
# We always mask client->server (required by RFC). Server->client frames are
# never masked. Control frames (close/ping/pong) are handled inline; ping is
# auto-ponged by ESP-IDF's httpd, so we just ignore them on RX.

_WS_FIN  = 0x80
_WS_TXT  = 0x1
_WS_BIN  = 0x2
_WS_CLOSE = 0x8
_WS_PING = 0x9
_WS_PONG = 0xA


class StdlibWebSocket:
    """Minimal asyncio-friendly WS client. Use via Brainslug.uart(n).ws()."""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self._r, self._w = reader, writer
        self._closed = False

    @classmethod
    async def connect(cls, url: str, *, timeout: float = 5.0) -> "StdlibWebSocket":
        u = urlparse(url)
        host, port = u.hostname, u.port or 80
        if u.scheme not in ("ws", "http"):
            raise BrainslugError(f"only ws:///http:// supported, got {u.scheme}")
        path = u.path or "/"
        if u.query:
            path += "?" + u.query
        key = base64.b64encode(os.urandom(16)).decode()
        try:
            r, w = await asyncio.wait_for(
                asyncio.open_connection(host, port), timeout=timeout)
        except (OSError, asyncio.TimeoutError) as e:
            raise BrainslugError(f"ws connect {host}:{port}: {e}") from e
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        ).encode()
        w.write(req); await w.drain()
        # Read headers — exactly one CRLF*2 separator.
        head = bytearray()
        while b"\r\n\r\n" not in head:
            try:
                chunk = await asyncio.wait_for(r.read(4096), timeout=timeout)
            except asyncio.TimeoutError as e:
                raise BrainslugError("ws handshake timeout") from e
            if not chunk:
                raise BrainslugError("ws handshake: peer closed")
            head.extend(chunk)
        status_line = bytes(head).split(b"\r\n", 1)[0]
        if b"101" not in status_line:
            raise BrainslugError(f"ws handshake: {status_line!r}")
        return cls(r, w)

    async def send(self, payload: bytes | str, *, text: bool = False) -> None:
        if isinstance(payload, str):
            payload = payload.encode(); text = True
        opcode = _WS_TXT if text else _WS_BIN
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        n = len(payload)
        if n < 126:
            hdr = struct.pack("!BB", _WS_FIN | opcode, 0x80 | n)
        elif n < 65536:
            hdr = struct.pack("!BBH", _WS_FIN | opcode, 0x80 | 126, n)
        else:
            hdr = struct.pack("!BBQ", _WS_FIN | opcode, 0x80 | 127, n)
        self._w.write(hdr + mask + masked)
        await self._w.drain()

    async def recv(self) -> bytes:
        """Return the next data frame's payload. Ignores ping/pong, raises on close."""
        while True:
            hdr = await self._readn(2)
            b1, b2 = hdr[0], hdr[1]
            opcode = b1 & 0x0F
            plen = b2 & 0x7F
            if plen == 126:
                plen = struct.unpack("!H", await self._readn(2))[0]
            elif plen == 127:
                plen = struct.unpack("!Q", await self._readn(8))[0]
            masked = bool(b2 & 0x80)
            mask = await self._readn(4) if masked else b""
            payload = await self._readn(plen) if plen else b""
            if masked:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            if opcode == _WS_CLOSE:
                self._closed = True
                raise WsClosed("peer closed")
            if opcode in (_WS_PING, _WS_PONG):
                continue
            if opcode in (_WS_BIN, _WS_TXT, 0x0):
                return payload
            # Unknown opcode — ignore.

    async def _readn(self, n: int) -> bytes:
        if n == 0:
            return b""
        try:
            data = await self._r.readexactly(n)
        except asyncio.IncompleteReadError as e:
            self._closed = True
            raise WsClosed(f"short read: wanted {n}, got {len(e.partial)}") from e
        return data

    async def close(self) -> None:
        if self._closed:
            return
        with contextlib.suppress(Exception):
            # opcode 0x8, empty payload, masked
            mask = os.urandom(4)
            self._w.write(struct.pack("!BB", _WS_FIN | _WS_CLOSE, 0x80) + mask)
            await self._w.drain()
        self._closed = True
        with contextlib.suppress(Exception):
            self._w.close()
            await self._w.wait_closed()


# ----- UART port wrapper ----------------------------------------------------

class UartPort:
    def __init__(self, slug: "Brainslug", port: int):
        self.slug = slug
        self.port = port
        self._base = f"{slug.base_url}/uart/{port}"

    # ---- REST polling path ----
    def config(self) -> UartConfig:
        return UartConfig.from_json(_json_call(self.slug.base_url, f"/uart/{self.port}/config"))

    def reconfigure(self, *, baud: Optional[int] = None, data: Optional[int] = None,
                    parity: Optional[int] = None, stop: Optional[int] = None,
                    tx_gpio: Optional[int] = None, rx_gpio: Optional[int] = None
                    ) -> UartConfig:
        body = {k: v for k, v in dict(baud=baud, data=data, parity=parity, stop=stop,
                                      tx_gpio=tx_gpio, rx_gpio=rx_gpio).items()
                if v is not None}
        return UartConfig.from_json(_json_call(
            self.slug.base_url, f"/uart/{self.port}/config", method="POST", body=body))

    def write(self, data: bytes) -> int:
        if not data:
            return 0
        status, _h, body = _request(
            f"{self._base}/write", method="POST", body=bytes(data),
            content_type="application/octet-stream", timeout=self.slug.timeout)
        if not 200 <= status < 300:
            raise HttpError(status, f"/uart/{self.port}/write", body)
        return int(json.loads(body or b"{}").get("written", 0))

    def read(self, *, max_bytes: int = 1024, timeout_ms: int = 100) -> bytes:
        url = f"{self._base}/read?max={int(max_bytes)}&timeout_ms={int(timeout_ms)}"
        http_to = max(self.slug.timeout, timeout_ms / 1000 + 2)
        status, _h, body = _request(url, timeout=http_to)
        if not 200 <= status < 300:
            raise HttpError(status, f"/uart/{self.port}/read", body)
        return body

    def swap_pins(self) -> UartConfig:
        return UartConfig.from_json(_json_call(
            self.slug.base_url, f"/uart/{self.port}/swap", method="POST"))

    def send_break(self, ms: int = 250) -> None:
        _json_call(self.slug.base_url, f"/uart/{self.port}/break?ms={int(ms)}", method="POST")

    def invert(self, *, rx: bool = False, tx: bool = False) -> None:
        _json_call(self.slug.base_url,
                   f"/uart/{self.port}/invert?rx={int(rx)}&tx={int(tx)}",
                   method="POST")

    # ---- WS full-duplex path ----
    def ws_url(self) -> str:
        u = urlparse(self.slug.base_url)
        scheme = "ws" if u.scheme == "http" else "wss"
        return f"{scheme}://{u.netloc}/uart/{self.port}/ws"

    @contextlib.asynccontextmanager
    async def ws(self, *, timeout: float = 5.0) -> AsyncIterator[StdlibWebSocket]:
        """Async context manager — full-duplex UART stream.

        Usage:
            async with slug.uart(1).ws() as ws:
                await ws.send(b'\\x03 \\r')
                rx = await ws.recv()
        """
        sock = await StdlibWebSocket.connect(self.ws_url(), timeout=timeout)
        try:
            yield sock
        finally:
            await sock.close()


# ----- top-level Brainslug --------------------------------------------------

class Brainslug:
    """Client for one brainslug debug probe.

    Use `Brainslug(host)` for a known IP, or `Brainslug.find()` to scan the
    known-host list. All methods that touch the network share `timeout`.
    """

    def __init__(self, host_or_url: str, *, timeout: float = 5.0,
                 expect_board: Optional[str] = None):
        if "://" in host_or_url:
            self.base_url = host_or_url.rstrip("/")
        else:
            self.base_url = f"http://{host_or_url.rstrip('/')}"
        self.timeout = timeout
        self._info: Optional[Info] = None
        self._expect_board = expect_board

    # ---- discovery ----
    @classmethod
    def find(cls, *, hosts: Optional[Iterable[str]] = None, timeout: float = 1.5,
             expect_board: Optional[str] = None) -> "Brainslug":
        """Probe known hosts; return the first that answers /info."""
        env_host = os.environ.get("BRAINSLUG_HOST")
        candidates: list[str] = []
        if env_host:
            candidates.append(env_host)
        candidates.extend(hosts if hosts is not None else DEFAULT_HOSTS)
        last_err: Optional[Exception] = None
        for h in candidates:
            try:
                slug = cls(h, timeout=timeout, expect_board=expect_board)
                slug.info()  # raises on miss
                return slug
            except Exception as e:
                last_err = e
                continue
        raise NotFoundError(f"no brainslug found in {candidates}: {last_err}")

    # ---- /info ----
    def info(self, *, refresh: bool = False) -> Info:
        if self._info is None or refresh:
            j = _json_call(self.base_url, "/info", timeout=self.timeout)
            self._info = Info.from_json(j)
            if self._expect_board and self._info.board != self._expect_board:
                raise WrongBoard(
                    f"expected board={self._expect_board!r}, got {self._info.board!r}")
        return self._info

    # ---- /uart/N ----
    def uart(self, port: int = 1) -> UartPort:
        return UartPort(self, port)

    # ---- /gpio ----
    def gpio_read(self, pin: int) -> int:
        j = _json_call(self.base_url, f"/gpio?pin={int(pin)}", timeout=self.timeout)
        return int(j["level"])

    def gpio_write(self, pin: int, *, mode: Optional[str] = None,
                   level: Optional[int] = None) -> int:
        body: dict = {"pin": int(pin)}
        if mode is not None:   body["mode"] = mode
        if level is not None:  body["level"] = int(level)
        j = _json_call(self.base_url, "/gpio", method="POST", body=body,
                       timeout=self.timeout)
        return int(j["level"])

    # ---- /net ----
    def net(self) -> dict:
        return _json_call(self.base_url, "/net", timeout=self.timeout)

    def net_set(self, *, mode: str = "dhcp", ip: str = "", netmask: str = "",
                gw: str = "", dns: str = "", hostname: str = "") -> dict:
        body = {"mode": mode, "ip": ip, "netmask": netmask, "gw": gw,
                "dns": dns, "hostname": hostname}
        return _json_call(self.base_url, "/net", method="POST", body=body,
                          timeout=self.timeout)

    # ---- /ota ----
    def ota(self, blob: bytes, *, wait: bool = True, wait_timeout: float = 60.0
            ) -> dict:
        """Upload firmware, optionally block until the slug comes back."""
        status, _h, body = _request(
            f"{self.base_url}/ota", method="POST", body=blob,
            content_type="application/octet-stream",
            timeout=max(self.timeout, 120.0))
        if not 200 <= status < 300:
            raise HttpError(status, "/ota", body)
        result = json.loads(body or b"{}")
        if wait:
            self._wait_back_online(wait_timeout)
            self._info = None  # version may have changed; refresh on next info()
        return result

    # ---- /reboot ----
    def reboot(self, *, wait: bool = True, wait_timeout: float = 30.0) -> None:
        try:
            _request(f"{self.base_url}/reboot", method="POST", timeout=self.timeout)
        except BrainslugError:
            # Slug may drop the conn before responding — that's fine.
            pass
        if wait:
            self._wait_back_online(wait_timeout)
            self._info = None

    def _wait_back_online(self, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        # Give it a moment to actually reboot.
        time.sleep(0.5)
        last: Optional[Exception] = None
        while time.monotonic() < deadline:
            try:
                _json_call(self.base_url, "/info", timeout=2.0)
                return
            except Exception as e:
                last = e
                time.sleep(1.0)
        raise Rebooting(f"slug didn't return within {timeout}s: {last}")

    # ---- /camera ----
    def camera_info(self) -> dict:
        return _json_call(self.base_url, "/camera/info", timeout=self.timeout)

    def camera_snapshot(self) -> bytes:
        status, _h, body = _request(f"{self.base_url}/camera/snapshot",
                                    timeout=self.timeout)
        if not 200 <= status < 300:
            raise HttpError(status, "/camera/snapshot", body)
        return body

    def camera_controls(self, **fields) -> dict:
        return _json_call(self.base_url, "/camera/controls",
                          method="POST", body=fields, timeout=self.timeout)

    # ---- ergonomics ----
    def __repr__(self) -> str:
        return f"Brainslug({self.base_url!r})"


# ----- CLI smoke test -------------------------------------------------------

if __name__ == "__main__":
    import sys
    host = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("BRAINSLUG_HOST", "10.99.0.35")
    s = Brainslug(host)
    print(s.info())
    print(s.uart(1).config())
