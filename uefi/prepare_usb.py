#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Create files in an ordinary directory; never format USB or edit boot settings."""
import argparse
from contextlib import contextmanager
import csv
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.4.0-rtx3080ti'
TARGET_VENDOR = 0x10de
TARGET_DEVICE = 0x2208
TARGET_SUBVENDOR = 0x10de
TARGET_SUBDEVICE = 0x1535
TARGET_MEMORY_MIB = 12288
sys.path.insert(0, str(ROOT / 'firmware'))
import extract_boot_assets as boot
import fetch_artifacts as fetch
import generate_v67 as v67


@contextmanager
def package_tar(path):
    # Python 3.14 has native zstd tar support. On 3.12/3.13, use zstandard.
    try:
        archive = tarfile.open(path, 'r:*')
    except tarfile.ReadError:
        try:
            import zstandard
        except ImportError:
            raise RuntimeError('Python <3.14 needs: python -m pip install zstandard') from None
        with path.open('rb') as source:
            with zstandard.ZstdDecompressor().stream_reader(source) as stream:
                with tarfile.open(fileobj=stream, mode='r|') as archive:
                    yield archive
    else:
        with archive:
            yield archive


def hexadecimal(value, name):
    if not re.fullmatch(r'0x[0-9a-fA-F]{1,8}', value):
        raise ValueError(f'Invalid {name} reported by NVIDIA')
    return int(value, 16)


def detect_card():
    """Return the unique GA102 RTX 3080 Ti target as (BDF, subsystem vendor, device)."""
    command = ['nvidia-smi', '--query-gpu=pci.bus_id,pci.device_id,pci.sub_device_id,memory.total', '--format=csv,noheader,nounits']
    result = subprocess.run(command, capture_output=True, text=True, timeout=15, check=True)
    matches = []
    for row in csv.reader(io.StringIO(result.stdout)):
        if len(row) != 4:
            continue
        bdf, identity, subsystem, memory = [v.strip() for v in row]
        identity = hexadecimal(identity, 'PCI identity')
        vendor, device = identity & 0xffff, identity >> 16
        if (vendor, device) != (TARGET_VENDOR, TARGET_DEVICE):
            continue
        sub = hexadecimal(subsystem, 'subsystem ID')
        subvendor, subdevice = sub & 0xffff, sub >> 16
        if not 0 < subvendor < 0xffff or not 0 < subdevice < 0xffff:
            raise ValueError('Invalid subsystem ID reported by NVIDIA')
        if (subvendor, subdevice) != (TARGET_SUBVENDOR, TARGET_SUBDEVICE):
            continue
        try:
            capacity = int(memory)
        except ValueError:
            raise ValueError('Invalid memory size reported by NVIDIA') from None
        if capacity != TARGET_MEMORY_MIB:
            continue
        # NVIDIA sometimes prints an eight-digit segment; EFI stores 16 bits.
        match = re.fullmatch(r'([0-9a-fA-F]{4,8}):([0-9a-fA-F]{2}):([0-9a-fA-F]{2})\.([0-7])', bdf)
        if not match:
            raise ValueError('Unexpected NVIDIA PCI BDF')
        segment, bus, device, function = [int(v, 16) for v in match.groups()]
        if segment > 0xffff or device > 31:
            raise ValueError('PCI address outside supported range')
        matches.append((f'{segment:04x}:{bus:02x}:{device:02x}.{function}', subvendor, subdevice))
    if len(matches) != 1:
        raise ValueError('Expected exactly one RTX 3080 Ti (10de:2208, subsystem 10de:1535, '
                         f'{TARGET_MEMORY_MIB} MiB); found {len(matches)}')
    return matches[0]


