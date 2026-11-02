#!/usr/bin/env python3
"""Build installed Arduino-wolfSSL with firmware crypto flags and host I/O.

No dependencies are fetched. Requires the x4pro-gh_release PlatformIO libdeps.
The executable only performs handshakes or verifies a supplied DER certificate.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--baseline", action="store_true", help="Remove the public-CA hash/P-384 fix")
parser.add_argument("--define", action="append", default=[])
args = parser.parse_args()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
source = ROOT / ".pio/libdeps/x4pro-gh_release/Arduino-wolfSSL/src"
settings = source / "user_settings.h"
if not settings.exists():
    parser.error("Install/build x4pro-gh_release dependencies first")
ini = (ROOT / "platformio.ini").read_text()
base = ini.split("[base]", 1)[1].split("\n[", 1)[0]
flags = re.findall(r"^\s+(-D(?:WOLFSSL|HAVE_|FREEINK_NET_WOLFSSL)[^\s;]+)", base, re.M)
fix = {"-DWOLFSSL_SHA384", "-DWOLFSSL_SHA512", "-DWOLFSSL_SP_384", "-DWOLFSSL_ALT_CERT_CHAINS"}
if args.baseline:
    flags = [flag for flag in flags if flag not in fix]
flags.extend("-D" + value for value in args.define)
# Arduino/ESP32 sets these crypto protections in settings.h. Keep them on host.
# Host substitutes BSD I/O, clock and OS entropy for Arduino/lwIP/esp_random.
# Force portable 32-bit SP/TFM digits to match ESP32-S3, not native arm64 math.
flags += ["-DTFM_TIMING_RESISTANT", "-DECC_TIMING_RESISTANT", "-DWC_RSA_BLINDING",
          "-DWC_NO_CACHE_RESISTANT", "-DNO_WRITEV", "-DNO_WOLFSSL_DIR",
          "-DWOLFSSL_NO_CURRDIR", "-DSIZEOF_LONG_LONG=8", "-DWOLFSSL_USER_IO",
          "-DSP_WORD_SIZE=32", "-DNO_TFM_64BIT", "-DTFM_NO_ASM"]
secure = ROOT / "freeink-sdk/libs/network/SecureNet"
subprocess.run(["python3", str(secure / "tools/build_ca_bundle.py"),
                str(secure / "certs/public-roots.pem"), str(output / "TrustAnchors.generated.h")], check=True)
includes = ["-I" + str(source), "-I" + str(output)]
crypto = sorted((source / "wolfcrypt/src").glob("*.c"))
tls = [source / "src" / (name + ".c") for name in
       ("ssl", "tls", "tls13", "internal", "keys", "wolfio", "ocsp", "crl", "dtls", "dtls13")]
units = crypto + tls

def compile_unit(path):
    target = output / (str(path.relative_to(source)).replace("/", "_") + ".o")
    command = ["cc", "-O2", "-Wno-comment", "-Wno-unused-function", *flags, *includes,
               "-c", str(path), "-o", str(target)]
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(" ".join(command) + "\n" + result.stdout + result.stderr)
    return target

with ThreadPoolExecutor(max_workers=2) as executor:
    objects = list(executor.map(compile_unit, units))
command = ["c++", "-std=c++17", "-O2", *flags, *includes, str(Path(__file__).with_name("probe.cpp")),
           *map(str, objects), "-o", str(output / "probe")]
subprocess.run(command, check=True)
manifest = {
    "flags": flags, "user_settings_sha256": hashlib.sha256(settings.read_bytes()).hexdigest(),
    "roots_sha256": hashlib.sha256((secure / "certs/public-roots.pem").read_bytes()).hexdigest(),
    "source_sha256": {str(path.relative_to(source)): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in units},
}
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(output / "probe")
