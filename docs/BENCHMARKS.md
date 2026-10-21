# Benchmark methodology and results

## Controlled comparison

These measurements are 4096×4096 dense matrix multiplications, with 20
iterations per sample and five samples per precision. Each test warms up with
10 GEMMs, uses CUDA events for timing, and seeds PyTorch with `20261019`.
FP32 ran with TF32 disabled and `float32_matmul_precision="highest"`.
FP16/BF16 reduced-precision reductions were disabled.

The per-sample time is the CUDA-event elapsed time divided by the 20 iterations.
Reported TFLOPS use the median of the five per-sample times and
`2 × N³ / seconds / 10¹²` (an FMA counts as two operations).

| State | FP32 TFLOPS | FP16 TFLOPS | BF16 TFLOPS |
|---|---:|---:|---:|
| Windows baseline | 23.376184626874288 | 62.378977875091174 | 70.74330371845537 |
| Applied | 23.369443357313255 | 113.62347609081499 | 115.18362905273638 |

The exact medians, per-sample times, accuracy results, and provenance hashes are
in [`../results/benchmark-results.json`](../results/benchmark-results.json).

## Accuracy checks

After timing, each script compares the first 32 output rows (all K terms and
all columns) against an independent FP64 matrix product. The relative L2 error
is `norm(measured - reference) / max(norm(reference), 1e-30)`. It also requires
every element of the full output matrix to be finite. These are sampled-row
accuracy checks, not full-matrix comparisons against an FP64 reference.

- FP32 passes only when the full output is finite and relative L2 error is
  **strictly less than 1e-5**.
- FP16 and BF16 pass only when the full output is finite and relative L2 error
  is **strictly less than 0.02**.

All six recorded checks passed. The measured relative L2 errors were
1.147595475736877e-6 (FP32), 0.0002072672638303298 (FP16), and
0.0016607262746021895 (BF16).

## Run conditions and limitations

The applied record is from the same applied-configuration Windows boot. The
Windows baseline record is from a new Windows boot with the experiment USB
absent. Wallpaper Engine was paused and the assistant application window was
minimized in both states; the
baseline context also records additional windows minimized.

Each state recorded six consecutive preflight samples with GPU utilization at
0%, and the final observation immediately before each test process also read
0%. For the reduced-precision block, each state additionally recorded an
earlier nonzero utilization observation before the final 0% reading (100% in
the applied record and 49% in the Windows record). These point samples do not
establish GPU idleness during the computation.

This is one recorded boot per state, not a repeated-independent-boot study.
Clocks and power were not held fixed; there is no long-term stability result.
The measurements describe this dense GEMM workload only and are not an
application-wide speedup or a prediction of application performance.

An earlier FP32 observation of approximately 10 TFLOPS was confounded by
display load. It is excluded from this controlled comparison, not erased.

## Portable benchmark source

The measured test code is in `bench/benchmark_fp32.py` and
`bench/benchmark_torch.py`. Run from the project root with a Python environment
that has CUDA-enabled PyTorch and an available NVIDIA GPU:

```text
python bench/benchmark_fp32.py --size 4096 --iterations 20 --samples 5 --device 0 --output results/fp32-run.json
python bench/benchmark_torch.py --size 4096 --iterations 20 --samples 5 --device 0 --output results/fp16-bf16-run.json
```

The scripts accept caller-selected output paths and do not configure GPU
clocks, power, firmware, or application state. The FP32 script refuses to
overwrite an existing output; the FP16/BF16 script writes to the selected
output path if provided. Choose unused paths when running new measurements.

The packaged source preserves the measured matrix generation, precision
controls, warmups, timing loops, and accuracy tests. Report-only absolute
interpreter paths and raw `nvidia-smi` output were removed. The hashes in the
results JSON identify the original measured source and result files; the
packaged scripts therefore do not have the same byte hashes as the original
source.

The FP32 script no longer accepts the local `--boot-time-utc` metadata argument
or treats the removed `nvidia-smi` invocation's return code as a benchmark
failure. Record the boot identity and validate the driver/GPU separately as
required by `docs/AGENT_HANDOFF.md`; neither packaged script establishes the
applied state. These changes do not alter the numerical workload or accuracy
thresholds used for the published measurements.
