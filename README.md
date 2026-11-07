# GB206 (RTX 5060 Ti) speed-select probe

Read-only register dump of the Blackwell tensor speed-select block on
`10de:2d04`, plus a three-write volatility ladder. Short version of the
finding: **GB206 is capped in exactly the same way as GB202, and host writes
are dropped the same way.**

As far as I can find, this is the first published register data for a GB206 part. Prior public
measurements cover GB202 (RTX 5090) and GB203M (5090 Laptop) only.

## Design

Small change, one sensible build path, so it's recorded here rather than in a
separate spec.

**What was wanted.** Not an unlock — there is no path to one. The contribution
is the missing data point: does the cap and its gate look the same on the
smallest Blackwell consumer die as on the largest?

**Approach taken.** Vendor `diagnostics/peek/gpu_peek.c` from
JustAResearcher/gpu-unlock-ada (GPL-2.0) — an already-proven read-only
sampler — and trim it. Rejected alternatives: writing a fresh probe (no
benefit over 305 tested lines), and using the driver's `GPU_EXEC_REG_OPS`
ioctl path (needs an RM client and driver-source introspection for the same
read-only data).

**Trimmed out:** the VBIOS/FWSEC parser and `#include` of `vbios_610.c`, the
SEC2 block (Blackwell has no SEC2; the reads just return `0xbadf` markers),
and the Ampere-specific "unlock succeeded" summary predicates.

**Added:** a `decode.h` with unit-tested field decoders, and an optional
`writetest=1` ladder.

**Non-goals / anti-goals.** No flash access, no firmware, no config space, no
FLR, no fuse writes, no clock or power registers. If the ladder leaves the card
in an unexpected state that would be a failure, not a feature.

**Why it's safe enough to run on a display GPU.** Every read is one
`ioread32` from a dev register window. The ladder is three volatile writes to
FEAT/FUSE words, each read back and then restored to its saved value in the
same run; a reboot clears anything left behind. The 9 MiB BAR0 window covers
the FEAT block without mapping a 32 GiB resizable BAR.

## Run

```bash
make                    # builds the module and runs the userspace self-tests
sudo ./run.sh           # read-only
sudo MODE=write ./run.sh
```

Output lands in `gb206-<timestamp>-<mode>.log`.

## Results

Card: GB206 `10de:2d04`, RTX 5060 Ti 16 GB, driver 615.71.10, kernel
6.12.113, BAR0 `0xf8000000`. Values identical across a module reload in the
same boot.

| Register | Offset | GB206 | GB202 (5090) | GB203M | Read |
|---|---|---:|---:|---:|---|
| `PMC_BOOT_0` | `0x000000` | `0x1b6000a1` | `0x21120000` | `0x1b3000a1` | chip id |
| `FUSE_SS_FMLA16` | `0x8207d4` | `0` | `0` | `0` | full |
| **`FUSE_SS_FMLA32`** | `0x8207d8` | **`3`** | **`3`** | **`3`** | capped |
| `FUSE_SS_IMLA1` | `0x8207e0` | `3` | `3` | `3` | capped |
| `FUSE_SS_IMLA2` | `0x8207e4` | `3` | `3` | `3` | capped |
| `FUSE_SS_IMLA4` | `0x8207ec` | `3` | `3` | `3` | capped |
| `FUSE_SS_DP` / `FFMA` / `IMLA0` / `IMLA3` | — | `0` | `0` | `0` | full |
| `FUSE_FEAT_OVR_DIS` | `0x8203f0` | `0` | `0` | `0` | override path enabled |
| `FUSE_EN_SW_OVERRIDE` | `0x820040` | `0` | `0` | `0` | floorsweep only |
| `FUSE_SS_PLM` | `0x8200fc` | `0xfffffbff` | — | `0xfffffbff` | not the effective gate |
| **`FEAT_OVR_PLM`** | `0x823804` | **`0xfffffb8f`** | **`0xfffffb8f`** | **`0xfffffb8f`** | **PL3-only writes** |
| `FEAT_READOUT_1` | `0x823818` | **`0x00c36180`** | **`0x00c36180`** | `0x00c36180` | identical |
| `FEAT_READOUT_0` | `0x823814` | `0x00000233` | `0x00000233` | — | ECC flags |
| `FEAT_OVR_ECC` | `0x82380c` | `0x00888888` | `0x00888888` | — | ECC, not speed |
| `FEAT_OVR_ECC_1` | `0x823810` | `0xaa2a2aaa` | `0x002a2aaa` | — | per-card variance |
| `SECURE_FEAT_OVR_QUADRO_WR_SEC` | `0x82038c` | `2` | `2` | — | per-card |
| `SM_ISSUE_RATE_MODIFIER` | `0x504204` | `5` | `5` | — | writable, inert |
| `GFW falcon CPUCTL` | `0x110100` | `0xbadf5720` | `0xbadf4100` | — | inaccessible / absent |

