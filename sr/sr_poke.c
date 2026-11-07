// SPDX-License-Identifier: GPL-2.0-only
/*
 * Patch one field of GspFwSRMeta on the stack, after the driver fills it and
 * before portMemCopy. Probe address is kgspPrepareSuspendResumeData_TU102+0x16b
 * in the 615.71.10 module (mov -0x130(%rbp), %rdi). Refuses to arm if those
 * bytes are not that instruction.
 */
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/uaccess.h>

static unsigned long addr;
module_param(addr, ulong, 0444);
MODULE_PARM_DESC(addr, "runtime address of the copy instruction");

static char *field = "flags";
module_param(field, charp, 0444);
MODULE_PARM_DESC(field, "flags or size");

static unsigned long val = 2;
module_param(val, ulong, 0444);
MODULE_PARM_DESC(val, "value to write; size 0 means half the stock size");

static struct kprobe kp;

static int pre(struct kprobe *p, struct pt_regs *regs)
{
	u64 magic;

	magic = *(u64 *)(regs->bp - 0x120);
	if (magic != 0x8a3bb9e6c6c39d93ULL) {
		pr_err("sr_poke: magic %#llx, not patching\n", magic);
		return 0;
	}
	if (strcmp(field, "flags") == 0) {
		u32 *fp = (u32 *)(regs->bp - 0x80);

		pr_info("sr_poke: flags %#x -> %#lx\n", *fp, val);
		*fp = (u32)val;
	} else if (strcmp(field, "size") == 0) {
		u64 *sp = (u64 *)(regs->bp - 0x108);
		u64 n = val ? val : (*sp / 2);

		pr_info("sr_poke: size %#llx -> %#llx\n", *sp, n);
		*sp = n;
	} else if (strcmp(field, "ptr") == 0) {
		u64 *pp = (u64 *)(regs->bp - 0x110);
		u64 old = *pp;

		if (val) {
			pr_info("sr_poke: ptr %#llx -> %#lx\n", old, val);
			*pp = val;
		} else {
			pr_info("sr_poke: ptr %#llx (read only, val=0)\n", old);
		}
	} else {
		pr_err("sr_poke: unknown field %s\n", field);
	}
	return 0;
}

static int __init sr_poke_init(void)
{
	u8 expect[7] = { 0x48, 0x8b, 0xbd, 0xd0, 0xfe, 0xff, 0xff };
	u8 got[7];

	if (!addr)
		return -EINVAL;
	if (copy_from_kernel_nofault(got, (void *)addr, sizeof(got)))
		return -EFAULT;
	if (memcmp(got, expect, sizeof(got))) {
		pr_err("sr_poke: bytes at %#lx are %*ph, not the copy insn\n",
		       addr, (int)sizeof(got), got);
		return -ENOEXEC;
	}
	kp.addr = (kprobe_opcode_t *)addr;
	kp.pre_handler = pre;
	return register_kprobe(&kp);
}

static void __exit sr_poke_exit(void)
{
	unregister_kprobe(&kp);
}

module_init(sr_poke_init);
module_exit(sr_poke_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("One-field poke of GspFwSRMeta before the resume copy");
