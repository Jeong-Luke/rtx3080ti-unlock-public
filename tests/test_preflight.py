import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'preflight', Path(__file__).resolve().parents[1] / 'tools/preflight.py')
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)


class PreflightTests(unittest.TestCase):
    def test_exact_rtx3080ti_board_ids_are_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            device = root / '0000:21:00.0'
            device.mkdir()
            for key, value in dict(vendor='0x10de', device='0x2208',
                                   subsystem_vendor='0x10de', subsystem_device='0x1535',
                                   reset_method='[flr] bus').items():
                (device / key).write_text(value)
            report = preflight.inspect(root, device.name)
            self.assertTrue(report['ready_for_pre_driver_attempt'])
            self.assertIn('rtx3080ti_unlock.target=0000:21:00.0', report['one_shot_boot_args'])
            self.assertIn('board_vendor=0x10de', report['one_shot_boot_args'])
            self.assertIn('board_device=0x1535', report['one_shot_boot_args'])
            (device / 'driver').symlink_to(root / 'nvidia')
            self.assertFalse(preflight.inspect(root, device.name)['ready_for_pre_driver_attempt'])
            (device / 'driver').unlink()
            for key, value in (('vendor', '0x10df'), ('device', '0x2207'),
                               ('subsystem_vendor', '0x1462'), ('subsystem_device', '0x1534')):
                (device / key).write_text(value)
                report = preflight.inspect(root, device.name)
                self.assertFalse(report['ready_for_pre_driver_attempt'])
                self.assertNotIn('one_shot_boot_args', report)
                (device / key).write_text({
                    'vendor': '0x10de', 'device': '0x2208',
                    'subsystem_vendor': '0x10de', 'subsystem_device': '0x1535'
                }[key])

    def test_missing_metadata_does_not_produce_execute_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            report = preflight.inspect(Path(directory), '0000:01:00.0')
            self.assertFalse(report['ready_for_pre_driver_attempt'])
            self.assertNotIn('one_shot_boot_args', report)


if __name__ == '__main__':
    unittest.main()
