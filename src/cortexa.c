/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2016  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements debugging functionality specific to ARM
 * the Cortex-A9 core.  This should be generic to ARMv7-A as it is
 * implemented according to the "ARMv7-A Architecture Reference Manual",
 * ARM doc DDI0406C.
 *
 * Cache line length is from Cortex-A9 TRM, may differ for others.
 * Janky reset code is for Zynq-7000 which disconnects the DP from the JTAG
 * scan chain during reset.
 */
#include "general.h"
#include "exception.h"
#include "jtagtap.h"
#include "jtag_scan.h"
#include "adiv5.h"
#include "target.h"
#include "command.h"
#include "gdb_packet.h"
#include "cortexm.h"
#include "morse.h"

#include <unistd.h>

static char cortexa_driver_str[] = "ARM Cortex-A";

/* Signals returned by cortexa_halt_wait() */
#define SIGINT 2
#define SIGTRAP 5
#define SIGSEGV 11
#define SIGLOST 29

static bool cortexa_attach(target *t);
static void cortexa_detach(target *t);
static void cortexa_halt_resume(target *t, bool step);

static void cortexa_regs_read(target *t, void *data);
static void cortexa_regs_write(target *t, const void *data);
static void cortexa_regs_read_internal(target *t);
static void cortexa_regs_write_internal(target *t);

static void cortexa_reset(target *t);
static int cortexa_halt_wait(target *t);
static void cortexa_halt_request(target *t);

static int cortexa_set_hw_bp(target *t, uint32_t addr, uint8_t len);
static int cortexa_clear_hw_bp(target *t, uint32_t addr, uint8_t len);
static uint32_t bp_bas(uint32_t addr, uint8_t len);

static void apb_write(target *t, uint16_t reg, uint32_t val);
static uint32_t apb_read(target *t, uint16_t reg);
static void write_gpreg(target *t, uint8_t regno, uint32_t val);
static uint32_t read_gpreg(target *t, uint8_t regno);

struct cortexa_priv {
	uint32_t base;
	ADIv5_AP_t *apb;
	ADIv5_AP_t *ahb;
	struct {
		uint32_t r[16];
		uint32_t cpsr;
		uint32_t fpscr;
		uint64_t d[16];
	} reg_cache;
	unsigned hw_breakpoint_max;
	unsigned hw_breakpoint[16];
	uint32_t bpc0;
	bool mmu_fault;
};

/* This may be specific to Cortex-A9 */
#define CACHE_LINE_LENGTH        (8*4)

/* Debug APB registers */
#define DBGDIDR                  0

#define DBGDTRRX                 32 /* DCC: Host to target */
#define DBGITR                   33

#define DBGDSCR                  34
#define DBGDSCR_TXFULL           (1 << 29)
#define DBGDSCR_INSTRCOMPL       (1 << 24)
#define DBGDSCR_EXTDCCMODE_STALL (1 << 20)
#define DBGDSCR_EXTDCCMODE_FAST  (2 << 20)
#define DBGDSCR_EXTDCCMODE_MASK  (3 << 20)
#define DBGDSCR_HDBGEN           (1 << 14)
#define DBGDSCR_ITREN            (1 << 13)
#define DBGDSCR_INTDIS           (1 << 11)
#define DBGDSCR_UND_I            (1 << 8)
#define DBGDSCR_SDABORT_L        (1 << 6)
#define DBGDSCR_MOE_MASK         (0xf << 2)
#define DBGDSCR_MOE_HALT_REQ     (0x0 << 2)
#define DBGDSCR_RESTARTED        (1 << 1)
#define DBGDSCR_HALTED           (1 << 0)

#define DBGDTRTX                 35 /* DCC: Target to host */

#define DBGDRCR                  36
#define DBGDRCR_CSE              (1 << 2)
#define DBGDRCR_RRQ              (1 << 1)
#define DBGDRCR_HRQ              (1 << 0)

#define DBGBVR(i)                (64+(i))
#define DBGBCR(i)                (80+(i))
#define DBGBCR_INST_MISMATCH     (4 << 20)
#define DBGBCR_BAS_ANY           (0xf << 5)
#define DBGBCR_BAS_LOW_HW        (0x3 << 5)
#define DBGBCR_BAS_HIGH_HW       (0xc << 5)
#define DBGBCR_EN                (1 << 0)

