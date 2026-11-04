// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpu_peek_gb206: sample the Blackwell speed-select block.
 *
 * Derived from gpu_peek.c in JustAResearcher/gpu-unlock-ada (GPL-2.0), trimmed
 * to what a GB20x card needs and extended with an optional write ladder.
 *
 * Every read is a single ioread32 from BAR0. Nothing here writes config space,
 * flash, firmware, clocks, power, or resets. The optional writetest=1 does
 * exactly three volatile BAR0 writes with readback, then puts the original
 * values back; a reboot clears anything left behind.
 *
 * The purpose is to answer the one question nobody has published for GB206:
 * does the FEAT block hold the same values as GB202, and does the write gate
 * still drop host writes?
 *
 * Usage:
 *   insmod gpu_peek_gb206.ko target=0000:08:00.0
 *   insmod gpu_peek_gb206.ko target=0000:08:00.0 writetest=1
 *   dmesg | grep gpu_peek_gb206
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>

#include "decode.h"

/* Highest register we touch is 0x00824140; the smallest NVIDIA BAR0 is 16 MiB. */
#define BAR_MAP_LEN 0x00900000u

static char *target;
module_param(target, charp, 0444);
MODULE_PARM_DESC(target, "PCI address to sample, e.g. 0000:08:00.0 (default: every NVIDIA display function)");

static bool writetest;
module_param(writetest, bool, 0444);
MODULE_PARM_DESC(writetest, "run the three-write volatility ladder with readback and restore (default: off)");

struct probe {
	const char *name;
	u32 offset;
};

static const struct probe probes[] = {
	{ "PMC_BOOT_0 (chip id)",            0x00000000 },
	{ "GFW PLM",                         0x00118128 },
	{ "GFW boot progress",               0x00118234 },
	{ "GFW falcon CPUCTL",               0x00110100 },
	{ "WPR_CFG PLM",                     0x001fa7cc },
	{ "FUSECTRL",                        0x00820000 },
	{ "FUSE_EN_SW_OVERRIDE",             0x00820040 },
	{ "FUSE_SS_PLM",                     0x008200fc },
	{ "FUSE_SS_DP",                      0x00820224 },
	{ "FUSE_FEAT_OVR_DIS",               0x008203f0 },
	{ "SECURE_FEAT_OVR_QUADRO_WR_SEC",   0x0082038c },
	{ "FUSE_SS_FFMA",                    0x0082059c },
	{ "FUSE_SS_FMLA16",                  0x008207d4 },
	{ "FUSE_SS_FMLA32",                  0x008207d8 },
	{ "FUSE_SS_IMLA0",                   0x008207dc },
	{ "FUSE_SS_IMLA1",                   0x008207e0 },
	{ "FUSE_SS_IMLA2",                   0x008207e4 },
	{ "FUSE_SS_IMLA3",                   0x008207e8 },
	{ "FUSE_SS_IMLA4",                   0x008207ec },
	{ "FEAT_OVR_ECC_PLM",                0x00823800 },
	{ "FEAT_OVR_PLM",                    0x00823804 },
	{ "FEAT_OVR_QUADRO",                 0x00823808 },
	{ "FEAT_OVR_ECC",                    0x0082380c },
	{ "FEAT_OVR_ECC_1",                  0x00823810 },
	{ "FEAT_READOUT_0",                  0x00823814 },
	{ "FEAT_READOUT_1",                  0x00823818 },
	{ "SS0 (FEAT_OVR_SM_SS0)",           0x0082381c },
	{ "SS1 (FEAT_OVR_SM_SS1)",           0x00823820 },
	{ "FEAT_OVR_ROW_REMAP",              0x00823824 },
	{ "FEAT_READOUT_2",                  0x00823828 },
	{ "FEAT_OVR_ECC_2",                  0x0082382c },
	{ "SM_ISSUE_RATE_MODIFIER",          0x00504204 },
};

#define PROBE_COUNT ARRAY_SIZE(probes)

#define SS0_OFFSET         0x0082381cu
#define SS1_OFFSET         0x00823820u
#define FEAT_PLM_OFFSET    0x00823804u

static void dump_decodes(struct pci_dev *pdev, void __iomem *bar)
{
	unsigned int readout[GPU_PEEK_READOUT_UNITS];
	unsigned int nibbles[GPU_PEEK_SS_UNITS];
	unsigned int i, value;
	char buf[128];
	int n;

	value = ioread32(bar + 0x00823818);
	gp_decode_readout1(value, readout);
	n = 0;
	for (i = 0; i < GPU_PEEK_READOUT_UNITS; i++)
		n += scnprintf(buf + n, sizeof(buf) - n, " %u", readout[i]);
	dev_info(&pdev->dev,
		 "gpu_peek_gb206: FEAT_READOUT_1 fields (imla0 fmla16 fmla32 ffma imla1 imla2 imla3 imla4 DP) =%s\n",
		 buf);

	value = ioread32(bar + SS0_OFFSET);
	gp_decode_ss(value, nibbles);
	n = 0;
	for (i = 0; i < GPU_PEEK_SS_UNITS; i++)
		n += scnprintf(buf + n, sizeof(buf) - n, " %x", nibbles[i]);
	dev_info(&pdev->dev, "gpu_peek_gb206: SS0 nibbles (bit3=enable, bits2:0=selector) =%s\n", buf);

	value = ioread32(bar + FEAT_PLM_OFFSET);
	dev_info(&pdev->dev,
		 "gpu_peek_gb206: FEAT_OVR_PLM read_mask=%#x write_mask=%#x pl3_only=%u\n",
		 gp_plm_read_mask(value), gp_plm_write_mask(value),
		 gp_plm_write_pl3_only(value));
}

