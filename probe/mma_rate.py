#!/usr/bin/env python3
"""Measure the tensor FMA-into-FP32 issue rate of a Blackwell consumer GPU.

The gpu-unlock family targets one fused restriction: on GeForce silicon the
FP16/BF16 tensor FMA that accumulates into FP32 issues at a fraction of the
FP16-accumulate rate. Clearing the FEAT_OVR_PLM gate and writing speed
selectors SS0/SS1 removes that fraction.

So the question "what is there to unlock on my card?" is answered by one
ratio: fp16->fp32 accumulate against fp16->fp16 accumulate measured on the
same card, same boot, same power cap. A ratio near 1.0 means nothing to
unlock. A ratio near 0.5 means a full doubling is sitting behind the fuse.

Usage:
    python mma_rate.py                 # human table
    python mma_rate.py --json out.json # also write a receipt

Pure torch, no other dependencies.
"""

from __future__ import annotations

import argparse
import json
import platform
import subprocess
import time

import torch

SIZES = [
    (4096, 4096, 4096),
    (8192, 8192, 4096),
    (8192, 4096, 8192),
    (16384, 8192, 4096),
    (16384, 16384, 2048),
]

MODES = [
    ("fp16_acc_fp16", torch.float16, True, "FP16 inputs, FP16 accumulate"),
    ("fp16_acc_fp32", torch.float16, False, "FP16 inputs, FP32 accumulate"),
    ("bf16_acc_fp32", torch.bfloat16, False, "BF16 inputs, FP32 accumulate"),
]


def set_acc(flag: bool) -> None:
    torch.backends.cuda.matmul.allow_fp16_accumulation = flag


def bench(dtype, m: int, n: int, k: int, fp16acc: bool, iters: int = 40) -> float:
    """Best-of-3 TFLOP/s for one mode, or 0.0 if it is unsupported."""
    try:
        a = torch.randn(m, k, device="cuda", dtype=dtype)
        b = torch.randn(k, n, device="cuda", dtype=dtype)
        prev = torch.backends.cuda.matmul.allow_fp16_accumulation
        set_acc(fp16acc)
        for _ in range(8):
            c = a @ b
        torch.cuda.synchronize()
        best = 0.0
        for _ in range(3):
            t = time.perf_counter()
            for _ in range(iters):
                c = a @ b
            torch.cuda.synchronize()
            best = max(best, 2 * m * n * k / ((time.perf_counter() - t) / iters) / 1e12)
        set_acc(prev)
        del a, b, c
        torch.cuda.empty_cache()
        return best
    except RuntimeError as error:
        print(f"  skipped {m}x{n}x{k}: {error}")
        return 0.0


def device_info() -> dict:
    p = torch.cuda.get_device_properties(0)


    nv = {}
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,driver_version,power.limit,power.max_limit,pcie.link.gen.current",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=10, check=False,
        ).stdout.strip()
        if out:
            name, driver, limit, max_limit, gen = [x.strip() for x in out.split(",")]
            nv = {"driver": driver, "power_limit_w": float(limit),
                  "power_max_w": float(max_limit), "pcie_gen": gen}
    except (OSError, ValueError, subprocess.SubprocessError):
        pass

    return {
        "gpu": torch.cuda.get_device_name(0),
        "sm_count": p.multi_processor_count,
        "compute_capability": f"{p.major}.{p.minor}",
        "torch": torch.__version__,
        "cuda": torch.version.cuda,
        "platform": platform.platform(),
        **nv,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", type=str, default=None, help="write the receipt here")
    args = ap.parse_args()

    info = device_info()
    print(json.dumps(info, indent=2))
    print()

    if not torch.cuda.is_available():
        print("no CUDA device")
        return 1

    if not hasattr(torch.backends.cuda.matmul, "allow_fp16_accumulation"):
        print("this torch has no allow_fp16_accumulation toggle; build a newer torch")
        return 1

    print(f"{'mode':16s} {'shape':>18s} {'TFLOP/s':>10s}")
    print("-" * 46)
    peaks: dict[str, float] = {}
    grid: list[dict] = []
    for key, dtype, acc, label in MODES:
        best, best_shape = 0.0, None
        for m, n, k in SIZES:
            tf = bench(dtype, m, n, k, acc)
            grid.append({"mode": key, "m": m, "n": n, "k": k, "tflops": round(tf, 2)})
            print(f"{key:16s} {f'{m}x{n}x{k}':>18s} {tf:10.2f}")
            if tf > best:
                best, best_shape = tf, f"{m}x{n}x{k}"
        peaks[key] = best
        if best:
            print(f"{key:16s} {'PEAK':>18s} {best:10.2f}   ({best_shape})")
        print()

    ref = peaks.get("fp16_acc_fp16", 0.0)
    ratios = {k: (v / ref if ref else None) for k, v in peaks.items()}

    print("peak summary")
    for k, v in peaks.items():
        r = ratios[k]
        print(f"  {k:16s} {v:8.2f} TFLOP/s" + (f"   {r:5.3f} x fp16->fp16" if r else ""))

    halved = ratios.get("fp16_acc_fp32", 1.0)
    print()
    if halved and ref:
        if halved < 0.80:
            print(f"VERDICT: fp16->fp32 accumulate runs at {halved:.2f}x the fp16->fp16 rate.")
            print("         The FMLA32 rate fuse is present and active on this silicon.")
            print(f"         Ceiling if a booter payload clears it: ~{ref / peaks['fp16_acc_fp32']:.2f}x on this path.")
        else:
            print(f"VERDICT: ratio {halved:.2f} is near 1.0; no meaningful fuse on this silicon.")
    else:
        print("VERDICT: inconclusive, reference mode did not measure")

    if args.json:
        receipt = {
            "device": info,
            "peaks_tflops": {k: round(v, 3) for k, v in peaks.items()},
            "ratio_vs_fp16_acc_fp16": {k: (round(v, 4) if v else None) for k, v in ratios.items()},
            "grid": grid,
            "note": "Issue-rate measurement. Large-K GEMM keeps store traffic small; "
                    "best-of-3 of the swept shapes. Not an application speedup.",
        }
        with open(args.json, "w") as fh:
            json.dump(receipt, fh, indent=2)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