/* Instruction encodings for accessing the coprocessor interface */
#define MCR 0xee000010
#define MRC 0xee100010
#define CPREG(coproc, opc1, rt, crn, crm, opc2) \
	(((opc1) << 21) | ((crn) << 16) | ((rt) << 12) | \
        ((coproc) << 8) | ((opc2) << 5) | (crm))

/* Debug registers CP14 */
#define DBGDTRRXint CPREG(14, 0, 0, 0, 5, 0)
#define DBGDTRTXint CPREG(14, 0, 0, 0, 5, 0)

/* Address translation registers CP15 */
#define PAR         CPREG(15, 0, 0, 7, 4, 0)
#define ATS1CPR     CPREG(15, 0, 0, 7, 8, 0)

/* Cache management registers CP15 */
#define ICIALLU     CPREG(15, 0, 0, 7, 5, 0)
#define DCCIMVAC    CPREG(15, 0, 0, 7, 14, 1)
#define DCCMVAC     CPREG(15, 0, 0, 7, 10, 1)

/* Thumb mode bit in CPSR */
#define CPSR_THUMB               (1 << 5)

/* GDB register map / target description */
static const char tdesc_cortex_a[] =
	"<?xml version=\"1.0\"?>"
	"<!DOCTYPE feature SYSTEM \"gdb-target.dtd\">"
	"<target>"
	"  <architecture>arm</architecture>"
	"  <feature name=\"org.gnu.gdb.arm.core\">"
	"    <reg name=\"r0\" bitsize=\"32\"/>"
	"    <reg name=\"r1\" bitsize=\"32\"/>"
	"    <reg name=\"r2\" bitsize=\"32\"/>"
	"    <reg name=\"r3\" bitsize=\"32\"/>"
	"    <reg name=\"r4\" bitsize=\"32\"/>"
	"    <reg name=\"r5\" bitsize=\"32\"/>"
	"    <reg name=\"r6\" bitsize=\"32\"/>"
	"    <reg name=\"r7\" bitsize=\"32\"/>"
	"    <reg name=\"r8\" bitsize=\"32\"/>"
	"    <reg name=\"r9\" bitsize=\"32\"/>"
	"    <reg name=\"r10\" bitsize=\"32\"/>"
	"    <reg name=\"r11\" bitsize=\"32\"/>"
	"    <reg name=\"r12\" bitsize=\"32\"/>"
	"    <reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
	"    <reg name=\"lr\" bitsize=\"32\" type=\"code_ptr\"/>"
	"    <reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
	"    <reg name=\"cpsr\" bitsize=\"32\"/>"
	"  </feature>"
	"  <feature name=\"org.gnu.gdb.arm.vfp\">"
	"    <reg name=\"fpscr\" bitsize=\"32\"/>"
	"    <reg name=\"d0\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d1\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d2\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d3\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d4\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d5\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d6\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d7\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d8\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d9\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d10\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d11\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d12\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d13\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d14\" bitsize=\"64\" type=\"float\"/>"
	"    <reg name=\"d15\" bitsize=\"64\" type=\"float\"/>"
	"  </feature>"
	"</target>";

static void apb_write(target *t, uint16_t reg, uint32_t val)
{
	struct cortexa_priv *priv = t->priv;
	ADIv5_AP_t *ap = priv->apb;
	uint32_t addr = priv->base + 4*reg;
	adiv5_ap_write(ap, ADIV5_AP_TAR, addr);
	adiv5_dp_low_access(ap->dp, ADIV5_LOW_WRITE, ADIV5_AP_DRW, val);
}

static uint32_t apb_read(target *t, uint16_t reg)
{
	struct cortexa_priv *priv = t->priv;
	ADIv5_AP_t *ap = priv->apb;
	uint32_t addr = priv->base + 4*reg;
	adiv5_ap_write(ap, ADIV5_AP_TAR, addr);
	adiv5_dp_low_access(ap->dp, ADIV5_LOW_READ, ADIV5_AP_DRW, 0);
	return adiv5_dp_low_access(ap->dp, ADIV5_LOW_READ, ADIV5_DP_RDBUFF, 0);
}

