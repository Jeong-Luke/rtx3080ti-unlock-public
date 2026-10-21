import importlib.util
from pathlib import Path
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prepare_usb', ROOT / 'uefi/prepare_usb.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)

TARGET = '00000000:01:00.0, 0x220810DE, 0x153510DE, 12288\n'


class WindowsPreparationTests(unittest.TestCase):
    def detect(self, text):
        with patch.object(prepare.subprocess, 'run', return_value=types.SimpleNamespace(stdout=text)):
            return prepare.detect_card()

    def test_exact_3080ti_target_is_accepted_and_segment_is_normalized(self):
        self.assertEqual(self.detect(TARGET), ('0000:01:00.0', 0x10de, 0x1535))

    def test_other_cards_do_not_prevent_unique_target_match(self):
        text = ('00000000:02:00.0, 0x220410DE, 0x1462381D, 24576\n'
                + TARGET)
        self.assertEqual(self.detect(text), ('0000:01:00.0', 0x10de, 0x1535))

    def test_wrong_sku_is_refused(self):
        for identity in ('0x250410DE', '0x220410DE', '0x221610DE'):
            with self.subTest(identity=identity):
                with self.assertRaises(ValueError):
                    self.detect(f'00000000:01:00.0, {identity}, 0x153510DE, 12288\n')

    def test_wrong_vendor_is_refused(self):
        with self.assertRaises(ValueError):
            self.detect('00000000:01:00.0, 0x220810DD, 0x153510DE, 12288\n')

    def test_wrong_subsystem_is_refused(self):
        for subsystem in ('0x15351462', '0x397D10DE', '0x00000000', '0xFFFFFFFF'):
            with self.subTest(subsystem=subsystem):
                with self.assertRaises(ValueError):
                    self.detect(f'00000000:01:00.0, 0x220810DE, {subsystem}, 12288\n')

    def test_wrong_capacity_is_refused(self):
        for memory in (12287, 12289, 11264, 24576):
            with self.subTest(memory=memory):
                with self.assertRaises(ValueError):
                    self.detect(f'00000000:01:00.0, 0x220810DE, 0x153510DE, {memory}\n')

    def test_duplicate_target_is_refused(self):
        with self.assertRaises(ValueError):
            self.detect(TARGET * 2)

    def test_invalid_or_unsupported_bdf_is_refused(self):
        for bdf in ('00000000:01:00.8', '00000000:01:20.0', '00010000:01:00.0',
                    '0000:1:00.0', 'not-a-bdf'):
            with self.subTest(bdf=bdf):
                with self.assertRaises(ValueError):
                    self.detect(f'{bdf}, 0x220810DE, 0x153510DE, 12288\n')

    def test_four_digit_segment_is_accepted(self):
        self.assertEqual(self.detect('0000:41:00.0, 0x220810de, 0x153510de, 12288\n'),
                         ('0000:41:00.0', 0x10de, 0x1535))


if __name__ == '__main__':
    unittest.main()
