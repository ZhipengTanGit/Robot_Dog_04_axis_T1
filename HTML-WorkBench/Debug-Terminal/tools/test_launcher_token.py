import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import launch_windows as launcher


class TokenConfigTests(unittest.TestCase):
    def test_explicit_environment_and_invalid_value(self):
        with patch.dict(os.environ, {'ROBOT_INGEST_TOKEN': 'a'*64}, clear=True):
            self.assertEqual(launcher.ingest_token(), 'a'*64)
        with patch.dict(os.environ, {'ROBOT_INGEST_TOKEN': 'invalid'}, clear=True):
            with self.assertRaises(RuntimeError): launcher.ingest_token()

    def test_relocated_local_and_shared_esp_config(self):
        with tempfile.TemporaryDirectory() as directory:
            repo=Path(directory);root=repo/'HTML-WorkBench'/'Debug-Terminal';root.mkdir(parents=True)
            shared=repo/'ESP32-IDF-Debug'/'Bsp'/'TelemetryConfig.local.h';shared.parent.mkdir(parents=True)
            shared.write_text('#define ROBOT_INGEST_TOKEN "'+'b'*64+'"\n')
            with patch.object(launcher,'ROOT',root),patch.dict(os.environ,{},clear=True):
                self.assertEqual(launcher.ingest_token(),'b'*64)
                own=root/'config'/'TelemetryConfig.local.h';own.parent.mkdir()
                own.write_text('#define ROBOT_INGEST_TOKEN "'+'c'*64+'"\n')
                self.assertEqual(launcher.ingest_token(),'c'*64)


if __name__=='__main__': unittest.main()
