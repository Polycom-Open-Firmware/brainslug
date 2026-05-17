#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "mcp>=1.2",
#   "httpx>=0.27",
# ]
# ///
"""brainslug_mcp — MCP server exposing the brainslug HTTP+WS API as tools.

The brainslug is an ESP32-S3 board (codename `wesp_debug_probe`) wired to a
TC-series panel: it owns the panel's UART, GPIOs, OTA, and a camera/audio
passthrough. This server lets a Claude agent drive the slug as if it were
an in-process library.

Transport: stdio JSON-RPC (the MCP default).

Registration (Claude Code, `~/.claude/settings.json` -> `mcpServers`):

    "brainslug": {
      "command": "/home/alex/polycom_re/brainslug/mcp/brainslug_mcp.py",
      "env": { "BRAINSLUG_URL": "http://10.99.0.35" }
    }

`BRAINSLUG_URL` is the natural default; per-call `brainslug_url` kwargs
override it (e.g. `slug_catch_uboot(brainslug_url="http://10.99.0.36")`).

Tools (v1):
    slug_info, slug_reboot,
    slug_gpio_set, slug_gpio_get,
    slug_uart_config, slug_uart_read, slug_uart_write,
    slug_uart_break, slug_uart_swap,
    slug_ota, slug_catch_uboot.

Deferred (v2):
    Camera capture, MJPEG stream framing, audio playback,
    network reconfig (/net POST). Reason: camera/audio are bigger
    bytestreams that need streaming-MCP-resource semantics rather than
    one-shot tool returns, and `/net` reconfig can lock you out of a
    live target — better as a deliberate CLI step than a tool call.

UART read/OTA responses are JSON-safe: raw bytes are returned both
hex-encoded (`hex` field) and as UTF-8-with-replacement (`text` field)
so the model can see ANSI logs without losing binary fidelity.
"""

from __future__ import annotations

import asyncio
import base64
import logging
import os
import re
import select
import socket
import struct
import sys
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

import httpx
from mcp.server.fastmcp import FastMCP

logging.basicConfig(level=logging.INFO, stream=sys.stderr,
                    format="%(asctime)s %(levelname)s %(name)s %(message)s")
log = logging.getLogger("brainslug-mcp")

DEFAULT_TIMEOUT = 10.0
mcp = FastMCP("brainslug")


def _base_url(override: str | None) -> str:
    url = override or os.environ.get("BRAINSLUG_URL")
    if not url:
        raise ValueError(
            "No brainslug URL: pass brainslug_url=... or set BRAINSLUG_URL env var."
        )
    return url.rstrip("/")


async def _http(method: str, path: str, *, base: str | None = None,
                json_body: Any = None, content: bytes | None = None,
                timeout: float = DEFAULT_TIMEOUT) -> dict[str, Any]:
    """Wrap brainslug HTTP calls so JSON / octet-stream / errors land as
    structured dicts the model can reason about."""
    url = _base_url(base) + path
    async with httpx.AsyncClient(timeout=timeout) as cx:
        try:
            r = await cx.request(method, url, json=json_body, content=content)
        except httpx.HTTPError as e:
            raise RuntimeError(f"brainslug {method} {path}: {e}") from e
    ctype = r.headers.get("content-type", "")
    out: dict[str, Any] = {"status": r.status_code, "url": url}
    if "application/json" in ctype:
        try:
            out["json"] = r.json()
        except Exception:
            out["body_text"] = r.text
    elif "application/octet-stream" in ctype:
        out["bytes_len"] = len(r.content)
        out["hex"] = r.content.hex()
        out["text"] = r.content.decode("utf-8", "replace")
        if (x := r.headers.get("X-UART-Bytes")) is not None:
            out["x_uart_bytes"] = int(x)
    else:
        out["body_text"] = r.text
    if r.status_code >= 400:
        raise RuntimeError(f"brainslug {method} {path} -> HTTP {r.status_code}: "
                           f"{out.get('json') or out.get('body_text')}")
    return out


