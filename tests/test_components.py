# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for retained module components.

Set GPU_UNLOCK_VBIOS_ROM to the approved RTX 3080 Ti ROM to enable its
read-only, 1 MiB-prefix integration check in vbios_610.
"""
import hashlib
from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "kernel/gpu_unlock"
# Generated or installed firmware lets the adapter run the whole unlock
# against mocked hardware.
ASSETS = next((path for path in (Path(os.environ.get("GPU_UNLOCK_ASSETS", "")),
                                 ROOT / "build/firmware")
               if (path / "gsp_ga10x.bin").is_file()), None)
VBIOS_ROM = os.environ.get("GPU_UNLOCK_VBIOS_ROM")
VBIOS_ROM_SIZE = 2048000
VBIOS_ROM_SHA256 = "ece77995f3162aa16800932bd1e9a038509356d3fd34b18f82177448355fd9a2"

CASES = {
    "main": [],
    "gfw_eval": ["gfw_eval.c"],
    "layout_610": ["layout_610.c"],
    "prep_610": ["layout_610.c", "prep_610.c"],
    "vbios_610": ["vbios_610.c"],
    "unlock_adapter": ["gfw_eval.c", "prep_610.c", "layout_610.c", "vbios_610.c"],
}
CASES["unlock_adapter_uefi"] = CASES["unlock_adapter"]
# Tests that include a kernel source directly build it against a shim.
SHIMS = {
    "unlock_adapter": ["-I", str(ROOT / "tests/unlock_shim")],
    # The kernel build does not warn on sign-compare; match it for main.c.
    "main": ["-I", str(ROOT / "tests/main_shim"), "-pthread", "-Wno-sign-compare"],
}
SHIMS["unlock_adapter_uefi"] = [*SHIMS["unlock_adapter"], "-DGU_CALLER_HALTS_ON_FAILURE=1"]


class ComponentTests(unittest.TestCase):
    def test_components(self):
        with tempfile.TemporaryDirectory() as directory:
            for name, sources in CASES.items():
                with self.subTest(name=name):
                    output = Path(directory) / name
                    shim = SHIMS.get(name, [])
                    test_name = "unlock_adapter" if name == "unlock_adapter_uefi" else name
                    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall",
                               "-Wextra", "-Werror", "-Wno-unused-parameter",
                               "-I", str(KERNEL), *shim,
                               *(str(KERNEL / source) for source in sources),
                               str(ROOT / "tests" / f"test_{test_name}.c"),
                               "-o", str(output)]
                    subprocess.run(command, check=True)
                    args = [str(output)]
                    if test_name == "unlock_adapter" and ASSETS:
                        args += ["-", str(ASSETS)]
                    if name == "vbios_610" and VBIOS_ROM:
                        rom_path = Path(VBIOS_ROM)
                        self.assertEqual(rom_path.stat().st_size, VBIOS_ROM_SIZE)
                        digest = hashlib.sha256()
                        with rom_path.open("rb") as rom:
                            for chunk in iter(lambda: rom.read(1024 * 1024), b""):
                                digest.update(chunk)
                        self.assertEqual(digest.hexdigest(), VBIOS_ROM_SHA256)
                        args += ["--rtx3080ti-rom", str(rom_path)]
                    result = subprocess.run(args, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0,
                                     (result.stdout + result.stderr)[-4000:])
