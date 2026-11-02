"""One scheduled pass over recent subscriptions and X Following, then site sync.

Each collector is a separate process so its JSON errors and nonzero exit are
visible without preventing the other collector from running. After collection
the job drains pending shared links, pushes new cache entries through the
private reader site, and pulls reader annotations into an explicit output path.
The management key is read from ``READER_SYNC_KEY`` on every host; only macOS
may fall back to the dedicated Keychain item when that variable is absent. The
reader's restricted device key is never read. SD delivery remains a separate
manual fallback, and firmware is never staged here.
"""

from __future__ import annotations

import json
import os
import plistlib
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

from . import card
from .cache import Cache
from .common import ReaderError, atomic_write

LAUNCHD_LABEL = "dev.reader-workbench.reader-send-subscriptions"
LOGS_NAME = "reader-send-collect"
SUBSCRIPTION_SINCE_DAYS = 7
X_SINCE_HOURS = 24
X_LIMIT = 50
COLLECTOR_TIMEOUT = 20 * 60
CLOUD_ANNOTATIONS_NAME = "cloud-annotations.ndjson"
KEY_PATTERN = re.compile(r"[0-9a-f]{64}")
KEYCHAIN_SERVICE = "reader-workbench:sync-key"
KEYCHAIN_ACCOUNT = "b"


def _collector_command(cache: Cache, *parts: str) -> list[str]:
    return [
        str(Path(sys.executable).absolute()),
        "-m",
        "reader_send",
        "--cache-dir",
        str(cache.root),
        *parts,
    ]


def _collector_errors(result: dict) -> list:
    errors = result.get("errors", [])
    return errors if isinstance(errors, list) else [errors]


def _run_collector(cache: Cache, label: str, parts: tuple[str, ...], *,
                   runner=subprocess.run, env: dict[str, str] | None = None) -> dict:
    command = _collector_command(cache, *parts)
    errors = []
    try:
        completed = runner(command, capture_output=True, text=True,
                          timeout=COLLECTOR_TIMEOUT, check=False, env=env)
    except subprocess.TimeoutExpired as exc:
        return {
            "ok": False,
            "returncode": None,
            "result": None,
            "stderr": str(exc),
            "errors": [f"{label} timed out after {COLLECTOR_TIMEOUT} seconds"],
            "command": command,
        }
    except OSError as exc:
        return {
            "ok": False,
            "returncode": None,
            "result": None,
            "stderr": str(exc),
            "errors": [f"{label} could not start: {exc}"],
            "command": command,
        }
    try:
        result = json.loads(completed.stdout)
    except (ValueError, TypeError):
        result = None
    if completed.returncode != 0:
        errors.append(f"{label} exited {completed.returncode}")
    if isinstance(result, dict):
        errors.extend(_collector_errors(result))
    return {
        "ok": not errors,
        "returncode": completed.returncode,
        "result": result,
        "stderr": completed.stderr,
        "errors": errors,
        "command": command,
    }


def _annotations_output(cache: Cache, annotations_out: Path | None) -> Path:
    if annotations_out is not None:
        return annotations_out.expanduser()
    return cache.root / "annotations" / CLOUD_ANNOTATIONS_NAME