/*
 * Three volatile writes with readback, then the originals restored.
 *
 * Order and payload are the prior-art ladder (SS0 = 0x88888888 enables every
 * selector at full rate, SS1 = 0x8 does the ninth, then FEAT_OVR_PLM opened).
 * A dropped write reads back unchanged and returns nothing, so readback is the
 * only evidence. If a write does stick we log it loudly and restore anyway.
 */
static void write_ladder(struct pci_dev *pdev, void __iomem *bar)
{
	struct step {
		const char *name;
		u32 offset;
		u32 want;
	};
	struct step steps[] = {
		{ "SS0",          SS0_OFFSET,       0x88888888u },
		{ "SS1",          SS1_OFFSET,       0x00000008u },
		{ "FEAT_OVR_PLM", FEAT_PLM_OFFSET,  0xffffffffu },
	};
	u32 saved[ARRAY_SIZE(steps)];
	unsigned int i, stuck = 0;

	for (i = 0; i < ARRAY_SIZE(steps); i++)
		saved[i] = ioread32(bar + steps[i].offset);

	dev_info(&pdev->dev,
		 "gpu_peek_gb206: WRITETEST begin (volatile, restored below, reboot clears)\n");

	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		u32 after;

		iowrite32(steps[i].want, bar + steps[i].offset);
		after = ioread32(bar + steps[i].offset);
		if (after == steps[i].want) {
			stuck++;
			dev_warn(&pdev->dev,
				 "gpu_peek_gb206: WRITETEST %s ACCEPTED wrote=%#x readback=%#x (was %#x)\n",
				 steps[i].name, steps[i].want, after, saved[i]);
		} else {
			dev_info(&pdev->dev,
				 "gpu_peek_gb206: WRITETEST %s dropped wrote=%#x readback=%#x\n",
				 steps[i].name, steps[i].want, after);
		}
	}

	/* Restore in reverse, unconditionally. */
	for (i = ARRAY_SIZE(steps); i-- > 0;) {
		u32 after;

		iowrite32(saved[i], bar + steps[i].offset);
		after = ioread32(bar + steps[i].offset);
		if (after != saved[i])
			dev_err(&pdev->dev,
				"gpu_peek_gb206: WRITETEST %s RESTORE FAILED wanted=%#x readback=%#x\n",
				steps[i].name, saved[i], after);
	}

	dev_info(&pdev->dev,
		 "gpu_peek_gb206: WRITETEST end: %u/3 writes accepted, all originals restored\n",
		 stuck);
}

static void peek_one(struct pci_dev *pdev)
{
	void __iomem *bar;
	unsigned int i;

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    !pci_resource_start(pdev, 0)) {
		dev_info(&pdev->dev, "gpu_peek_gb206: BAR0 unusable, skipped\n");
		return;
	}

	/* Short map: the FEAT/FUSE block lives in the first 16 MiB of every
	 * NVIDIA BAR0, so a 9 MiB window covers it without mapping 32 GiB. */
	bar = pci_iomap(pdev, 0, BAR_MAP_LEN);
	if (!bar) {
		dev_err(&pdev->dev, "gpu_peek_gb206: BAR0 map failed, skipped\n");
		return;
	}

	dev_info(&pdev->dev,
		 "gpu_peek_gb206: driver=%s device=%04x:%04x BAR0=%#llx\n",
		 pdev->driver ? pdev->driver->name : "none",
		 pdev->vendor, pdev->device,
		 (unsigned long long)pci_resource_start(pdev, 0));

	for (i = 0; i < PROBE_COUNT; i++)
		dev_info(&pdev->dev, "gpu_peek_gb206: %#010x %-32s = %#010x\n",
			 probes[i].offset, probes[i].name,
			 ioread32(bar + probes[i].offset));

	dump_decodes(pdev, bar);

	if (writetest)
		write_ladder(pdev, bar);

	pci_iounmap(pdev, bar);
}

static int __init gpu_peek_gb206_init(void)
{
	struct pci_dev *pdev = NULL;

	if (target && *target) {
		unsigned int domain, bus, slot, func;

		if (sscanf(target, "%x:%x:%x.%x", &domain, &bus, &slot, &func) != 4 ||
		    slot > 0x1f || func > 7) {
			pr_err("gpu_peek_gb206: bad target \"%s\"\n", target);
			return -EINVAL;
		}
		pdev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(slot, func));
		if (!pdev) {
			pr_err("gpu_peek_gb206: no device at \"%s\"\n", target);
			return -ENODEV;
		}
		peek_one(pdev);
		pci_dev_put(pdev);
		return 0;
	}

	while ((pdev = pci_get_device(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, pdev)))
		if ((pdev->class >> 16) == PCI_BASE_CLASS_DISPLAY)
			peek_one(pdev);
	return 0;
}

static void __exit gpu_peek_gb206_exit(void) { }

module_init(gpu_peek_gb206_init);
module_exit(gpu_peek_gb206_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only sampler and optional volatile write ladder for the Blackwell speed-select block");
MODULE_VERSION("0.1.0");
