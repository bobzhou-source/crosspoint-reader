from __future__ import annotations

import hashlib
import http.client
import json
import select
import socket
import struct
import time
import uuid
import zipfile
from dataclasses import asdict, dataclass
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlencode, urlsplit

from .common import USER_AGENT, ReaderError, filename

PORT = 53317
GROUP = "224.0.0.167"
PREFIX = "/api/localsend/v2/"
MAX_TRANSFER = 256 * 1024 * 1024
MAX_RESPONSE = 64 * 1024


def sender_info(port: int = PORT, fingerprint: str | None = None) -> dict:
    return {"alias": "Reader Workbench", "version": "2.0", "deviceModel": "reader-send",
            "deviceType": "desktop", "fingerprint": fingerprint or uuid.uuid4().hex,
            "port": port, "protocol": "http", "download": False}


@dataclass
class Device:
    host: str
    port: int = PORT
    alias: str = "Reader"
    fingerprint: str = ""
    protocol: str = "http"

    @property
    def address(self) -> str:
        return f"[{self.host}]:{self.port}" if ":" in self.host else f"{self.host}:{self.port}"

    def metadata(self) -> dict:
        return {**asdict(self), "address": self.address}


def endpoint(value: str) -> Device:
    try:
        parsed = urlsplit(value if "://" in value else "http://" + value)
        if (parsed.scheme != "http" or not parsed.hostname or parsed.username is not None
                or parsed.password is not None or parsed.path not in ("", "/") or parsed.query or parsed.fragment
                or any(ord(c) < 33 or ord(c) == 127 for c in value)):
            raise ValueError("expected HTTP host:port (use [IPv6]:port for IPv6)")
        port = parsed.port if parsed.port is not None else PORT
        if not 1 <= port <= 65535:
            raise ValueError("port must be between 1 and 65535")
        return Device(parsed.hostname, port)
    except ValueError as exc:
        raise ReaderError(f"Invalid --reader: {exc}") from exc


def announcement(data: dict, host: str, own_fingerprint: str = "") -> Device | None:
    if not isinstance(data, dict) or not isinstance(data.get("version"), str) or data["version"].split(".")[0] != "2":
        return None
    fingerprint = data.get("fingerprint", "")
    if not isinstance(fingerprint, str) or not fingerprint or fingerprint == own_fingerprint:
        return None
    port = data.get("port", PORT)
    if type(port) is not int or not 1 <= port <= 65535 or data.get("protocol", "http") not in ("http", "https"):
        return None
    alias = data.get("alias", "Reader")
    if not isinstance(alias, str):
        return None
    return Device(host, port, alias[:128], fingerprint, data.get("protocol", "http"))


def discover(seconds: float = 3, interface: str = "0.0.0.0") -> list[Device]:
    """Receive both multicast fallback replies and the protocol's HTTP register replies."""
    devices: dict[tuple[str, int], Device] = {}
    info = sender_info()
    def remember(data, host):
        device = announcement(data, host, info["fingerprint"])
        if device:
            devices[(device.host, device.port)] = device
    class Register(BaseHTTPRequestHandler):
        def setup(self):
            self.request.settimeout(0.5)
            super().setup()

        def log_message(self, *_):
            pass

        def do_POST(self):
            if self.path != PREFIX + "register":
                self.send_error(404)
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 8192 or self.headers.get("Transfer-Encoding"):
                    self.send_error(400)
                    return
                data = json.loads(self.rfile.read(length))
                remember(data, self.client_address[0])
                body = json.dumps(info).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except (ValueError, OSError):
                self.close_connection = True
    class DiscoveryServer(HTTPServer):
        def handle_error(self, request, client_address):
            pass  # Untrusted discovery clients must not print server tracebacks.
    try:
        socket.inet_aton(interface)
        with DiscoveryServer((interface, 0), Register) as server, socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as udp:
            info["port"] = server.server_address[1]
            server.timeout = 0.2
            udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            udp.bind(("", PORT))
            udp.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(GROUP) + socket.inet_aton(interface))
            udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, struct.pack("b", 1))
            if interface != "0.0.0.0":
                udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(interface))
            packet = json.dumps({**info, "announce": True}).encode()
            udp.sendto(packet, (GROUP, PORT))
            deadline = time.monotonic() + seconds
            repeated = False
            while time.monotonic() < deadline:
                remaining = deadline - time.monotonic()
                if not repeated and remaining <= seconds / 2:
                    udp.sendto(packet, (GROUP, PORT))
                    repeated = True
                ready, _, _ = select.select([udp, server], [], [], min(0.2, max(0, remaining)))
                if server in ready:
                    server.handle_request()
                if udp in ready:
                    raw, peer = udp.recvfrom(8193)
                    if len(raw) <= 8192:
                        try:
                            remember(json.loads(raw), peer[0])
                        except (ValueError, UnicodeError):
                            pass
    except OSError as exc:
        raise ReaderError(f"LocalSend discovery failed: {exc}; use --reader HOST:53317 (or --interface LAN_IP)") from exc
    return sorted(devices.values(), key=lambda d: (d.alias, d.host, d.port))


