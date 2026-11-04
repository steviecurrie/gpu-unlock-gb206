/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Pure decoders for the FEAT/FUSE speed-select words. No kernel or hardware
 * dependency, so the same code is unit-tested in userspace (selftest.c).
 *
 * SS0/SS1 hold one 4-bit field per unit: bit 3 = override enable, bits 2:0 =
 * selector (0 = full rate). FEAT_READOUT_1 holds nine 3-bit fields: 0 = full.
 * The encoding is the documented Ada/Ampere one; on Blackwell the nonzero
 * readout does not follow the same rate law (FMLA32=3 measures ~half, not
 * 1/8), so these decoders report fields, not rates.
 *
 * FEAT_READOUT_1 is NOT nine contiguous 3-bit fields. The fields sit at these
 * bit offsets, with gaps at bit 6 and bits 27:25 (measured on GB202; the
 * contiguous decode yields out-of-range 6s):
 *
 *     0, 3, 7, 10, 13, 16, 19, 22, 28
 */
#ifndef GPU_PEEK_DECODE_H
#define GPU_PEEK_DECODE_H

#define GPU_PEEK_SS_UNITS    8
#define GPU_PEEK_READOUT_UNITS 9

static inline void gp_decode_readout1(unsigned int value,
				      unsigned int out[GPU_PEEK_READOUT_UNITS])
{
	static const unsigned char shift[GPU_PEEK_READOUT_UNITS] = {
		0, 3, 7, 10, 13, 16, 19, 22, 28
	};
	unsigned int field;

	for (field = 0; field < GPU_PEEK_READOUT_UNITS; field++)
		out[field] = (value >> shift[field]) & 0x7u;
}

static inline void gp_decode_ss(unsigned int value,
				unsigned int out[GPU_PEEK_SS_UNITS])
{
	unsigned int field;

	for (field = 0; field < GPU_PEEK_SS_UNITS; field++)
		out[field] = (value >> (4 * field)) & 0xfu;
}

/* bit 3 of a SS nibble */
static inline unsigned int gp_ss_enabled(unsigned int nibble)
{
	return (nibble >> 3) & 1u;
}

/* bits 2:0 of a SS nibble */
static inline unsigned int gp_ss_selector(unsigned int nibble)
{
	return nibble & 0x7u;
}

/* PLM fields, per the public Falcon v4 layout. */
static inline unsigned int gp_plm_read_mask(unsigned int plm)
{
	return plm & 0xfu;
}

static inline unsigned int gp_plm_write_mask(unsigned int plm)
{
	return (plm >> 4) & 0xfu;
}

/* Bit 3 of the write mask = PL3 (heavy secure) may write. */
static inline int gp_plm_write_pl3_only(unsigned int plm)
{
	return gp_plm_write_mask(plm) == 0x8u;
}

#endif