Decoded `FEAT_READOUT_1` (`0x00c36180`), field order
`imla0 fmla16 fmla32 ffma imla1 imla2 imla3 imla4 DP`:

```
  0 0 3 0 3 3 0 3 0
```

Decoded `FEAT_OVR_PLM` (`0xfffffb8f`): read mask `0xf` (all levels),
write mask `0x8` (**PL3 only**), source enable `0xfffff`.

### Write ladder

| Step | Wrote | Read back | Result |
|---|---:|---:|---|
| `SS0` | `0x88888888` | `0x06277073` | dropped |
| `SS1` | `0x00000008` | `0x00025005` | dropped |
| `FEAT_OVR_PLM` | `0xffffffff` | `0xfffffb8f` | dropped |

`0/3 writes accepted, all originals restored.`

## What this establishes

1. **The cap is uniform across the Blackwell consumer line.** GB206 has the
   same `FUSE_SS_FMLA32 = 3` as GB202 and GB203M. The `FEAT_READOUT_1` word is
   **byte-identical** (`0x00c36180`) to the 5090's, on a die with less than
   half the SMs. Product segmentation, applied per-SKU from the same fuse map.
2. **The gate is identical.** `FEAT_OVR_PLM = 0xfffffb8f` = writes from PL3
   only, same as GB202/GB203M. Host (PL0) writes are dropped, and the drop is
   silent — reads back unchanged, no error.
3. **The override path itself is open, and only the privilege stops it.**
   `FUSE_FEAT_OVR_DIS = 0` and `FUSE_EN_SW_OVERRIDE = 0`, matching GB202. The
   selector override is not fused off; `FEAT_OVR_PLM` is the wall.
4. **No SEC2/Falcon story on GB206 either.** `GFW falcon CPUCTL` returns the
   `0xbadf` decode marker, same as GB202. Consistent with the FSP/FMC boot
   chain and the absence of a `booter_load` for GB20x.

## What this does *not* show

- Not a pre-bind dump. The register reads are with `nvidia` bound. GB202
  measurements show the fuse mirrors are boot-static, so this should equal the
  pre-bind values, but that is an inference here, not a measurement.
- Not a test of the FLR window. The PL2-writable window after a function-level
  reset was not probed; that needs a reset, which is out of scope for a
  display card.
- Not an unlock, and not evidence that one is possible. The missing primitive
  is still PL3 code execution in the FSP/FMC path.

## Side finding: `gpu_peek.c` decodes `FEAT_READOUT_1` wrong

The upstream sampler decodes the field as nine contiguous 3-bit values
(`(value >> (3 * field)) & 7`). The fields are not contiguous: they sit at bit
offsets `0, 3, 7, 10, 13, 16, 19, 22, 28`, with gaps at bit 6 and bits 27:25.
The contiguous decode prints `0 0 6 0 6 6 0 6 0` for `0x00c36180`, with 6s
outside the documented 3-bit range. `decode.h` uses the real offsets and
`selftest.c` fails if anyone "simplifies" it back.

## Measured rate, RTX 5060 Ti (GB206)

