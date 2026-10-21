#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""FP32-only copy of benchmark_torch.py. Does not unlock or configure the GPU."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import statistics
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--size', type=int, default=4096)
    parser.add_argument('--iterations', type=int, default=20)
    parser.add_argument('--samples', type=int, default=5)
    parser.add_argument('--device', type=int, default=0)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 256 <= args.size <= 8192 or min(args.iterations, args.samples) < 1:
        parser.error('size must be 256..8192; iterations/samples must be positive')
    if args.output.exists():
        parser.error('Refusing to overwrite an existing measurement')
    try:
        import torch
    except ImportError:
        parser.exit(2, 'Run with the existing CUDA PyTorch Python.\n')
    if not torch.cuda.is_available():
        parser.exit(2, 'CUDA PyTorch and an NVIDIA GPU are required.\n')
    if not 0 <= args.device < torch.cuda.device_count():
        parser.error('Invalid CUDA device index')
    torch.cuda.set_device(args.device)
    torch.manual_seed(20261019)
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cuda.matmul.allow_fp16_reduced_precision_reduction = False
    torch.backends.cuda.matmul.allow_bf16_reduced_precision_reduction = False
    if torch.backends.cuda.matmul.allow_tf32 or torch.get_float32_matmul_precision() != 'highest':
        parser.exit(2, 'FP32 matmul must run with TF32 disabled.\n')
    props = torch.cuda.get_device_properties(args.device)
    report = dict(kind='FP32 benchmark only; no unlock performed by this script',
                  started_utc=datetime.now(timezone.utc).isoformat(),
                  benchmark_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  os=platform.system(),
                  torch_version=torch.__version__, cuda_version=torch.version.cuda,
                  device=props.name, capability=list(torch.cuda.get_device_capability()),
                  memory_mib=props.total_memory // 2**20,
                  size=args.size, iterations=args.iterations, samples=args.samples,
                  warmup_iterations=10, seed=20261019,
                  allow_tf32=torch.backends.cuda.matmul.allow_tf32,
                  float32_matmul_precision=torch.get_float32_matmul_precision(),
                  reduced_precision_reduction=False,
                  correctness_relative_l2_limit=1e-5, results={})
    failed = False
    with torch.inference_mode():
        for label, dtype in [('FP32', torch.float32)]:
            try:
                a = torch.randn(args.size, args.size, device='cuda', dtype=dtype) * 0.1
                b = torch.randn(args.size, args.size, device='cuda', dtype=dtype) * 0.1
                out = torch.empty_like(a)
                for _ in range(10):
                    torch.mm(a, b, out=out)
                torch.cuda.synchronize()
                times = []
                for _ in range(args.samples):
                    start, end = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
                    start.record()
                    for _ in range(args.iterations):
                        torch.mm(a, b, out=out)
                    end.record()
                    end.synchronize()
                    times.append(start.elapsed_time(end) / args.iterations)
                # Independent FP64 reference for 32 rows, all K terms and columns.
                # The FP32-specific tolerance is recorded, not a full-output check.
                reference = a[:32].double() @ b.double()
                measured = out[:32].double()
                relative_l2 = ((measured - reference).norm() / reference.norm().clamp_min(1e-30)).item()
                finite = bool(torch.isfinite(out).all().item())
                passed = finite and relative_l2 < report['correctness_relative_l2_limit']
                failed |= not passed
                median_ms = statistics.median(times)
                report['results'][label] = dict(median_ms=median_ms,
                    tflops=2 * args.size**3 / (median_ms / 1000) / 1e12,
                    sample_ms=times, finite=finite, relative_l2_sample=relative_l2,
                    checked_rows=32, correctness_sample_passed=passed)
                del a, b, out, reference, measured
            except (RuntimeError, NotImplementedError) as error:
                failed = True
                report['results'][label] = dict(error=str(error))
    report['note'] = ('Boot state is established outside this script. Compare the same '
                      'software/workload and similar clocks/power; no application-wide '
                      'speedup or long-term stability guarantee.')
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    text = json.dumps(report, indent=2)
    with args.output.open('x', encoding='utf-8') as stream:
        stream.write(text + '\n')
    print(text)
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