static uint32_t va_to_pa(target *t, uint32_t va)
{
	struct cortexa_priv *priv = t->priv;
	write_gpreg(t, 0, va);
	apb_write(t, DBGITR, MCR | ATS1CPR);
	apb_write(t, DBGITR, MRC | PAR);
	uint32_t par = read_gpreg(t, 0);
	if (par & 1)
		priv->mmu_fault = true;
	uint32_t pa = (par & ~0xfff) | (va & 0xfff);
	DEBUG("%s: VA = 0x%08X, PAR = 0x%08X, PA = 0x%08X\n", __func__, va, par, pa);
	return pa;
}

static void cortexa_mem_read(target *t, void *dest, uint32_t src, size_t len)
{
	/* Clean cache before reading */
	for (uint32_t cl = src & ~(CACHE_LINE_LENGTH-1);
	     cl < src + len; cl += CACHE_LINE_LENGTH) {
		write_gpreg(t, 0, cl);
		apb_write(t, DBGITR, MCR | DCCMVAC);
	}

	ADIv5_AP_t *ahb = ((struct cortexa_priv*)t->priv)->ahb;
	adiv5_mem_read(ahb, dest, va_to_pa(t, src), len);
}

static void cortexa_slow_mem_read(target *t, void *dest, uint32_t src, size_t len)
{
	struct cortexa_priv *priv = t->priv;
	unsigned words = (len + (src & 3) + 3) / 4;
	uint32_t dest32[words];

	/* Set r0 to aligned src address */
	write_gpreg(t, 0, src & ~3);

	/* Switch to fast DCC mode */
	uint32_t dbgdscr = apb_read(t, DBGDSCR);
	dbgdscr = (dbgdscr & ~DBGDSCR_EXTDCCMODE_MASK) | DBGDSCR_EXTDCCMODE_FAST;
	apb_write(t, DBGDSCR, dbgdscr);

	apb_write(t, DBGITR, 0xecb05e01); /* ldc 14, cr5, [r0], #4 */
	/* According to the ARMv7-A ARM, in fast mode, the first read from
	 * DBGDTRTX is  supposed to block until the instruction is complete,
	 * but we see the first read returns junk, so it's read here and
	 * ignored. */
	apb_read(t, DBGDTRTX);

	for (unsigned i = 0; i < words; i++)
		dest32[i] = apb_read(t, DBGDTRTX);

	memcpy(dest, (uint8_t*)dest32 + (src & 3), len);

	/* Switch back to stalling DCC mode */
	dbgdscr = (dbgdscr & ~DBGDSCR_EXTDCCMODE_MASK) | DBGDSCR_EXTDCCMODE_STALL;
	apb_write(t, DBGDSCR, dbgdscr);

	if (apb_read(t, DBGDSCR) & DBGDSCR_SDABORT_L) {
		/* Memory access aborted, flag a fault */
		apb_write(t, DBGDRCR, DBGDRCR_CSE);
		priv->mmu_fault = true;
	} else {
		apb_read(t, DBGDTRTX);
	}
}

static void cortexa_mem_write(target *t, uint32_t dest, const void *src, size_t len)
{
	/* Clean and invalidate cache before writing */
	for (uint32_t cl = dest & ~(CACHE_LINE_LENGTH-1);
	     cl < dest + len; cl += CACHE_LINE_LENGTH) {
		write_gpreg(t, 0, cl);
		apb_write(t, DBGITR, MCR | DCCIMVAC);
	}
	ADIv5_AP_t *ahb = ((struct cortexa_priv*)t->priv)->ahb;
	adiv5_mem_write(ahb, va_to_pa(t, dest), src, len);
}

static void cortexa_slow_mem_write_bytes(target *t, uint32_t dest, const uint8_t *src, size_t len)
{
	struct cortexa_priv *priv = t->priv;

	/* Set r13 to dest address */
	write_gpreg(t, 13, dest);

	while (len--) {
		write_gpreg(t, 0, *src++);
		apb_write(t, DBGITR, 0xe4cd0001); /* strb r0, [sp], #1 */
		if (apb_read(t, DBGDSCR) & DBGDSCR_SDABORT_L) {
			/* Memory access aborted, flag a fault */
			apb_write(t, DBGDRCR, DBGDRCR_CSE);
			priv->mmu_fault = true;
			return;
		}
	}
}