def firmware(cache, output):
    manifest = json.loads((ROOT / 'firmware/artifacts-610.43.03.json').read_text())
    source_entry, package_entry = fetch.inputs(manifest)
    source = fetch.fetch(source_entry, cache)
    package = fetch.fetch(package_entry, cache)
    print('Pinned input checksums verified.', flush=True)
    notice = package_entry['notice']
    wanted = {'.PKGINFO', v67.FIRMWARE_PREFIX + 'gsp_ga10x.bin', notice['path']}
    members = {}
    with package_tar(package) as archive:
        for item in archive:
            if item.name not in wanted:
                continue
            if item.name in members or not item.isfile() or item.size > 128 * 1024**2:
                raise ValueError('Invalid/duplicate package member')
            stream = archive.extractfile(item)
            if stream is None:
                raise ValueError('Missing regular file data')
            members[item.name] = stream.read()
    if set(members) != wanted:
        raise ValueError('Pinned package is missing required files')
    if f'pkgver = {v67.PACKAGE_VERSION}' not in members['.PKGINFO'].decode().splitlines():
        raise ValueError('Package version mismatch')
    gsp = members[v67.FIRMWARE_PREFIX + 'gsp_ga10x.bin']
    payload = v67.build_payload()
    license_bytes = members[notice['path']]
    for data, expected in [(gsp, v67.GSP_SHA256), (payload, v67.PAYLOAD_SHA256), (license_bytes, notice['sha256'])]:
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('Extracted asset checksum mismatch')
    with tempfile.TemporaryDirectory() as directory:
        tree = Path(directory)
        prefix = 'NVIDIA-kernel-module-source-610.43.03/src/nvidia/generated/'
        with tarfile.open(source, 'r:xz') as archive:
            for name in boot.PINNED:
                item = archive.getmember(prefix + name)
                if not item.isfile() or item.size > 32 * 1024**2:
                    raise ValueError('Invalid generated source member')
                target = tree / 'src/nvidia/generated' / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(item).read())
        boot.extract_assets(tree, output)
    (output / 'gsp_ga10x.bin').write_bytes(gsp)
    (output / 'v67-ga102-610.43.03.bin').write_bytes(payload)
    (output / 'NVIDIA-LICENSE').write_bytes(license_bytes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('USB_READY'))
    parser.add_argument('--cache', type=Path, default=ROOT / 'audit-inputs')
    parser.add_argument('--mode', choices=('probe', 'unlock'), default='probe')
    parser.add_argument('--execute', action='store_true', help='Explicitly create execute=1 config and include firmware')
    parser.add_argument('--ss1-full', action='store_true', help='Optional SS1=8 experiment; default preserves SS1')
    args = parser.parse_args()
    if args.execute and args.mode != 'unlock':
        parser.error('--execute requires --mode unlock')
    if args.ss1_full and not args.execute:
        parser.error('--ss1-full requires --execute')
    # Never overwrite an existing destination, let alone a mounted USB root.
    if args.output.exists():
        parser.error('--output must be a new ordinary folder; existing paths are refused')
    prebuilt = ROOT / 'uefi/prebuilt'
    manifest = json.loads((prebuilt / 'manifest.json').read_text())
    name = 'GA102-3080TI-PROBE.efi' if args.mode == 'probe' else 'GA102-3080TI-UNLOCK.efi'
    binary = prebuilt / name
    if hashlib.sha256(binary.read_bytes()).hexdigest() != manifest['sha256'][name]:
        raise ValueError('Prebuilt EFI checksum mismatch')
    card = None
    try:
        card = detect_card()
    except (OSError, ValueError, subprocess.SubprocessError):
        if args.execute:
            raise RuntimeError('Could not uniquely detect the RTX 3080 Ti board (10de:2208, '
                               'subsystem 10de:1535, 12288 MiB) via nvidia-smi. No execute folder was created.') from None
    # Stage everything before exposing a reviewable output directory.
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='rtx3080ti-stage-', dir=args.output.parent) as directory:
        stage = Path(directory) / 'payload'
        bootdir = stage / 'EFI/BOOT'
        settings = stage / 'ga102-3080ti'
        bootdir.mkdir(parents=True)
        settings.mkdir()
        shutil.copy2(binary, bootdir / 'BOOTX64.EFI')
        config = f'# GA102 RTX 3080 Ti experimental EFI {VERSION}; execute is opt-in\n'
        if card:
            target, vendor, device = card
            config += f'target={target}\nboard_vendor=0x{vendor:04x}\nboard_device=0x{device:04x}\n'
        config += f'execute={int(args.execute)}\nss1_full={int(args.ss1_full)}\nwindows=1\n'
        (settings / 'config.txt').write_text(config, encoding='ascii')
        if args.execute:
            firmware(args.cache, settings / 'firmware')
        shutil.copy2(ROOT / 'uefi/README.md', stage / 'README.md')
        shutil.copy2(ROOT / 'licenses/GPL-2.0.txt', settings / 'GPL-2.0.txt')
        shutil.copy2(ROOT / 'uefi/GNU-EFI-NOTICE.txt', settings / 'GNU-EFI-NOTICE.txt')
        stage.rename(args.output)
    print(f'Created {args.output.resolve()}')
    print('No drive was formatted and no Windows/UEFI settings were changed.')
    print('Mode: ' + ('EXPERIMENTAL EXECUTE; RTX 3080 Ti hardware validation pending' if args.execute else 'read-only PCI probe; no GPU firmware execution'))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, tarfile.TarError) as error:
        raise SystemExit(str(error)) from None