def request(device: Device, method: str, route: str, body=None, *, content_type: str = "application/json",
            length: int | None = None, timeout: float = 20) -> tuple[int, bytes]:
    if device.protocol != "http":
        raise ReaderError("This companion targets the reader's HTTP LocalSend receiver, not HTTPS/self-signed LocalSend apps")
    connection = http.client.HTTPConnection(device.host, device.port, timeout=timeout)
    headers = {"User-Agent": USER_AGENT, "Connection": "close"}
    if body is not None:
        headers["Content-Type"] = content_type
        headers["Content-Length"] = str(length if length is not None else len(body))
    try:
        connection.request(method, PREFIX + route, body=body, headers=headers)
        response = connection.getresponse()
        raw = response.read(MAX_RESPONSE + 1)
        if len(raw) > MAX_RESPONSE:
            raise ReaderError("Reader returned an oversized protocol response")
        return response.status, raw
    except (OSError, http.client.HTTPException) as exc:
        raise ReaderError(f"LocalSend {route.split('?')[0]} failed at {device.address}: {exc}") from exc
    finally:
        connection.close()


def require_success(status: int, action: str) -> None:
    if status in (200, 204):
        return
    meanings = {400: "invalid request", 401: "PIN required (this reader companion does not use PIN authentication)",
                403: "rejected or invalid session token", 404: "LocalSend v2 endpoint not found; open LocalSend on the reader",
                409: "reader busy with another transfer", 413: "file too large", 422: "SHA-256 checksum mismatch",
                429: "reader rate limit", 500: "reader storage/protocol error"}
    raise ReaderError(f"LocalSend {action}: HTTP {status}: {meanings.get(status, 'unexpected response')}")


def json_response(raw: bytes) -> dict:
    try:
        value = json.loads(raw)
        if not isinstance(value, dict):
            raise ValueError("expected an object")
        return value
    except (ValueError, UnicodeError) as exc:
        raise ReaderError("Reader returned invalid LocalSend JSON") from exc


def send(path: Path, device: Device | None, *, timeout: float = 30, dry_run: bool = False) -> dict:
    path = path.expanduser()
    if path.suffix.lower() != ".epub":
        raise ReaderError("send accepts an EPUB path or cached article ID; fetch/convert HTML first")
    with path.open("rb") as stream:
        stream.seek(0, 2)
        size = stream.tell()
        if not 0 < size <= MAX_TRANSFER:
            raise ReaderError("EPUB must be nonempty and at most 256 MiB")
        stream.seek(0)
        try:
            with zipfile.ZipFile(stream) as archive:
                mimetype = archive.getinfo("mimetype")
                if mimetype.file_size != 20 or archive.read(mimetype) != b"application/epub+zip":
                    raise ReaderError("Not an EPUB: invalid mimetype entry")
        except (zipfile.BadZipFile, KeyError) as exc:
            raise ReaderError("Not an EPUB ZIP with a mimetype entry") from exc
        stream.seek(0)
        sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
        stream.seek(0)
        safe_name = filename(path.stem, sha256)
        # Generated companion names already meet the receiver's 128-byte limit.
        if len(path.name) <= 100 and all(c in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_." for c in path.name) and not path.name.startswith("."):
            safe_name = path.name
        file_id = sha256[:32]
        file_info = {"id": file_id, "fileName": safe_name, "size": size, "fileType": "application/epub+zip", "sha256": sha256}
        summary = {"file": str(path), "fileName": safe_name, "size": size, "sha256": sha256,
                   "reader": device.address if device else "discover at send time", "dry_run": dry_run}
        if dry_run:
            return summary
        if device is None:
            raise ReaderError("No reader selected")
        status, raw = request(device, "GET", "info", timeout=timeout)
        require_success(status, "info")
        info = json_response(raw)
        if not str(info.get("version", "")).startswith("2."):
            raise ReaderError("Reader does not advertise LocalSend protocol v2")
        body = json.dumps({"info": sender_info(), "files": {file_id: file_info}}, separators=(",", ":")).encode()
        try:
            status, raw = request(device, "POST", "prepare-upload", body, timeout=timeout)
        except ReaderError as exc:
            raise ReaderError(f"{exc}; preparation state is unknown. Wait for the reader session to expire before retrying.") from exc
        require_success(status, "prepare-upload")
        if status == 204:
            return {**summary, "status": "already-present"}
        prepared = json_response(raw)
        session = prepared.get("sessionId")
        tokens = prepared.get("files")
        if not isinstance(session, str) or not session or len(session) > 256:
            raise ReaderError("Reader returned no valid sessionId")
        try:
            token = tokens.get(file_id) if isinstance(tokens, dict) else None
            if not isinstance(token, str) or not token or len(token) > 256:
                raise ReaderError("Reader did not accept the EPUB")
            query = urlencode({"sessionId": session, "fileId": file_id, "token": token})
            status, _ = request(device, "POST", "upload?" + query, stream, content_type="application/octet-stream", length=size, timeout=timeout)
            require_success(status, "upload")
        except (ReaderError, KeyboardInterrupt) as exc:
            try:
                request(device, "POST", "cancel?" + urlencode({"sessionId": session}), b"", timeout=min(timeout, 3))
            except ReaderError:
                pass
            if isinstance(exc, KeyboardInterrupt):
                raise
            raise ReaderError(f"{exc}; no automatic upload retry was made. Check /received on the reader before retrying.") from exc
        return {**summary, "status": "sent", "alias": info.get("alias", device.alias)}