`probe/mma_rate.py` (vendored from `phiraml/gpu-unlock-blackwell`, MIT)
measures the one ratio the unlock targets, on this card, same boot, same power
cap (180 W, the card's factory limit).

| Mode | Peak TFLOPS | vs uncapped |
|---|---:|---:|
| FP16 in, FP16 accumulate | 86.27 | 1.000 |
| FP16 in, FP32 accumulate | 48.33 | **0.560** |
| BF16 in, FP32 accumulate | 48.76 | **0.565** |

Receipt: `probe/5060ti-stock.json`.

The absolute number is the stronger evidence than the ratio. NVIDIA's table
lists the 5060 Ti at **94.9 TFLOPS** FP16 tensor with FP16 accumulate and
**47.4** with FP32 accumulate. The capped path measures **48.3** — matching the
published halved figure almost exactly. The uncapped path measures 86.3, i.e.
91% of its own spec number, which is why the ratio reads 0.56 rather than 0.50.

Expected ceiling if a payload ever clears the gate: **~1.79×** on this path.

Caveat on ratio comparisons: the denominator moves with shape and how well the
uncapped path saturates. `phiraml` measured 0.68 on a 5090, `ai.rs` 0.51 on a
different probe, this 0.56. All three agree the cap is real; none of them is
"the" ratio. Quote the absolute TFLOPS and the driver-reported fuse, not the
ratio alone.

## Cross-card comparison

GB206 is measured in this repo. GB203M values are from ElXreno's public
`gpu-unlock-ada` post (2026-10-25). The GB202 column was removed: its values
had no traceable source. "varies" means the value changes across boots or
suspends with no write from us (see the resume notes in the wiki).

| Register | GB203M (5090 Laptop) | GB206 (5060 Ti) |
|---|---|---|
| `PMC_BOOT_0` | `0x1b3000a1` | `0x1b6000a1` |
| `FEAT_OVR_PLM` | `0xfffffb8f` | `0xfffffb8f` |
| `FUSE_SS_DP` | 0 | 0 |
| `FUSE_SS_FFMA` | 0 | 0 |
| `FUSE_SS_FMLA16` | 0 | 0 |
| `FUSE_SS_FMLA32` | 3 | 3 |
| `FUSE_SS_IMLA0` | 0 | 0 |
| `FUSE_SS_IMLA1` | 3 | 3 |
| `FUSE_SS_IMLA2` | 3 | 3 |
| `FUSE_SS_IMLA3` | 0 | 0 |
| `FUSE_SS_IMLA4` | 3 | 3 |
| `FEAT_READOUT_1` | `0x00c36180` | `0x00c36180` |
| `FEAT_OVR_ECC` | not reported | `0x00888888` |
| `FEAT_OVR_ECC_1` | not reported | `0xaa2a2aaa` |
| `FEAT_OVR_SM_SS0` | `0x02000000` / `0x12000000` | varies (`0x06275053` to `0x07377053`) |
| `FEAT_OVR_SM_SS1` | `0x00002004` | varies (`0x00025005` to `0x00065005`) |
| `SM_ISSUE_RATE_MODIFIER` | 5 | 5 |
| Measured FP32-acc / FP16-acc | not measured | 0.56x to 0.57x |

The FUSE and PLM words match on both dies. A GB202 comparison needs a GB202
card and a fresh read.

## Related work

- `JustAResearcher/gpu-unlock-ada` issue #12 — the GB202/GB203M reconnaissance
  this extends.
- `shuttie/bf16-unlock-blackwell-research` — 87 experiments; establishes the
  missing primitive as PL3 code execution.
- `phiraml/gpu-unlock-blackwell` — measurement harness and port analysis, dated
  one day before issue #12. Its register-of-assets evidence is sound; its
  conclusion (blocker = a missing decrypted GB20x debug booter) is superseded,
  because Blackwell has no SEC2 stage to boot a booter in.

Closed separately: the debug-signed FMC is **not** an alternative route to PL3.
Debug and prod ship byte-identical, fully-encrypted `UCODE_IMAGE`; only the
96-byte `UCODE_SIG` differs, and there is no multi-signature table to downgrade
through. Detail in `CONTRIBUTION.md` §3.

## Files

| File | What |
|---|---|
| `decode.h` | field decoders, no kernel deps |
| `selftest.c` | userspace unit tests for the decoders |
| `gpu_peek_gb206.c` | the module |
| `run.sh` | load, capture, unload |
| `*.log` | captured runs |
| `probe/mma_rate.py` | issue-rate harness, vendored from `phiraml/gpu-unlock-blackwell` (MIT) |
| `probe/5060ti-stock.json` | measured baseline receipt for this card |
| `CONTRIBUTION.md` | ready-to-post writeups |
