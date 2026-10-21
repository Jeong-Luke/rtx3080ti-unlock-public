import importlib.util
import hashlib
import json
from pathlib import Path
import tarfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prepare', ROOT / 'tools/prepare.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


CHIP_SOURCE = """
void __nvoc_init_halspec_ChipHal(ChipHal *pChipHal, NvU32 arch, NvU32 impl, NvU32 hidrev)
{
    else if (arch == 0x17 && impl == 0x2)
    {
        pChipHal->__nvoc_HalVarIdx = 43;
    }
}
"""


def hal_source(mask='0x0000f800UL', implementation='kgspGetBinArchiveBooterLoadUcode_GA102'):
    return f"""
void __nvoc_init_funcTable_KernelGsp_1(KernelGsp *pThis)
{{
    if (vf)
    {{
        pThis->__kgspGetBinArchiveBooterLoadUcode__ = &kgspGetBinArchiveBooterLoadUcode_VF;
    }}
    else if (( ((chipHal_HalVarIdx >> 5) == 1UL) &&
               ((1UL << (chipHal_HalVarIdx & 0x1f)) & {mask}) ))
    {{
        pThis->__kgspGetBinArchiveBooterLoadUcode__ = &{implementation};
    }}
}}
"""


class HalDispatchTests(unittest.TestCase):
    def test_all_required_dispatches_against_pinned_nvidia_source(self):
        manifest = json.loads((ROOT / 'firmware/artifacts-610.43.03.json').read_text())
        entry = manifest['source_artifacts'][0]
        archive_path = ROOT / 'audit-inputs' / entry['filename']
        with archive_path.open('rb') as stream:
            self.assertEqual(hashlib.file_digest(stream, 'sha256').hexdigest(), entry['sha256'])
        prefix = 'NVIDIA-kernel-module-source-610.43.03/src/nvidia/generated/'
        with tarfile.open(archive_path, 'r:xz') as archive:
            hal = archive.extractfile(prefix + 'g_kernel_gsp_nvoc.c').read().decode()
            chips = archive.extractfile(prefix + 'g_chips2halspec_nvoc.c').read().decode()
        expected = (
            ('kgspGetBinArchiveBooterLoadUcode', 'kgspGetBinArchiveBooterLoadUcode_GA102'),
            ('kgspGetBinArchiveBooterUnloadUcode', 'kgspGetBinArchiveBooterUnloadUcode_GA102'),
            ('kgspGetBinArchiveGspRmBoot', 'kgspGetBinArchiveGspRmBoot_GA102'),
            ('kgspGetPrescrubbedTopFbSize', 'kgspGetPrescrubbedTopFbSize_13afc5'),
            ('kgspGetMinWprHeapSizeMB', 'kgspGetMinWprHeapSizeMB_d2c3c5'),
        )
        for method, implementation in expected:
            with self.subTest(method=method):
                result = prepare.ga102_dispatch(hal, chips, method, implementation)
                self.assertEqual(result['chip_hal_index'], 43)
                self.assertEqual(result['implementation'], implementation)

    def test_dispatch_condition_selects_ga102_index_not_comment_text(self):
        source = hal_source().replace(
            'else if', 'else if /* ChipHal: not authoritative */', 1)
        evidence = prepare.ga102_dispatch(
            source, CHIP_SOURCE, 'kgspGetBinArchiveBooterLoadUcode',
            'kgspGetBinArchiveBooterLoadUcode_GA102')
        self.assertEqual(evidence['chip_hal_index'], 43)
        self.assertIn('0x0000f800UL', evidence['dispatch_condition'])

    def test_ga102_name_nearby_does_not_rescue_wrong_mask(self):
        source = hal_source(mask='0x00001000UL').replace(
            'else if', 'else if /* ChipHal: GA102 */', 1)
        with self.assertRaises(ValueError):
            prepare.ga102_dispatch(
                source, CHIP_SOURCE, 'kgspGetBinArchiveBooterLoadUcode',
                'kgspGetBinArchiveBooterLoadUcode_GA102')

    def test_wrong_chip_to_hal_mapping_is_rejected(self):
        source = CHIP_SOURCE.replace('= 43;', '= 48;')
        with self.assertRaises(ValueError):
            prepare.ga102_dispatch(
                hal_source(), source, 'kgspGetBinArchiveBooterLoadUcode',
                'kgspGetBinArchiveBooterLoadUcode_GA102')

    def test_dispatch_cannot_be_inferred_from_an_unconditional_assignment(self):
        source = """
void __nvoc_init_funcTable_KernelGsp_1(KernelGsp *pThis)
{
    pThis->__kgspGetBinArchiveBooterLoadUcode__ =
        &kgspGetBinArchiveBooterLoadUcode_GA102;
}
"""
        with self.assertRaises(ValueError):
            prepare.ga102_dispatch(
                source, CHIP_SOURCE, 'kgspGetBinArchiveBooterLoadUcode',
                'kgspGetBinArchiveBooterLoadUcode_GA102')


if __name__ == '__main__':
    unittest.main()