static void cortexa_slow_mem_write(target *t, uint32_t dest, const void *src, size_t len)
{
	struct cortexa_priv *priv = t->priv;
	if (len == 0)
		return;

	if ((dest & 3) || (len & 3)) {
		cortexa_slow_mem_write_bytes(t, dest, src, len);
		return;
	}

	write_gpreg(t, 0, dest);
	const uint32_t *src32 = src;

	/* Switch to fast DCC mode */
	uint32_t dbgdscr = apb_read(t, DBGDSCR);
	dbgdscr = (dbgdscr & ~DBGDSCR_EXTDCCMODE_MASK) | DBGDSCR_EXTDCCMODE_FAST;
	apb_write(t, DBGDSCR, dbgdscr);

	apb_write(t, DBGITR, 0xeca05e01); /* stc 14, cr5, [r0], #4 */

	for (; len; len -= 4)
		apb_write(t, DBGDTRRX, *src32++);

	/* Switch back to stalling DCC mode */
	dbgdscr = (dbgdscr & ~DBGDSCR_EXTDCCMODE_MASK) | DBGDSCR_EXTDCCMODE_STALL;
	apb_write(t, DBGDSCR, dbgdscr);

	if (apb_read(t, DBGDSCR) & DBGDSCR_SDABORT_L) {
		/* Memory access aborted, flag a fault */
		apb_write(t, DBGDRCR, DBGDRCR_CSE);
		priv->mmu_fault = true;
	}
}

static bool cortexa_check_error(target *t)
{
	struct cortexa_priv *priv = t->priv;
	ADIv5_AP_t *ahb = priv->ahb;
	bool err = (ahb && (adiv5_dp_error(ahb->dp)) != 0) || priv->mmu_fault;
	priv->mmu_fault = false;
	return err;
}


bool cortexa_probe(ADIv5_AP_t *apb, uint32_t debug_base)
{
	target *t;

	DEBUG("%s base=0x%08"PRIx32"\n", __func__, debug_base);

	/* Prepend to target list... */
	t = target_new(sizeof(*t));
	adiv5_ap_ref(apb);
	struct cortexa_priv *priv = calloc(1, sizeof(*priv));
	t->priv = priv;
	t->priv_free = free;
	priv->apb = apb;
	/* FIXME Find a better way to find the AHB.  This is likely to be
	 * device specific. */
	priv->ahb = adiv5_new_ap(apb->dp, 0);
	adiv5_ap_ref(priv->ahb);
	if ((priv->ahb->idr & 0xfffe00f) == 0x4770001) {
		/* This is an AHB */
		t->mem_read = cortexa_mem_read;
		t->mem_write = cortexa_mem_write;
	} else {
		/* This is not an AHB, fall back to slow APB access */
		adiv5_ap_unref(priv->ahb);
		priv->ahb = NULL;
		t->mem_read = cortexa_slow_mem_read;
		t->mem_write = cortexa_slow_mem_write;
	}

	priv->base = debug_base;
	/* Set up APB CSW, we won't touch this again */
	uint32_t csw = apb->csw | ADIV5_AP_CSW_SIZE_WORD;
	adiv5_ap_write(apb, ADIV5_AP_CSW, csw);
	uint32_t dbgdidr = apb_read(t, DBGDIDR);
	priv->hw_breakpoint_max = ((dbgdidr >> 24) & 15)+1;
	DEBUG("Target has %d breakpoints\n", priv->hw_breakpoint_max);

	t->check_error = cortexa_check_error;

	t->driver = cortexa_driver_str;

	t->attach = cortexa_attach;
	t->detach = cortexa_detach;

	t->tdesc = tdesc_cortex_a;
	t->regs_read = cortexa_regs_read;
	t->regs_write = cortexa_regs_write;

	t->reset = cortexa_reset;
	t->halt_request = cortexa_halt_request;
	t->halt_wait = cortexa_halt_wait;
	t->halt_resume = cortexa_halt_resume;
	t->regs_size = sizeof(priv->reg_cache);

	t->set_hw_bp = cortexa_set_hw_bp;
	t->clear_hw_bp = cortexa_clear_hw_bp;

	return true;
}

