import importlib.util
import contextlib
import io
import json
import os
import shutil
from unittest import mock
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

helper = load("audio_helper", "tools/audio-helper.py")
audio_test = load("audio_test", "tools/audio-test.py")

class AudioHelperTests(unittest.TestCase):
    def test_argument_surface_is_exact(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(helper.main(["enable", "x"]), 2)

    def test_params_accept_only_whitelisted_scalar_sysfs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "parameters").mkdir()
            (root / "parameters/trace_frames").write_text("Y\n")
            (root / "parameters/test_hpd_after_ms").write_text("1200\n")
            (root / "parameters/evil").write_text("; touch /tmp/no\n")
            self.assertEqual(helper._module_params(root), ["trace_frames=Y", "test_hpd_after_ms=1200"])
            (root / "parameters/trace_frames").write_text("yes\n")
            with self.assertRaises(helper.Failure):
                helper._module_params(root)

    def test_arecord_capture_reports_exact_silent_capture_without_saving(self):
        with tempfile.TemporaryDirectory() as tmp:
            param = Path(tmp) / "param"
            param.write_text("Y")
            calls = []
            def run(argv, **kwargs):
                calls.append((argv, kwargs))
                return subprocess.CompletedProcess(argv, 0, b"\0" * (48000*5*4), b"")
            result = audio_test._capture("3", run=run, sysfs=str(param))
            self.assertEqual(result["result"], "silent")
            self.assertEqual(result["frames"], 240000)
            self.assertEqual(calls[0][0][0], audio_test.ARECORD)
            self.assertIn("hw:3,0", calls[0][0])
            self.assertIn("--period-size=960", calls[0][0])

    def test_capture_distinguishes_busy_and_short_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            param = Path(tmp) / "param"
            param.write_text("1")
            busy = audio_test._capture("0", sysfs=param, run=lambda *a, **k: subprocess.CompletedProcess(a, 1, b"", b"Device busy"))
            short = audio_test._capture("0", sysfs=param, run=lambda *a, **k: subprocess.CompletedProcess(a, 0, b"\0"*8, b""))
        self.assertEqual(busy["result"], "busy")
        self.assertEqual(short["result"], "inconclusive")

    def _switch_fixture(self, initially_enabled):
        tmp = tempfile.TemporaryDirectory()
        base = Path(tmp.name)
        module = base / "module/gc573_pure"
        bus = base / "bus"
        pci = bus / "0000:01:00.0"
        params = module / "parameters"
        params.mkdir(parents=True)
        bus.mkdir(exist_ok=True)
        pci.mkdir()
        (pci / "vendor").write_text("0x1461")
        (pci / "device").write_text("0x0054")
        (pci / "driver").symlink_to(base / "drivers/gc573_pure")
        (base / "drivers/gc573_pure").mkdir(parents=True)
        (params / "audio_experimental").write_text("Y" if initially_enabled else "N")
        (params / "trace_frames").write_text("N")
        sound = base / "class/sound"
        sound.mkdir(parents=True)
        class_root = base / "class"
        videos = class_root / "video4linux/video0"
        videos.mkdir(parents=True)
        (videos / "device").symlink_to(pci)
        (videos / "dev").write_text("81:0")
        dev_char = base / "dev-char"
        dev_char.mkdir()
        (dev_char / "81:0").touch()
        if initially_enabled:
            self._make_card(sound, pci)
        def emulate(argv, **kwargs):
            if argv[0].endswith("modinfo"):
                return subprocess.CompletedProcess(argv, 0, "audio_experimental: bool\ntrace_frames: bool\n", "")
            if argv[0].endswith("fuser"):
                return subprocess.CompletedProcess(argv, 1, "", "")
            if argv[1:3] == ["-r", "gc573_pure"]:
                shutil.rmtree(module)
                (pci / "driver").unlink()
                return subprocess.CompletedProcess(argv, 0, "", "")
            enabled = "audio_experimental=1" in argv
            params.mkdir(parents=True, exist_ok=True)
            (params / "audio_experimental").write_text("Y" if enabled else "N")
            (params / "trace_frames").write_text("N")
            (pci / "driver").symlink_to(base / "drivers/gc573_pure")
            if enabled:
                self._make_card(sound, pci)
            elif (sound / "card0").exists():
                shutil.rmtree(sound / "card0")
            if getattr(self, "fail_next_probe", False):
                self.fail_next_probe = False
                shutil.rmtree(module)
                (pci / "driver").unlink()
                return subprocess.CompletedProcess(argv, 1, "", "probe failed")
            return subprocess.CompletedProcess(argv, 0, "", "")
        return tmp, base, module, bus, emulate, sound, class_root, dev_char

    @staticmethod
    def _make_card(sound, pci):
        card = sound / "card0"
        card.mkdir(exist_ok=True)
        link = card / "device"
        if not link.exists():
            link.symlink_to(pci)

    def _run_switch(self, action, fixture):
        tmp, base, module, bus, runner = fixture[:5]
        lock = base / "lock"
        with mock.patch.object(helper.os, "geteuid", return_value=0), \
             mock.patch.object(helper, "_lock", side_effect=lambda: os.open(lock, os.O_CREAT | os.O_RDWR, 0o600)):
            return helper.switch(action, run=runner, root=module, bus=bus,
                                 sound_root=fixture[5], class_root=fixture[6], dev_char=fixture[7])

    def test_switch_enable_from_audio_off(self):
        fixture = self._switch_fixture(False)
        try:
            result = self._run_switch("enable", fixture)
            self.assertEqual(result, {"ok": True, "enabled": True})
        finally:
            fixture[0].cleanup()

    def test_switch_disable_from_audio_on(self):
        fixture = self._switch_fixture(True)
        try:
            result = self._run_switch("disable", fixture)
            self.assertEqual(result, {"ok": True, "enabled": False})
        finally:
            fixture[0].cleanup()

    def test_missing_installed_parameter_refuses_before_unload(self):
        fixture = self._switch_fixture(False)
        tmp, base, module, bus, runner = fixture[:5]
        calls = []
        def missing_parameter(argv, **kwargs):
            calls.append(argv)
            if argv[0].endswith("modinfo"):
                return subprocess.CompletedProcess(argv, 0, "trace_frames: bool\n", "")
            return runner(argv, **kwargs)
        fixture = (*fixture[:4], missing_parameter, *fixture[5:])
        try:
            result = self._run_switch("enable", fixture)
            self.assertFalse(result["ok"])
            self.assertIn("does not declare audio_experimental", result["error"])
            self.assertFalse(any(command[1:3] == ["-r", "gc573_pure"] for command in calls))
        finally:
            tmp.cleanup()

    def test_busy_client_prevents_unload(self):
        fixture = self._switch_fixture(False)
        original = fixture[4]
        calls = []
        def busy_runner(argv, **kwargs):
            calls.append(argv)
            if argv[0].endswith("fuser"):
                return subprocess.CompletedProcess(argv, 0, "/dev/video0: 123", "")
            return original(argv, **kwargs)
        fixture = (*fixture[:4], busy_runner, *fixture[5:])
        try:
            result = self._run_switch("enable", fixture)
            self.assertFalse(result["ok"])
            self.assertFalse(any(command[1:3] == ["-r", "gc573_pure"] for command in calls))
        finally:
            fixture[0].cleanup()

    def test_fuser_usage_error_does_not_mean_idle(self):
        fixture = self._switch_fixture(False)
        calls = []
        original = fixture[4]
        def runner(argv, **kwargs):
            calls.append(argv)
            if argv[0].endswith("fuser"):
                return subprocess.CompletedProcess(argv, 1, "", "Usage: fuser NAME...")
            return original(argv, **kwargs)
        fixture = (*fixture[:4], runner, *fixture[5:])
        try:
            result = self._run_switch("enable", fixture)
            self.assertFalse(result["ok"])
            self.assertIn("could not check", result["error"])
            self.assertFalse(any(command[1:3] == ["-r", "gc573_pure"] for command in calls))
        finally:
            fixture[0].cleanup()

    def test_failed_probe_rolls_back_prior_state(self):
        fixture = self._switch_fixture(False)
        self.fail_next_probe = True
        try:
            result = self._run_switch("enable", fixture)
            self.assertFalse(result["ok"])
            self.assertIn("rollback succeeded", result["error"])
            self.assertEqual((fixture[2] / "parameters/audio_experimental").read_text(), "N")
        finally:
            fixture[0].cleanup()

    def test_failed_rollback_is_reported_truthfully(self):
        fixture = self._switch_fixture(False)
        tmp, base, module, bus, runner = fixture[:5]
        count = 0
        def always_fail_probe(argv, **kwargs):
            nonlocal count
            if argv[0].endswith("modinfo"):
                return runner(argv, **kwargs)
            if argv[0].endswith("fuser"):
                return runner(argv, **kwargs)
            if argv[1:3] == ["-r", "gc573_pure"]:
                return runner(argv, **kwargs)
            count += 1
            if count <= 2:
                # Fail first requested load and the subsequent restore attempt.
                params = module / "parameters"
                params.mkdir(parents=True, exist_ok=True)
                (params / "audio_experimental").write_text("N")
                (params / "trace_frames").write_text("N")
                (bus / "0000:01:00.0/driver").symlink_to(base / "drivers/gc573_pure")
                return subprocess.CompletedProcess(argv, 1, "", "probe failed")
            return runner(argv, **kwargs)
        fixture = (*fixture[:4], always_fail_probe, *fixture[5:])
        try:
            result = self._run_switch("enable", fixture)
            self.assertFalse(result["ok"])
            self.assertIn("rollback failed", result["error"])
        finally:
            tmp.cleanup()

if __name__ == "__main__":
    unittest.main()