def _keychain_key(runner=subprocess.run) -> tuple[str, str]:
    try:
        completed = runner(
            ["/usr/bin/security", "find-generic-password", "-s", KEYCHAIN_SERVICE,
             "-a", KEYCHAIN_ACCOUNT, "-w"],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return "", f"Could not read the dedicated reader admin key from Keychain: {exc}"
    key = (completed.stdout or "").strip()
    if completed.returncode != 0 or not KEY_PATTERN.fullmatch(key):
        return "", ("Could not read a valid dedicated reader admin key from Keychain service "
                    f"{KEYCHAIN_SERVICE}, account {KEYCHAIN_ACCOUNT}.")
    return key, ""


def _management_key(runner=subprocess.run) -> tuple[str, str]:
    if "READER_SYNC_KEY" in os.environ:
        key = os.environ["READER_SYNC_KEY"]
        if not KEY_PATTERN.fullmatch(key):
            return "", ("READER_SYNC_KEY is set but invalid; it must be exactly 64 lowercase "
                        "hexadecimal characters. It is never replaced by a Keychain value.")
        return key, ""
    if sys.platform != "darwin":
        return "", ("Set READER_SYNC_KEY to 64 lowercase hexadecimal characters; the Keychain "
                    "fallback is used only on macOS when that variable is absent.")
    return _keychain_key(runner)


def _management_environment(runner=subprocess.run) -> tuple[dict[str, str] | None, str]:
    key, error = _management_key(runner)
    if error:
        return None, error
    env = os.environ.copy()
    env["READER_SYNC_KEY"] = key
    key = ""
    return env, ""


def _run_cloud(cache: Cache, annotations_out: Path | None, *,
               runner=subprocess.run) -> dict:
    output = _annotations_output(cache, annotations_out)
    env, error = _management_environment(runner)
    if error:
        return {"ok": False, "errors": [error], "annotations_out": str(output)}
    try:
        result = _run_collector(
            cache, "cloud",
            ("cloud", "sync", "--drain", "--annotations-out", str(output)),
            runner=runner, env=env,
        )
    finally:
        env.pop("READER_SYNC_KEY", None)
    result["annotations_out"] = str(output)
    return result


def _run_links(cache: Cache, *, runner=subprocess.run) -> dict:
    env, error = _management_environment(runner)
    if error:
        return {"ok": False, "errors": [error]}
    try:
        return _run_collector(cache, "links", ("cloud", "links", "drain"),
                              runner=runner, env=env)
    finally:
        env.pop("READER_SYNC_KEY", None)


def _acquire_lock(cache: Cache):
    try:
        import fcntl
    except ImportError as exc:
        raise ReaderError("Scheduled collection requires POSIX fcntl locking") from exc
    cache.root.mkdir(parents=True, exist_ok=True)
    lock_path = cache.root / ".collect.lock"
    handle = lock_path.open("a+")
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        handle.close()
        return None
    return handle


def collect(cache: Cache, *, annotations_out: Path | None = None,
            runner=subprocess.run) -> dict:
    """Run recent collectors, then push/pull the private site automatically."""
    lock = _acquire_lock(cache)
    if lock is None:
        return {
            "ok": True,
            "skipped": "another collection is already running",
            "started_at": datetime.now(timezone.utc).isoformat(),
        }
    collectors = {}
    try:
        collectors["subscriptions"] = _run_collector(
            cache, "subscriptions",
            ("subscriptions", "sync", "--since-days", str(SUBSCRIPTION_SINCE_DAYS)),
            runner=runner,
        )
        collectors["x_following"] = _run_collector(
            cache, "x-following",
            ("x-following", "sync", "--since-hours", str(X_SINCE_HOURS),
             "--limit", str(X_LIMIT)),
            runner=runner,
        )
        links = _run_links(cache, runner=runner)
        cloud = _run_cloud(cache, annotations_out, runner=runner)
    finally:
        lock.close()
    errors = []
    for name, result in collectors.items():
        if not result["ok"]:
            errors.append({"collector": name, "errors": result["errors"]})
    if not links["ok"]:
        errors.append({"collector": "links", "errors": links["errors"]})
    if not cloud["ok"]:
        errors.append({"collector": "cloud", "errors": cloud["errors"]})
    return {
        "ok": not errors,
        "started_at": datetime.now(timezone.utc).isoformat(),
        "since_days": SUBSCRIPTION_SINCE_DAYS,
        "since_hours": X_SINCE_HOURS,
        "x_limit": X_LIMIT,
        "subscriptions": collectors["subscriptions"],
        "x_following": collectors["x_following"],
        "links": links,
        "cloud": cloud,
        "errors": errors,
    }


def install_launchd(cache: Cache, *, card_config: Path | None = None,
                    runner=subprocess.run) -> dict:
    if sys.platform != "darwin":
        raise ReaderError("launchd scheduling is supported only on macOS")
    config_path = (card_config or card.default_config_path()).expanduser()
    if config_path.is_file():
        config = card.load_config(config_path)
        annotations_out = (Path(config["annotations_dir"]).expanduser()
                           / CLOUD_ANNOTATIONS_NAME)
        annotations_source = "card-config"
    else:
        annotations_out = cache.root / "annotations" / CLOUD_ANNOTATIONS_NAME
        annotations_source = "cache-default"
    python = Path(sys.executable).absolute()
    if not python.is_file():
        raise ReaderError(f"Current Python executable is unavailable: {python}")
    launch_agents = Path.home() / "Library/LaunchAgents"
    logs = Path.home() / "Library/Logs"
    launch_agents.mkdir(parents=True, exist_ok=True)
    logs.mkdir(parents=True, exist_ok=True)
    plist_path = launch_agents / f"{LAUNCHD_LABEL}.plist"
    payload = {
        "Label": LAUNCHD_LABEL,
        "ProgramArguments": [
            str(python),
            "-m",
            "reader_send",
            "--cache-dir",
            str(cache.root),
            "collect",
            "--annotations-out",
            str(annotations_out),
        ],
        "RunAtLoad": True,
        "StartInterval": 900,
        "ProcessType": "Background",
        "WorkingDirectory": str(Path.home()),
        "StandardOutPath": str(logs / f"{LOGS_NAME}.log"),
        "StandardErrorPath": str(logs / f"{LOGS_NAME}.error.log"),
    }
    atomic_write(plist_path, plistlib.dumps(payload, fmt=plistlib.FMT_XML, sort_keys=True))
    domain = f"gui/{os.getuid()}"
    runner(["/bin/launchctl", "bootout", f"{domain}/{LAUNCHD_LABEL}"],
           capture_output=True, check=False, timeout=10)
    try:
        loaded = runner(
            ["/bin/launchctl", "bootstrap", domain, str(plist_path)],
            capture_output=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise ReaderError(f"Wrote {plist_path}, but launchd could not load it: {exc}") from exc
    if loaded.returncode != 0:
        detail = loaded.stderr.decode("utf-8", "replace").strip() if isinstance(loaded.stderr, bytes) else str(loaded.stderr).strip()
        raise ReaderError(f"Wrote {plist_path}, but launchd bootstrap failed: {detail or 'unknown launchctl error'}")
    return {
        "installed": str(plist_path),
        "label": LAUNCHD_LABEL,
        "start_interval": 900,
        "run_at_load": True,
        "command": payload["ProgramArguments"],
        "annotations_out": str(annotations_out),
        "annotations_out_source": annotations_source,
        "collects_subscriptions": True,
        "collects_x_following": True,
        "collects_links": True,
        "delivery_target": "site",
        "automatic_site_sync": True,
        "pulls_annotations": True,
        "card_fallback": True,
        "card_required": False,
        "automatic_firmware_update": False,
        "automatic_assist": False,
    }
