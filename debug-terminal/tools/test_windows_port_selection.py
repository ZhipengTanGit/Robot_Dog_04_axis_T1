"""Port discovery tests never open hardware."""
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import launch_windows as launcher


class PortSelectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.runtime = Path(self.temp.name)
        self.root_patch = patch.object(launcher, 'RUNTIME', self.runtime)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def save(self, value):
        (self.runtime / 'serial').mkdir(exist_ok=True)
        (self.runtime / 'serial' / 'serial-port.json').write_text(json.dumps(value), encoding='utf-8')

    def select(self, ports, preferred=None):
        with patch('serial.tools.list_ports.comports', return_value=[
            SimpleNamespace(device=name, vid=vid) for name, vid in ports
        ]):
            return launcher.serial_port(preferred)

    def test_explicit_port_never_silently_changes_to_a_different_board(self):
        self.assertEqual(self.select([('COM18', 0x1A86)], 'com9'), 'COM9')

    def test_saved_present_selection_takes_priority_over_auto_discovery(self):
        self.save({'port': 'COM8'})
        self.assertEqual(self.select([('COM18', 0x1A86), ('COM8', 0x10C4)]), 'COM8')

    def test_changed_number_uses_unique_ch340(self):
        self.save({'port': 'COM9'})
        self.assertEqual(self.select([('COM18', 0x1A86)]), 'COM18')

    def test_ambiguous_or_empty_inventory_still_allows_webpage_to_start(self):
        self.assertEqual(self.select([('COM18', 0x1A86), ('COM19', 0x1A86)]), '')
        self.assertEqual(self.select([]), '')

    def test_saved_disconnected_port_waits_for_reconnection(self):
        self.save({'port': 'COM9'})
        self.assertEqual(self.select([]), 'COM9')

    def test_untrusted_saved_value_cannot_become_serial_url(self):
        self.save({'port': 'socket://remote:1234'})
        self.assertEqual(self.select([]), '')
        with self.assertRaises(ValueError):
            self.select([], 'socket://remote:1234')


if __name__ == '__main__':
    unittest.main()