# ===================== tools =====================

@mcp.tool()
async def slug_info(brainslug_url: str | None = None) -> dict:
    """Return `/info` JSON: app, version, idf, build, board, partition, uart_ports."""
    return (await _http("GET", "/info", base=brainslug_url))["json"]


@mcp.tool()
async def slug_reboot(brainslug_url: str | None = None) -> dict:
    """POST /reboot. Slug restarts ~200 ms after acking; subsequent calls
    will fail for ~3-5 s while the slug reboots."""
    return await _http("POST", "/reboot", base=brainslug_url, timeout=3.0)


@mcp.tool()
async def slug_gpio_get(pin: int, brainslug_url: str | None = None) -> dict:
    """GET /gpio?pin=N. Returns `{pin, level}`."""
    return (await _http("GET", f"/gpio?pin={pin}", base=brainslug_url))["json"]


@mcp.tool()
async def slug_gpio_set(pin: int, mode: str | None = None, level: int | None = None,
                       brainslug_url: str | None = None) -> dict:
    """POST /gpio. `mode` in {out, in, in_pu, in_pd, od}; `level` 0|1.
    Either or both may be set."""
    body: dict[str, Any] = {"pin": pin}
    if mode is not None:
        body["mode"] = mode
    if level is not None:
        body["level"] = int(level)
    return (await _http("POST", "/gpio", base=brainslug_url, json_body=body))["json"]


@mcp.tool()
async def slug_uart_config(port: int, baud: int | None = None,
                           data: int | None = None, parity: int | None = None,
                           stop: int | None = None,
                           tx_gpio: int | None = None, rx_gpio: int | None = None,
                           brainslug_url: str | None = None) -> dict:
    """GET (no kwargs) or POST /uart/<port>/config.
    If any of baud/data/parity/stop/tx_gpio/rx_gpio is provided, a POST is
    issued; otherwise the current config is read. Returns the post-call config."""
    body: dict[str, Any] = {}
    for k, v in (("baud", baud), ("data", data), ("parity", parity),
                 ("stop", stop), ("tx_gpio", tx_gpio), ("rx_gpio", rx_gpio)):
        if v is not None:
            body[k] = int(v)
    path = f"/uart/{port}/config"
    if body:
        return (await _http("POST", path, base=brainslug_url, json_body=body))["json"]
    return (await _http("GET", path, base=brainslug_url))["json"]


@mcp.tool()
async def slug_uart_read(port: int, max: int = 4096, timeout_ms: int = 200,
                         brainslug_url: str | None = None) -> dict:
    """GET /uart/<port>/read?max=&timeout_ms=. Returns
    `{port, bytes_len, hex, text, x_uart_bytes}`. `text` is UTF-8 with
    replacement (handy for u-boot logs); `hex` preserves binary fidelity."""
    path = f"/uart/{port}/read?max={int(max)}&timeout_ms={int(timeout_ms)}"
    r = await _http("GET", path, base=brainslug_url,
                    timeout=max(2.0, timeout_ms / 1000.0 + 2.0))
    return {"port": port, **{k: r[k] for k in ("bytes_len", "hex", "text", "x_uart_bytes")
                              if k in r}}


@mcp.tool()
async def slug_uart_write(port: int, data: str, encoding: str = "utf8",
                          brainslug_url: str | None = None) -> dict:
    """POST /uart/<port>/write. `data` is a string; `encoding` selects how
    we serialize it before sending:
        utf8   - data.encode('utf-8') (default; good for u-boot text)
        hex    - bytes.fromhex(data)
        base64 - base64.b64decode(data)
    Returns `{port, written}`."""
    if encoding == "utf8":
        body = data.encode("utf-8")
    elif encoding == "hex":
        body = bytes.fromhex(data)
    elif encoding == "base64":
        body = base64.b64decode(data)
    else:
        raise ValueError(f"unknown encoding {encoding!r}")
    return (await _http("POST", f"/uart/{port}/write",
                        base=brainslug_url, content=body))["json"]


