# Ready-to-post writeups

Prior art consulted: `Zagitta/gpu-unlock`, `JustAResearcher/gpu-unlock-ada`
(including issue #12), `phiraml/gpu-unlock-blackwell`, and
`shuttie/bf16-unlock-blackwell-research`. One correction to the phiraml framing
if it comes up: its "0.68x on a 5090" uses a 334.7 TF/s FP16-accumulate
reference, where a tighter probe gets 441.4. Its own ratios (0.676 / 0.664) are
correct as measured, but the denominator is low at those shapes, so 0.68
understates the gap — the cap is closer to 2x. Quote the ratio, not the word
"0.68x".

---

## 1. Comment for `JustAResearcher/gpu-unlock-ada` issue #12

> **GB206 (RTX 5060 Ti) register dump: cap and gate identical to GB202**
>
> Same probe, smaller die. `10de:2d04`, RTX 5060 Ti 16 GB, driver 615.71.10,
> kernel 6.12.113, BAR0 `0xf8000000`. Read-only `ioread32` from BAR0 with
> `nvidia` bound, plus a three-write ladder that restores every original in the
> same run. Nothing touched flash, firmware, config space or reset.
>
> ```
> PMC_BOOT_0                          0x00000000 = 0x1b6000a1
> FUSE_FEAT_OVR_DIS                   0x008203f0 = 0x00000000
> FUSE_EN_SW_OVERRIDE                 0x00820040 = 0x00000000
> FUSE_SS_PLM                         0x008200fc = 0xfffffbff
> FUSE_SS_FMLA16                      0x008207d4 = 0x00000000
> FUSE_SS_FMLA32                      0x008207d8 = 0x00000003
> FUSE_SS_IMLA1                       0x008207e0 = 0x00000003
> FUSE_SS_IMLA2                       0x008207e4 = 0x00000003
> FUSE_SS_IMLA4                       0x008207ec = 0x00000003
> FEAT_OVR_PLM                        0x00823804 = 0xfffffb8f
> FEAT_OVR_ECC                        0x0082380c = 0x00888888
> FEAT_READOUT_0                      0x00823814 = 0x00000233
> FEAT_READOUT_1                      0x00823818 = 0x00c36180
> SS0 (FEAT_OVR_SM_SS0)               0x0082381c = 0x06277073
> SS1 (FEAT_OVR_SM_SS1)               0x00823820 = 0x00025005
> SECURE_FEAT_OVR_QUADRO_WR_SEC       0x0082038c = 0x00000002
> SM_ISSUE_RATE_MODIFIER              0x00504204 = 0x00000005
> GFW falcon CPUCTL                   0x00110100 = 0xbadf5720
> ```
>
> Decoded:
>
> - `FEAT_READOUT_1` (`0x00c36180`) → `0 0 3 0 3 3 0 3 0`, field order
>   `imla0 fmla16 fmla32 ffma imla1 imla2 imla3 imla4 DP`. **Byte-identical to
>   the 5090's word.**
> - `FEAT_OVR_PLM` (`0xfffffb8f`) → read mask `0xf`, write mask `0x8`
>   (PL3 only), source enable `0xfffff`. Same as GB202/GB203M.
> - `SS0` nibbles → `3 7 0 7 7 2 6 0`; every bit 3 clear, so nothing is
>   overridden and the fused selectors are in force.
>
> Write ladder (same boot, values identical before and after a module reload):
>
> | Step | Wrote | Read back | Result |
> |---|---:|---:|---|
> | `SS0` | `0x88888888` | `0x06277073` | dropped |
> | `SS1` | `0x00000008` | `0x00025005` | dropped |
> | `FEAT_OVR_PLM` | `0xffffffff` | `0xfffffb8f` | dropped |
>
> The cap holds on all three parts measured so far (GB202, GB203M, GB206), so
> it is not a big-die quirk: same fuse words, same readout word, same PL3-only
> gate, same silent drop, and the override path is still open at the fuse level
> (`FUSE_FEAT_OVR_DIS = 0`). Only the privilege level stands in the way.
>
> Paired issue-rate measurement on the same card, `phiraml`'s `mma_rate.py`,
> 180 W factory cap, best-of-3 across five shapes:
>
> | Mode | Peak TFLOPS | vs uncapped |
> |---|---:|---:|
> | FP16 in, FP16 accumulate | 86.27 | 1.000 |
> | FP16 in, FP32 accumulate | 48.33 | 0.560 |
> | BF16 in, FP32 accumulate | 48.76 | 0.565 |
>
> NVIDIA lists the 5060 Ti at 94.9 / 47.4 for those first two modes, so the
> capped path measures 48.3 against a published 47.4 — the halved figure almost
> exactly. Ceiling if a payload ever clears the gate: ~1.79x on this path.
>
> Caveats: the register dump was taken with `nvidia` bound (fuse mirrors are
> boot-static on GB202, so it should match pre-bind, but that's an inference
> here), and the write ladder is three volatile writes with immediate restore,
> not a cold-boot experiment.
>
> Tool is GPL-2.0, derived from `diagnostics/peek/gpu_peek.c` here. Source,
> the captured runs and the measurement receipt:
> https://github.com/steviecurrie/gpu-unlock-gb206 — happy to send any of it
> as a PR instead.

---

## 2. Issue for `JustAResearcher/gpu-unlock-ada`: `gpu_peek` decodes `FEAT_READOUT_1` wrong

Title: `gpu_peek: FEAT_READOUT_1 is not nine contiguous 3-bit fields`

> `peek_cluster()` decodes `FEAT_READOUT_1` as `(value >> (3 * field)) & 7` for
> `field` 0..8. The fields are not contiguous — they sit at bit offsets
> `0, 3, 7, 10, 13, 16, 19, 22, 28`, with gaps at bit 6 and bits 27:25.
>
> On a stock `0x00c36180` the contiguous decode prints
> `0 0 6 0 6 6 0 6 0`: four out-of-range 6s, because the shift slips one bit
> from `fmla32` onward. The correct decode is `0 0 3 0 3 3 0 3 0`.
>
> That also explains the first follow-up on #12 describing "values 6 are out
> of range" — it's the decoder, not the card. Anyone reading the sampled line
> as a fuse mirror gets a wrong answer for every field from `fmla32` up.
>
> Fix is a shift table:
>
> ```c
> static const unsigned char shift[9] = { 0, 3, 7, 10, 13, 16, 19, 22, 28 };
> out[field] = (value >> shift[field]) & 0x7u;
> ```
>
> Suggested guard: a userspace unit test asserting `0x00c36180` decodes to
> `0 0 3 0 3 3 0 3 0` and that no field exceeds 7. Reproduced on GB206
> (`10de:2d04`), and the same word is `0x00c36180` on GB202/GB203M, so it
> affects every Blackwell dump, not just one card.

---

## 3. Follow-up: the debug-signed FMC is not an alternative route

Context: `phiraml/gpu-unlock-blackwell` lists
`g_bindata_kgspGetBinArchiveGspRmFmcGfwDebugSigned_GB202.c` in its bindata
inventory, and the Ada route depends on a *debug* build being minable. The FMC
runs at PL3, which is exactly the level the Blackwell unlock is missing — so a
minable debug FMC would be the missing primitive without needing the SEC2
booter at all. Worth checking directly. It doesn't work.

Source: the 615.71.10 open module, `src/nvidia/generated/`.

```
g_bindata_kgspGetBinArchiveGspRmFmcGfwDebugSigned_GB202.c
  UCODE_IMAGE  287304 B   entropy 7.99-8.00 bits/byte
g_bindata_kgspGetBinArchiveGspRmFmcGfwProdSigned_GB202.c
  UCODE_IMAGE  287304 B   entropy 7.99-8.00 bits/byte
```

Decoded and compared:

| Asset | Debug | Prod |
|---|---|---|
| `UCODE_IMAGE` | 287,304 B | **byte-identical to debug** |
| `UCODE_HASH` | 48 B (`b54e23af…`) | **byte-identical to debug** |
| `UCODE_SIG` | 96 B (`c9907005…`) | differs (`207634c2…`) |

So "DebugSigned" changes the signature only. Same ciphertext, same SHA-384.

Two things follow, and neither is good news:

1. **The image is encrypted, not plaintext.** Entropy is 8.00 bits/byte
   uniformly, including the first 64 KiB and across the 478 KB
   `ConcatenatedFMC_GB202` `UCODE_IMAGE_PROD`. There is no cleartext preamble
   and no RISC-V code to disassemble. (Contrast the Falcon BooterLoad bundles,
   which ship `IMAGE_PROD` in plaintext and only the signature is checked.)
2. **There is no signature-downgrade surface.** `gpu_unlock_booter_patch()`
   works by choosing among `num_sigs` signature slots inside the image, indexed
   by the fuse version read from the card. The FMC ships one 96-byte signature
   next to the image, with no slot table and no `patch_loc`. There is nothing
   to select between.

Conclusion: the FMC does not give PL3 code execution for free, and the
debug/prod split is not a lever. It needs the key, same as the negative result
already recorded in `shuttie`'s FMC key search. This closes the "maybe the
debug build is the way in" idea — the debug build is the same bytes.