bool cortexa_attach(target *t)
{
	struct cortexa_priv *priv = t->priv;
	int tries;

	/* Clear any pending fault condition */
	target_check_error(t);

	/* Enable halting debug mode */
	uint32_t dbgdscr = apb_read(t, DBGDSCR);
	dbgdscr |= DBGDSCR_HDBGEN | DBGDSCR_ITREN;
	dbgdscr = (dbgdscr & ~DBGDSCR_EXTDCCMODE_MASK) | DBGDSCR_EXTDCCMODE_STALL;
	apb_write(t, DBGDSCR, dbgdscr);
	DEBUG("DBGDSCR = 0x%08x\n", dbgdscr);

	target_halt_request(t);
	tries = 10;
	while(!platform_srst_get_val() && !target_halt_wait(t) && --tries)
		platform_delay(200);
	if(!tries)
		return false;

	/* Clear any stale breakpoints */
	for(unsigned i = 0; i < priv->hw_breakpoint_max; i++) {
		apb_write(t, DBGBCR(i), 0);
		priv->hw_breakpoint[i] = 0;
	}

	platform_srst_set_val(false);

	return true;
}

void cortexa_detach(target *t)
{
	struct cortexa_priv *priv = t->priv;

	/* Clear any stale breakpoints */
	for(unsigned i = 0; i < priv->hw_breakpoint_max; i++) {
		priv->hw_breakpoint[i] = 0;
		apb_write(t, DBGBCR(i), 0);
	}

	/* Restore any clobbered registers */
	cortexa_regs_write_internal(t);
	/* Invalidate cache */
	apb_write(t, DBGITR, MCR | ICIALLU);

	uint32_t dbgdscr = apb_read(t, DBGDSCR);
	/* Disable halting debug mode */
	dbgdscr &= ~(DBGDSCR_HDBGEN | DBGDSCR_ITREN);
	apb_write(t, DBGDSCR, dbgdscr);
	/* Clear sticky error and resume */
	apb_write(t, DBGDRCR, DBGDRCR_CSE | DBGDRCR_RRQ);
}


static uint32_t read_gpreg(target *t, uint8_t regno)
{
	/* To read a register we use DBGITR to load an MCR instruction
	 * that sends the value via DCC DBGDTRTX using the CP14 interface.
	 */
	uint32_t instr = MCR | DBGDTRTXint | ((regno & 0xf) << 12);
	apb_write(t, DBGITR, instr);
	/* Return value read from DCC channel */
	return apb_read(t, DBGDTRTX);
}

static void write_gpreg(target *t, uint8_t regno, uint32_t val)
{
	/* Write value to DCC channel */
	apb_write(t, DBGDTRRX, val);
	/* Run instruction to load register */
	uint32_t instr = MRC | DBGDTRRXint | ((regno & 0xf) << 12);
	apb_write(t, DBGITR, instr);
}

static void cortexa_regs_read(target *t, void *data)
{
	struct cortexa_priv *priv = (struct cortexa_priv *)t->priv;
	memcpy(data, &priv->reg_cache, t->regs_size);
}

static void cortexa_regs_write(target *t, const void *data)
{
	struct cortexa_priv *priv = (struct cortexa_priv *)t->priv;
	memcpy(&priv->reg_cache, data, t->regs_size);
}

static void cortexa_regs_read_internal(target *t)
{
	struct cortexa_priv *priv = (struct cortexa_priv *)t->priv;
	/* Read general purpose registers */
	for (int i = 0; i < 15; i++) {
		priv->reg_cache.r[i] = read_gpreg(t, i);
	}
	/* Read PC, via r0.  MCR is UNPREDICTABLE for Rt = r15. */
	apb_write(t, DBGITR, 0xe1a0000f); /* mov r0, pc */
	priv->reg_cache.r[15] = read_gpreg(t, 0);
	/* Read CPSR */
	apb_write(t, DBGITR, 0xE10F0000); /* mrs r0, CPSR */
	priv->reg_cache.cpsr = read_gpreg(t, 0);
	/* Read FPSCR */
	apb_write(t, DBGITR, 0xeef10a10); /* vmrs r0, fpscr */
	priv->reg_cache.fpscr = read_gpreg(t, 0);
	/* Read out VFP registers */
	for (int i = 0; i < 16; i++) {
		/* Read D[i] to R0/R1 */
		apb_write(t, DBGITR, 0xEC510B10 | i); /* vmov r0, r1, d0 */
		priv->reg_cache.d[i] = ((uint64_t)read_gpreg(t, 1) << 32) | read_gpreg(t, 0);
	}
	priv->reg_cache.r[15] -= (priv->reg_cache.cpsr & CPSR_THUMB) ? 4 : 8;
}

