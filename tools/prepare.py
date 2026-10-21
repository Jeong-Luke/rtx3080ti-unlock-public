#!/usr/bin/env python3
"""Verify local inputs, extract pinned assets, and record offline evidence.

No downloads, installation, GPU access, or execution of extracted binaries.
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]

def strip_c_comments(source):
    return re.sub(r'/\*.*?\*/|//[^\n]*',
                  lambda match: ''.join('\n' if char == '\n' else ' ' for char in match.group(0)),
                  source, flags=re.S)


def matching_delimiter(source, start, opening, closing):
    depth = 0
    quote = None
    escaped = False
    for offset in range(start, len(source)):
        char = source[offset]
        if quote:
            if escaped:
                escaped = False
            elif char == '\\':
                escaped = True
            elif char == quote:
                quote = None
        elif char in ('"', "'"):
            quote = char
        elif char == opening:
            depth += 1
        elif char == closing:
            depth -= 1
            if not depth:
                return offset
    raise ValueError(f'unmatched {opening!r} in pinned HAL source')


def ga102_hal_index(chip_source):
    code = strip_c_comments(chip_source)
    matches = re.findall(
        r'\belse\s+if\s*\(\s*arch\s*==\s*0x17\s*&&\s*impl\s*==\s*0x2\s*\)'
        r'\s*\{\s*pChipHal\s*->\s*__nvoc_HalVarIdx\s*=\s*(0[xX][0-9a-fA-F]+|\d+)\s*;\s*\}',
        code)
    if len(matches) != 1:
        raise ValueError('pinned source does not uniquely map GA102 to a ChipHal index')
    return int(matches[0], 0)


def eval_hal_condition(node, chip_index):
    if isinstance(node, ast.Expression):
        return eval_hal_condition(node.body, chip_index)
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, bool)):
        return node.value
    if isinstance(node, ast.Name) and node.id == 'chipHal_HalVarIdx':
        return chip_index
    if isinstance(node, ast.BoolOp):
        values = [eval_hal_condition(value, chip_index) for value in node.values]
        if isinstance(node.op, ast.And):
            return all(values)
        if isinstance(node.op, ast.Or):
            return any(values)
    if isinstance(node, ast.Compare) and len(node.ops) == len(node.comparators) == 1:
        left = eval_hal_condition(node.left, chip_index)
        right = eval_hal_condition(node.comparators[0], chip_index)
        if isinstance(node.ops[0], ast.Eq):
            return left == right
    if isinstance(node, ast.BinOp):
        left = eval_hal_condition(node.left, chip_index)
        right = eval_hal_condition(node.right, chip_index)
        if isinstance(node.op, ast.BitAnd):
            return left & right
        if isinstance(node.op, ast.LShift):
            return left << right
        if isinstance(node.op, ast.RShift):
            return left >> right
    raise ValueError('unsupported expression in pinned HAL dispatch condition')


def ga102_dispatch(hal_source, chip_source, method, implementation):
    chip_index = ga102_hal_index(chip_source)
    code = strip_c_comments(hal_source)
    assignment = re.compile(
        rf'\bpThis\s*->\s*__{re.escape(method)}__\s*=\s*&{re.escape(implementation)}\s*;')
    assignments = list(assignment.finditer(code))
    if len(assignments) != 1:
        raise ValueError(f'{method} does not have one exact {implementation} dispatch assignment')
    selected = assignments[0]
    conditions = []
    for match in re.finditer(r'\bif\s*\(', code):
        open_paren = code.find('(', match.start())
        close_paren = matching_delimiter(code, open_paren, '(', ')')
        brace = close_paren + 1
        while brace < len(code) and code[brace].isspace():
            brace += 1
        if brace >= len(code) or code[brace] != '{':
            continue
        close_brace = matching_delimiter(code, brace, '{', '}')
        if not (brace < selected.start() < close_brace):
            continue
        condition = code[open_paren + 1:close_paren]
        if 'chipHal_HalVarIdx' not in condition:
            continue
        normalized = re.sub(
            r'\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', condition)
        normalized = normalized.replace('&&', ' and ').replace('||', ' or ')
        try:
            value = eval_hal_condition(ast.parse('(' + normalized + ')', mode='eval'), chip_index)
        except (SyntaxError, ValueError):
            continue
        if value:
            conditions.append(condition.strip())
    if len(conditions) != 1:
        raise ValueError(f'{method} is not dispatched to {implementation} for GA102')
    return dict(method=method, implementation=implementation,
                line=code[:selected.start()].count('\n') + 1,
                chip_hal_index=chip_index, dispatch_condition=conditions[0])


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, default=ROOT/'audit-inputs')
    args = parser.parse_args()
    manifest = json.loads((ROOT/'firmware/artifacts-610.43.03.json').read_text())
    inputs = {}
    for item in manifest['source_artifacts']:
        if 'filename' not in item:
            continue
        path = args.inputs/item['filename']
        actual = digest(path)
        if actual != item['sha256']:
            raise SystemExit(f'Input SHA-256 mismatch: {path}')
        inputs[item['filename']] = actual
        print(f'Input SHA-256 OK: {path.name}', flush=True)
    source_parent = ROOT/'research/source'
    source_parent.mkdir(parents=True, exist_ok=True)
    source_name = 'NVIDIA-kernel-module-source-610.43.03'
    with tarfile.open(args.inputs/(source_name+'.tar.xz')) as archive:
        archive.extractall(source_parent, filter='data')
    source = source_parent/source_name
    assets = ROOT/'build/firmware'
    for script, path in [('generate_v67.py', args.inputs/'nvidia-utils-610.43.03-1-x86_64_v3.pkg.tar.zst'),
                         ('extract_boot_assets.py', source)]:
        subprocess.run([sys.executable, str(ROOT/'firmware'/script), str(path), str(assets)], check=True)

    hal_path = source/'src/nvidia/generated/g_kernel_gsp_nvoc.c'
    chip_path = source/'src/nvidia/generated/g_chips2halspec_nvoc.c'
    hal = hal_path.read_text()
    chips = chip_path.read_text()
    findings = []
    for name, symbol in [('kgspGetBinArchiveBooterLoadUcode', 'kgspGetBinArchiveBooterLoadUcode_GA102'),
                         ('kgspGetBinArchiveBooterUnloadUcode', 'kgspGetBinArchiveBooterUnloadUcode_GA102'),
                         ('kgspGetBinArchiveGspRmBoot', 'kgspGetBinArchiveGspRmBoot_GA102'),
                         ('kgspGetPrescrubbedTopFbSize', 'kgspGetPrescrubbedTopFbSize_13afc5'),
                         ('kgspGetMinWprHeapSizeMB', 'kgspGetMinWprHeapSizeMB_d2c3c5')]:
        finding = ga102_dispatch(hal, chips, name, symbol)
        finding['source'] = str(hal_path.relative_to(ROOT))
        findings.append(finding)
    bundle = (assets/'booter_load_ga102.bin').read_bytes()
    fields = struct.unpack('<18I', bundle[:72])
    # Wire format: magic/version, 14 descriptor values, image/signature sizes.
    report = dict(status='offline-assets-verified; RTX 3080 Ti hardware unvalidated',
                  experimental_version='0.4.0-rtx3080ti',
                  target=dict(name='RTX 3080 Ti', pci_id='10de:2208',
                              subsystem_vendor='10de', subsystem_device='1535',
                              memory_mib=12288),
                  hardware_unvalidated=True,
                  hardware_validation_status='unvalidated',
                  hardware_execution_tested=False,
                  windows_driver='617.42 (unvalidated)',
                  vbios='94.02.71.00.83 (unvalidated)',
                  inputs=inputs, dispatch=findings,
                  booter=dict(ucode_id=fields[12], engine_mask=fields[13],
                              num_signatures=fields[14], signature_bytes=fields[15],
                              image_bytes=fields[16], bundle_signature_bytes=fields[17]),
                  output_sha256={p.name:digest(p) for p in sorted(assets.iterdir()) if p.is_file()},
                  unresolved=['RTX 3080 Ti physical BootROM/fuse acceptance; hardware unvalidated',
                              'board PROM contains parseable FWSEC-FRTS',
                              'RTX 3080 Ti SS0 effect and preservation through FLR',
                              'post-FLR handoff to Windows 617.42 on VBIOS 94.02.71.00.83 (not hardware tested)',
                              'per-operation SS0 bit mapping not established'])
    (ROOT/'build/offline-audit.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Assets and GA102 source HAL dispatch evidence recorded; RTX 3080 Ti hardware remains unvalidated.')


if __name__ == '__main__':
    main()