@mcp.tool()
async def slug_uart_break(port: int, ms: int = 250,
                          brainslug_url: str | None = None) -> dict:
    """POST /uart/<port>/break?ms=N — assert a UART BREAK. Default 250 ms
    (slug clamps internally). Used for sysrq-b style flows."""
    return (await _http("POST", f"/uart/{port}/break?ms={int(ms)}",
                        base=brainslug_url))["json"]


@mcp.tool()
async def slug_uart_swap(port: int, brainslug_url: str | None = None) -> dict:
    """POST /uart/<port>/swap — swap TX/RX GPIO pins. Useful when you've
    wired the harness backwards. Returns the post-swap config."""
    return (await _http("POST", f"/uart/{port}/swap", base=brainslug_url))["json"]


@mcp.tool()
async def slug_ota(firmware_path: str, brainslug_url: str | None = None) -> dict:
    """POST /ota with the contents of `firmware_path` (absolute path, expected
    to be a slug `brainslug.bin`). The slug reboots into the new partition.
    Returns the slug's pre-reboot ack JSON; the slug then drops the TCP
    connection so callers should poll `slug_info` to confirm recovery."""
    p = Path(firmware_path)
    if not p.is_file():
        raise FileNotFoundError(firmware_path)
    data = p.read_bytes()
    log.info("OTA: %s (%d bytes) -> %s", firmware_path, len(data),
             _base_url(brainslug_url))
    try:
        return (await _http("POST", "/ota", base=brainslug_url,
                            content=data, timeout=120.0))["json"]
    except RuntimeError as e:
        # Slug may drop the socket mid-reboot; treat ConnectionError as success.
        msg = str(e).lower()
        if "connect" in msg or "remotedisconnected" in msg or "remoteprotocolerror" in msg:
            return {"ota": "submitted", "note": "slug dropped socket on reboot",
                    "written": len(data)}
        raise


# ===================== catch_uboot (stdlib-only WS) =====================
# Mirrors smoke/catch_uboot.py so the MCP server can install on a host
# without the `websockets` package. Sync code wrapped in run_in_executor.

_PROMPT_RE = re.compile(rb"(u-boot=> |^=> )", re.MULTILINE)


def _ws_connect(url: str, path: str, timeout: float = 5.0) -> tuple[socket.socket, bytes]:
    u = urlparse(url)
    host, port = u.hostname, u.port or 80
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    )
    s.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise RuntimeError("ws handshake: connection closed mid-response")
        buf += chunk
    head, _, leftover = buf.partition(b"\r\n\r\n")
    status_line = head.split(b"\r\n")[0]
    if b"101" not in status_line:
        raise RuntimeError(f"ws handshake failed: {status_line!r}")
    s.setblocking(False)
    return s, leftover


def _ws_send_binary(sock: socket.socket, payload: bytes) -> None:
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    n = len(payload)
    if n < 126:
        header = struct.pack("!BB", 0x82, 0x80 | n) + mask
    elif n < 65536:
        header = struct.pack("!BBH", 0x82, 0x80 | 126, n) + mask
    else:
        header = struct.pack("!BBQ", 0x82, 0x80 | 127, n) + mask
    sock.sendall(header + masked)


