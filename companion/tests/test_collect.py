import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from reader_send import collect
from reader_send.cache import Cache

KEY = "a" * 64


class FakeRunner:
    """Answer collector subprocesses with JSON and record every invocation."""

    def __init__(self, key=KEY, fail=None):
        self.key = key
        self.fail = fail
        self.calls = []

    def __call__(self, command, **kwargs):
        recorded = dict(kwargs)
        if recorded.get("env") is not None:
            recorded["env"] = dict(recorded["env"])
        self.calls.append((list(command), recorded))
        if command[0] == "/usr/bin/security":
            return subprocess.CompletedProcess(command, 0, stdout=self.key + "\n", stderr="")
        if self.fail and self.fail in " ".join(command):
            raise subprocess.TimeoutExpired(command, 1)
        return subprocess.CompletedProcess(command, 0,
                                           stdout=json.dumps({"ok": True}), stderr="")

    def collector_calls(self):
        return [(command, kwargs) for command, kwargs in self.calls
                if command[0] != "/usr/bin/security"]

    def security_calls(self):
        return [command for command, _ in self.calls if command[0] == "/usr/bin/security"]

    def cloud_commands(self):
        return [command for command, _ in self.collector_calls() if "cloud" in command]

    def cloud_environments(self):
        return [kwargs.get("env") for command, kwargs in self.collector_calls()
                if "cloud" in command]


class EnvironmentKeyTest(unittest.TestCase):
    def test_environment_key_is_used_on_every_host_without_keychain(self):
        for platform in ("darwin", "linux"):
            runner = FakeRunner()
            with tempfile.TemporaryDirectory() as temporary:
                cache = Cache(Path(temporary) / "cache")
                with patch.dict(os.environ, {"READER_SYNC_KEY": KEY}, clear=True):
                    with patch("reader_send.collect.sys.platform", platform):
                        result = collect.collect(cache, runner=runner)
            self.assertTrue(result["ok"])
            self.assertEqual(runner.security_calls(), [])
            self.assertEqual(len(runner.collector_calls()), 4)
            environments = runner.cloud_environments()
            self.assertEqual(len(environments), 2)
            for environment in environments:
                self.assertEqual(environment["READER_SYNC_KEY"], KEY)

    def test_present_invalid_environment_key_fails_without_keychain_substitution(self):
        for bad in ("", "a" * 63, "A" * 64, "g" * 64, KEY + "\n"):
            runner = FakeRunner()
            with tempfile.TemporaryDirectory() as temporary:
                cache = Cache(Path(temporary) / "cache")
                with patch.dict(os.environ, {"READER_SYNC_KEY": bad}, clear=True):
                    with patch("reader_send.collect.sys.platform", "darwin"):
                        result = collect.collect(cache, runner=runner)
            self.assertFalse(result["ok"])
            self.assertEqual(runner.security_calls(), [])
            self.assertEqual(runner.cloud_commands(), [])
            self.assertEqual(len(runner.collector_calls()), 2)
            for hidden in ("links", "cloud"):
                self.assertFalse(result[hidden]["ok"])
                self.assertEqual(len(result[hidden]["errors"]), 1)
                self.assertIn("READER_SYNC_KEY", result[hidden]["errors"][0])
            for environment in runner.cloud_environments():
                self.assertIsNone(environment)
            if bad:
                self.assertNotIn(bad, json.dumps(result))

    def test_absent_environment_key_uses_keychain_only_on_macos(self):
        runner = FakeRunner()
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary) / "cache")
            with patch.dict(os.environ, {}, clear=True):
                with patch("reader_send.collect.sys.platform", "darwin"):
                    result = collect.collect(cache, runner=runner)
        self.assertTrue(result["ok"])
        self.assertTrue(runner.security_calls())
        self.assertEqual(runner.security_calls()[0][:2],
                         ["/usr/bin/security", "find-generic-password"])
        self.assertEqual(len(runner.collector_calls()), 4)

        runner = FakeRunner()
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary) / "cache")
            with patch.dict(os.environ, {}, clear=True):
                with patch("reader_send.collect.sys.platform", "linux"):
                    result = collect.collect(cache, runner=runner)
        self.assertFalse(result["ok"])
        self.assertEqual(runner.security_calls(), [])
        self.assertIn("READER_SYNC_KEY", result["cloud"]["errors"][0])
        self.assertIn("READER_SYNC_KEY", result["links"]["errors"][0])

    def test_invalid_keychain_item_is_reported_without_printing_it(self):
        runner = FakeRunner(key="not-a-key")
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary) / "cache")
            with patch.dict(os.environ, {}, clear=True):
                with patch("reader_send.collect.sys.platform", "darwin"):
                    result = collect.collect(cache, runner=runner)
        self.assertFalse(result["ok"])
        self.assertNotIn("not-a-key", json.dumps(result))


class CollectRunTest(unittest.TestCase):
    def test_collect_runs_every_source_without_any_card_config(self):
        runner = FakeRunner()
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary) / "cache")
            expected = cache.root / "annotations" / "cloud-annotations.ndjson"
            with patch.dict(os.environ, {"READER_SYNC_KEY": KEY}, clear=True), \
                    patch("reader_send.collect.card.load_config",
                          side_effect=AssertionError("collect must not read card config")):
                result = collect.collect(cache, runner=runner)
        self.assertTrue(result["ok"])
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["cloud"]["annotations_out"], str(expected))
        self.assertEqual(len(runner.collector_calls()), 4)
        joined = [" ".join(command) for command, _ in runner.collector_calls()]
        self.assertTrue(any("subscriptions sync" in command for command in joined))
        self.assertTrue(any("x-following sync" in command for command in joined))
        self.assertTrue(any("cloud links drain" in command for command in joined))
        cloud = [command for command in runner.cloud_commands() if "--annotations-out" in command][0]
        self.assertEqual(cloud[cloud.index("--annotations-out") + 1], str(expected))

    def test_explicit_annotations_path_is_used_and_timeouts_stay_visible(self):
        runner = FakeRunner(fail="x-following")
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary) / "cache")
            output = Path(temporary) / "exports" / "annotations.ndjson"
            with patch.dict(os.environ, {"READER_SYNC_KEY": KEY}, clear=True):
                result = collect.collect(cache, annotations_out=output, runner=runner)
        self.assertFalse(result["ok"])
        self.assertEqual(result["cloud"]["annotations_out"], str(output))
        cloud = [command for command in runner.cloud_commands() if "--annotations-out" in command][0]
        self.assertEqual(cloud[cloud.index("--annotations-out") + 1], str(output))
        self.assertEqual(result["x_following"]["errors"],
                         [f"x-following timed out after {collect.COLLECTOR_TIMEOUT} seconds"])
        self.assertEqual(result["errors"],
                         [{"collector": "x_following",
                           "errors": result["x_following"]["errors"]}])


if __name__ == "__main__":
    unittest.main()
