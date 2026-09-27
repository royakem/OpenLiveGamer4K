# SPDX-License-Identifier: GPL-2.0-only
"""Passive diagnostic report and default-audio regression checks."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('report', ROOT/'tools/collect-audio-report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

class DiagnosticsTests(unittest.TestCase):
    def test_audio_default_is_on_and_override_retained(self):
        source = (ROOT/'src/gc573_pci.c').read_text()
        self.assertIn('static bool audio_experimental = true;', source)
        self.assertIn('module_param(audio_experimental, bool, 0444)', source)

    def test_status_is_known_numeric_fields_only(self):
        self.assertEqual(report.parse_status('dma_running=1 queue_overruns=0 unknown=private audio_output_c7=ff video_scdt=oops'),
                         {'dma_running':'1','queue_overruns':'0','audio_output_c7':'ff'})
        self.assertIsNone(report.parse_status(None))

    def test_no_card_report_is_useful(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            result=report.collect(root,root,root,software={'packages':{}})
            self.assertEqual(result['schema_version'],2)
            self.assertEqual(result['devices'],[])
            self.assertFalse(result['capture_test_performed'])
            self.assertIn('issues/new',result['issue_url'])

    def test_system_info_allowlist(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'os-release').write_text('ID=linuxmint\nVERSION_ID="22.3"\nPRETTY_NAME="Private custom title"\n')
            with patch.dict(os.environ,{'XDG_SESSION_TYPE':'wayland'}):
                self.assertEqual(report.system_info(root),{'distribution':'linuxmint','version':'22.3','session':'wayland'})
            (root/'os-release').write_text('ID=personal-machine\nVERSION_ID=private\n')
            self.assertEqual(report.system_info(root)['distribution'],'other-or-unknown')
            self.assertIsNone(report.system_info(root)['version'])

    def test_package_query_is_bounded_and_fixed(self):
        calls=[]
        def run(args,**kwargs):
            calls.append((args,kwargs))
            return subprocess.CompletedProcess(args,0,'openlivegamer4k-control:amd64\t0.1.0~beta2\nother\t9\n','')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);sha='a'*40;(root/'SOURCE-REVISION.md').write_text('Public Git revision: '+sha+'\n')
            result=report.software_info(run,root)
        self.assertEqual(result['packages'],{'openlivegamer4k-control':'0.1.0~beta2'})
        self.assertEqual(result['public_source_revision'],sha)
        self.assertEqual(calls[0][1]['timeout'],3)
        self.assertNotIn('shell',calls[0][1])

    def test_missing_package_manager_is_not_fatal(self):
        def fail(*a,**k):raise FileNotFoundError()
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(report.software_info(fail,Path(tmp)),{'packages':{},'public_source_revision':None})

    def test_unrelated_and_custom_sound_names_are_excluded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);pci=root/'devices/0000:01:00.0';pci.mkdir(parents=True)
            card=root/'class/sound/card3';card.mkdir(parents=True);(card/'device').symlink_to(pci)
            (card/'id').write_text('personal-device-name')
            self.assertEqual(report.alsa_cards(root,root,{pci.name}),[{'number':3}])
            (card/'id').write_text('Audio');self.assertEqual(report.alsa_cards(root,root,{pci.name}),[{'number':3,'id':'Audio'}])
            self.assertEqual(report.alsa_cards(root,root,{'0000:02:00.0'}),[])

    def test_cli_file_is_private_and_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);out=root/'report.json'
            args=['python3',str(ROOT/'tools/collect-audio-report.py'),str(out),'--sysfs-root',str(root),'--proc-root',str(root)]
            subprocess.run(args,check=True,capture_output=True)
            self.assertEqual(out.stat().st_mode & 0o777,0o600)
            data=out.read_bytes();self.assertEqual(json.loads(data)['schema_version'],2)
            self.assertNotEqual(subprocess.run(args,capture_output=True).returncode,0)
            self.assertEqual(out.read_bytes(),data)

if __name__=='__main__':unittest.main()