def _ws_recv_all(sock: socket.socket, leftover: bytes,
                 max_bytes: int = 65536) -> tuple[bytes, bytes]:
    out = bytearray()
    buf = bytearray(leftover)

    def need(n: int) -> bool:
        while len(buf) < n:
            try:
                chunk = sock.recv(8192)
            except (BlockingIOError, socket.timeout):
                return False
            if not chunk:
                return False
            buf.extend(chunk)
        return True

    while True:
        try:
            chunk = sock.recv(8192)
            if chunk:
                buf.extend(chunk)
            elif not buf:
                break
        except (BlockingIOError, socket.timeout):
            if not buf:
                break
        if len(buf) < 2:
            break
        b1, b2 = buf[0], buf[1]
        plen = b2 & 0x7F
        idx = 2
        if plen == 126:
            if not need(4): break
            plen = struct.unpack("!H", bytes(buf[2:4]))[0]; idx = 4
        elif plen == 127:
            if not need(10): break
            plen = struct.unpack("!Q", bytes(buf[2:10]))[0]; idx = 10
        masked = bool(b2 & 0x80)
        mask = b""
        if masked:
            if not need(idx + 4): break
            mask = bytes(buf[idx:idx + 4]); idx += 4
        if not need(idx + plen): break
        payload = bytes(buf[idx:idx + plen])
        if masked:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        opcode = b1 & 0x0F
        if opcode in (0x1, 0x2, 0x0):
            out.extend(payload)
        elif opcode == 0x8:
            return bytes(out), b""
        buf = buf[idx + plen:]
        if len(out) >= max_bytes:
            break
    return bytes(out), bytes(buf)


def _catch_uboot_sync(brainslug_url: str, port: int, total_timeout: float) -> dict:
    path = f"/uart/{port}/ws"
    sock, leftover = _ws_connect(brainslug_url, path)
    burst = b"\x03 \r" * 8
    rx_buf = bytearray()
    sends = 0
    start = time.monotonic()
    deadline = start + total_timeout
    send_interval = 1.0 / 50
    next_send = time.monotonic()

    try:
        while time.monotonic() < deadline:
            readable, _, _ = select.select([sock], [], [], 0)
            if readable:
                data, leftover = _ws_recv_all(sock, leftover)
                if data:
                    rx_buf.extend(data)
                    if len(rx_buf) > 16384:
                        del rx_buf[:len(rx_buf) - 8192]
                    if _PROMPT_RE.search(rx_buf[-200:]):
                        return {
                            "ok": True,
                            "prompt": True,
                            "bursts": sends,
                            "elapsed_s": round(time.monotonic() - start, 2),
                            "tail_text": bytes(rx_buf[-400:]).decode("utf-8", "replace"),
                        }
            now = time.monotonic()
            if now >= next_send:
                try:
                    _ws_send_binary(sock, burst)
                    sends += 1
                    next_send = now + send_interval
                except BlockingIOError:
                    time.sleep(0.005)
            else:
                time.sleep(min(0.002, next_send - now))
    finally:
        try:
            sock.close()
        except OSError:
            pass

    return {
        "ok": False,
        "prompt": False,
        "bursts": sends,
        "elapsed_s": round(time.monotonic() - start, 2),
        "tail_text": bytes(rx_buf[-400:]).decode("utf-8", "replace"),
    }


@mcp.tool()
async def slug_catch_uboot(brainslug_url: str | None = None, port: int = 1,
                           total_timeout: float = 45.0) -> dict:
    """Spam Ctrl-C over the /uart/<port>/ws WebSocket until the panel u-boot
    prompt appears, or `total_timeout` elapses. Returns `{ok, prompt, bursts,
    elapsed_s, tail_text}`. Use after `slug_reboot` (or an external power
    cycle) to land the panel in u-boot for cmdline work.

    Re-uses the proven stdlib WS implementation from
    `tc8-firmware-build/smoke/catch_uboot.py`."""
    url = _base_url(brainslug_url)
    return await asyncio.get_running_loop().run_in_executor(
        None, _catch_uboot_sync, url, port, total_timeout)


# ===================== entrypoint =====================

def main() -> None:
    log.info("brainslug MCP server starting (default url: %s)",
             os.environ.get("BRAINSLUG_URL", "<unset>"))
    mcp.run()


if __name__ == "__main__":
    main()
