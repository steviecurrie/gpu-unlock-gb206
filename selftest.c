/* SPDX-License-Identifier: GPL-2.0-only */
/* Unit tests for decode.h. Runs anywhere; needs no GPU. */
#include <stdio.h>
#include <stdlib.h>

#include "decode.h"

static int failures;

static void expect_field(const char *what, unsigned int got, unsigned int want)
{
	if (got != want) {
		fprintf(stderr, "FAIL %s: got %u want %u\n", what, got, want);
		failures++;
	}
}

static void test_readout_gb202_stock(void)
{
	/* Measured on RTX 5090 GB202, driver 615.71.09. */
	static const unsigned int want[GPU_PEEK_READOUT_UNITS] =
		{ 0, 0, 3, 0, 3, 3, 0, 3, 0 };
	unsigned int got[GPU_PEEK_READOUT_UNITS];
	unsigned int i;

	gp_decode_readout1(0x00c36180u, got);
	for (i = 0; i < GPU_PEEK_READOUT_UNITS; i++)
		expect_field("readout1 gb202", got[i], want[i]);
}

/*
 * Regression: the contiguous decode (bit 3*field) is wrong and produces 6s
 * for 0x00c36180. If someone "simplifies" the shift table, this fails.
 */
static void test_readout_is_not_contiguous(void)
{
	unsigned int got[GPU_PEEK_READOUT_UNITS];
	unsigned int i, sixes = 0;

	for (i = 0; i < GPU_PEEK_READOUT_UNITS; i++)
		got[i] = (0x00c36180u >> (3 * i)) & 0x7u;
	for (i = 0; i < GPU_PEEK_READOUT_UNITS; i++)
		sixes += got[i] == 6;
	expect_field("naive decode 6s", sixes, 4);
	expect_field("naive decode out of range", sixes > 0, 1);
}

static void test_readout_full_rate(void)
{
	/* RTX PRO 6000 GB202 reads all zeros. */
	unsigned int got[GPU_PEEK_READOUT_UNITS];
	unsigned int i;

	gp_decode_readout1(0u, got);
	for (i = 0; i < GPU_PEEK_READOUT_UNITS; i++)
		expect_field("readout1 full", got[i], 0);
}

static void test_unlock_payload(void)
{
	unsigned int got[GPU_PEEK_SS_UNITS];
	unsigned int i;

	gp_decode_ss(0x88888888u, got);
	for (i = 0; i < GPU_PEEK_SS_UNITS; i++) {
		expect_field("ss enable", gp_ss_enabled(got[i]), 1);
		expect_field("ss selector", gp_ss_selector(got[i]), 0);
	}
}

static void test_plm(void)
{
	/* Stock GB202/GB203M: PL3-only writes, readable by all. */
	expect_field("plm stock read", gp_plm_read_mask(0xfffffb8fu), 0xf);
	expect_field("plm stock write", gp_plm_write_mask(0xfffffb8fu), 0x8);
	expect_field("plm stock pl3only", gp_plm_write_pl3_only(0xfffffb8fu), 1);
	/* After FLR: PL2 + PL3 may write (0x8 | 0x4). */
	expect_field("plm flr write", gp_plm_write_mask(0x80842bcfu), 0xc);
	expect_field("plm flr pl3only", gp_plm_write_pl3_only(0x80842bcfu), 0);
	/* Open: all levels. */
	expect_field("plm open", gp_plm_write_mask(0xffffffffu), 0xf);
}

int main(void)
{
	test_readout_gb202_stock();
	test_readout_is_not_contiguous();
	test_readout_full_rate();
	test_unlock_payload();
	test_plm();

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	printf("ok\n");
	return EXIT_SUCCESS;
}