static void cortexa_regs_write_internal(target *t)
{
	struct cortexa_priv *priv = (struct cortexa_priv *)t->priv;
	/* First write back floats */
	for (int i = 0; i < 16; i++) {
		write_gpreg(t, 1, priv->reg_cache.d[i] >> 32);
		write_gpreg(t, 0, priv->reg_cache.d[i]);
		apb_write(t, DBGITR, 0xec410b10 | i); /* vmov d[i], r0, r1 */
	}
	/* Write back FPSCR */
	write_gpreg(t, 0, priv->reg_cache.fpscr);
	apb_write(t, DBGITR, 0xeee10a10); /* vmsr fpscr, r0 */
	/* Write back the CPSR */
	write_gpreg(t, 0, priv->reg_cache.cpsr);
	apb_write(t, DBGITR, 0xe12ff000); /* msr CPSR_fsxc, r0 */
	/* Write back PC, via r0.  MRC clobbers CPSR instead */
	write_gpreg(t, 0, priv->reg_cache.r[15]);
	apb_write(t, DBGITR, 0xe1a0f000); /* mov pc, r0 */
	/* Finally the GP registers now that we're done using them */
	for (int i = 0; i < 15; i++) {
		write_gpreg(t, i, priv->reg_cache.r[i]);
	}
}

static void cortexa_reset(target *t)
{
	/* This mess is Xilinx Zynq specific
	 * See Zynq-7000 TRM, Xilinx doc UG585
	 */
#define ZYNQ_SLCR_UNLOCK       0xf8000008
#define ZYNQ_SLCR_UNLOCK_KEY   0xdf0d
#define ZYNQ_SLCR_PSS_RST_CTRL 0xf8000200
	target_mem_write32(t, ZYNQ_SLCR_UNLOCK, ZYNQ_SLCR_UNLOCK_KEY);
	target_mem_write32(t, ZYNQ_SLCR_PSS_RST_CTRL, 1);

	/* Try hard reset too */
	platform_srst_set_val(true);
	platform_srst_set_val(false);

	/* Spin until Xilinx reconnects us */
	platform_timeout timeout;
	platform_timeout_set(&timeout, 1000);
	volatile struct exception e;
	do {
		TRY_CATCH (e, EXCEPTION_ALL) {
			apb_read(t, DBGDIDR);
		}
	} while (!platform_timeout_is_expired(&timeout) && e.type == EXCEPTION_ERROR);
	if (e.type == EXCEPTION_ERROR)
		raise_exception(e.type, e.msg);

	platform_delay(100);

	cortexa_attach(t);
}

static void cortexa_halt_request(target *t)
{
	volatile struct exception e;
	TRY_CATCH (e, EXCEPTION_TIMEOUT) {
		apb_write(t, DBGDRCR, DBGDRCR_HRQ);
	}
	if (e.type) {
		gdb_out("Timeout sending interrupt, is target in WFI?\n");
	}
}

