#!/usr/bin/env python3
"""Exercise production SecureClient/SecureHttpClient with real local TLS.

Requires a C/C++ compiler and Python cryptography. wolfSSL comes from
PlatformIO's pinned Arduino-wolfSSL dependency, not a mock or system TLS client.
Run after `pio pkg install -e x4pro`: python3 test/tls_verification/run.py
All generated keys, certificates, binaries and objects live in a temp directory.
"""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import ssl
import subprocess
import tempfile
import threading

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SECURE = ROOT / "freeink-sdk/libs/network/SecureNet"
WOLF = ROOT / ".pio/libdeps/x4pro/Arduino-wolfSSL/src"


def run(*args):
    subprocess.run([str(x) for x in args], check=True)


def certificate(key, issuer_key, issuer, common_name, san=None, expired=False, ca=False):
    now = datetime.now(timezone.utc)
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])
    builder = (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer or subject)
               .public_key(key.public_key()).serial_number(x509.random_serial_number())
               .not_valid_before(now - timedelta(days=2))
               .not_valid_after(now + timedelta(days=-1 if expired else 7))
               .add_extension(x509.BasicConstraints(ca=ca, path_length=0 if ca else None), critical=True)
               .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False))
    if san:
        builder = builder.add_extension(x509.SubjectAlternativeName([x509.DNSName(san)]), critical=False)
        builder = builder.add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
    return builder.sign(issuer_key, hashes.SHA256())


with tempfile.TemporaryDirectory(prefix="reader-tls-") as scratch:
    work = Path(scratch)
    if not WOLF.is_dir():
        raise SystemExit("Install the pinned x4pro PlatformIO dependencies first")
    run("python3", SECURE / "tools/build_ca_bundle.py", SECURE / "certs/public-roots.pem",
        work / "TrustAnchors.generated.h")
    source = (SECURE / "certs/public-roots.pem").read_text()
    corrupt = source.replace(source.split("# SHA256: ")[1][:64], "0" * 64, 1)
    for name, content in [("empty", ""), ("corrupt", corrupt)]:
        invalid = work / (name + ".pem")
        invalid.write_text(content)
        result = subprocess.run(["python3", str(SECURE / "tools/build_ca_bundle.py"), str(invalid),
                                 str(work / "invalid.generated.h")], capture_output=True)
        assert result.returncode != 0 and not (work / "invalid.generated.h").exists()
    print("PASS: empty/corrupt trust bundles rejected at generation", flush=True)
    sources = sorted((WOLF / "src").glob("*.c")) + sorted((WOLF / "wolfcrypt/src").glob("*.c"))
    flags = ["-O1", "-DWOLFSSL_USER_SETTINGS", "-DWOLFSSL_OPTIONS_H", "-I" + str(HERE / "stubs"), "-I" + str(WOLF)]

    def compile_source(item):
        index, source = item
        target = work / f"wolf-{index}.o"
        run(os.environ.get("CC", "cc"), *flags, "-c", source, "-o", target)
        return target

    with ThreadPoolExecutor(max_workers=4) as pool:
        objects = list(pool.map(compile_source, enumerate(sources)))
    binary = work / "client"
    run(os.environ.get("CXX", "c++"), "-std=c++17", *flags, "-DFREEINK_NET_WOLFSSL=1",
        "-I" + str(SECURE / "include"), "-I" + str(work), HERE / "client.cpp",
        SECURE / "src/SecureClient.cpp", *objects, "-o", binary)

    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca = certificate(ca_key, ca_key, None, "Local regression CA", ca=True)
    ca_path = work / "root.pem"
    ca_path.write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    key_path = work / "leaf.key"
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                          serialization.NoEncryption()))
    paths = []
    for name, san, expired in [("good", "localhost", False), ("expired", "localhost", True),
                               ("mismatch", "other.example", False)]:
        path = work / (name + ".pem")
        cert = certificate(key, ca_key, ca.subject, "localhost", san=san, expired=expired)
        path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        paths.append(path)

    plaintext_paths = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if not isinstance(self.connection, ssl.SSLSocket):
                plaintext_paths.append(self.path)
            if self.path == "/downgrade":
                self.send_response(302)
                self.send_header("Location", f"hTtP://localhost:{plain.server_port}/leaked")
                body = b""
            else:
                self.send_response(200)
                body = b"local" if self.path == "/local" else b"verified"
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def server(context=None):
        result = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        if context:
            result.socket = context.wrap_socket(result.socket, server_side=True)
        threading.Thread(target=result.serve_forever, daemon=True).start()
        return result

    plain = server()
    try:
        for version in [ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3]:
            servers = []
            try:
                for cert_path in paths:
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.minimum_version = context.maximum_version = version
                    context.load_cert_chain(cert_path, key_path)
                    servers.append(server(context))
                run(binary, *(s.server_port for s in servers), plain.server_port, ca_path)
                print(f"PASS: {version.name}", flush=True)
            finally:
                for endpoint in servers:
                    endpoint.shutdown()
                    endpoint.server_close()
        assert plaintext_paths == ["/local", "/local"], plaintext_paths
        print("PASS: no plaintext redirect request reached the local endpoint")
    finally:
        plain.shutdown()
        plain.server_close()