static int cortexa_halt_wait(target *t)
{
	volatile uint32_t dbgdscr = 0;
	volatile struct exception e;
	TRY_CATCH (e, EXCEPTION_ALL) {
		/* If this times out because the target is in WFI then
		 * the target is still running. */
		dbgdscr = apb_read(t, DBGDSCR);
	}
	switch (e.type) {
	case EXCEPTION_ERROR:
		/* Oh crap, there's no recovery from this... */
		target_list_free();
		morse("TARGET LOST.", 1);
		return SIGLOST;
	case EXCEPTION_TIMEOUT:
		/* Timeout isn't a problem, target could be in WFI */
		return 0;
	}

	if (!(dbgdscr & DBGDSCR_HALTED)) /* Not halted */
		return 0;

	DEBUG("%s: DBGDSCR = 0x%08x\n", __func__, dbgdscr);
	/* Reenable DBGITR */
	dbgdscr |= DBGDSCR_ITREN;
	apb_write(t, DBGDSCR, dbgdscr);

	/* Find out why we halted */
	int sig;
	switch (dbgdscr & DBGDSCR_MOE_MASK) {
	case DBGDSCR_MOE_HALT_REQ:
		sig = SIGINT;
		break;
	default:
		sig = SIGTRAP;
	}

	cortexa_regs_read_internal(t);

	return sig;
}

void cortexa_halt_resume(target *t, bool step)
{
	struct cortexa_priv *priv = t->priv;
	/* Set breakpoint comarator for single stepping if needed */
	if (step) {
		uint32_t addr = priv->reg_cache.r[15];
		uint32_t bas = bp_bas(addr, (priv->reg_cache.cpsr & CPSR_THUMB) ? 2 : 4);
		DEBUG("step 0x%08x  %x\n", addr, bas);
		/* Set match any breakpoint */
		apb_write(t, DBGBVR(0), priv->reg_cache.r[15] & ~3);
		apb_write(t, DBGBCR(0), DBGBCR_INST_MISMATCH | bas |
		                             DBGBCR_EN);
	} else {
		apb_write(t, DBGBVR(0), priv->hw_breakpoint[0] & ~3);
		apb_write(t, DBGBCR(0), priv->bpc0);
	}

	/* Write back register cache */
	cortexa_regs_write_internal(t);

	apb_write(t, DBGITR, MCR | ICIALLU); /* invalidate cache */

	/* Disable DBGITR.  Not sure why, but RRQ is ignored otherwise. */
	uint32_t dbgdscr = apb_read(t, DBGDSCR);
	if (step)
		dbgdscr |= DBGDSCR_INTDIS;
	else
		dbgdscr &= ~DBGDSCR_INTDIS;
	dbgdscr &= ~DBGDSCR_ITREN;
	apb_write(t, DBGDSCR, dbgdscr);

	do {
		apb_write(t, DBGDRCR, DBGDRCR_CSE | DBGDRCR_RRQ);
		dbgdscr = apb_read(t, DBGDSCR);
		DEBUG("%s: DBGDSCR = 0x%08x\n", __func__, dbgdscr);
	} while (!(dbgdscr & DBGDSCR_RESTARTED));
}

/* Breakpoints */
static uint32_t bp_bas(uint32_t addr, uint8_t len)
{
	if (len == 4)
		return DBGBCR_BAS_ANY;
	else if (addr & 2)
		return DBGBCR_BAS_HIGH_HW;
	else
		return DBGBCR_BAS_LOW_HW;
}

static int cortexa_set_hw_bp(target *t, uint32_t addr, uint8_t len)
{
	struct cortexa_priv *priv = t->priv;
	unsigned i;

	for(i = 0; i < priv->hw_breakpoint_max; i++)
		if((priv->hw_breakpoint[i] & 1) == 0) break;

	if(i == priv->hw_breakpoint_max) return -1;

	priv->hw_breakpoint[i] = addr | 1;

	apb_write(t, DBGBVR(i), addr & ~3);
	uint32_t bpc =  bp_bas(addr, len) | DBGBCR_EN;
	apb_write(t, DBGBCR(i), bpc);
	if (i == 0)
		priv->bpc0 = bpc;

	return 0;
}

static int cortexa_clear_hw_bp(target *t, uint32_t addr, uint8_t len)
{
	struct cortexa_priv *priv = t->priv;
	unsigned i;

	(void)len;

	for (i = 0; i < priv->hw_breakpoint_max; i++)
		if ((priv->hw_breakpoint[i] & ~1) == addr)
			break;
	if (i == priv->hw_breakpoint_max)
		return -1;

	priv->hw_breakpoint[i] = 0;

	apb_write(t, DBGBCR(i), 0);
	if (i == 0)
		priv->bpc0 = 0;

	return 0;
}
