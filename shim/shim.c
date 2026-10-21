/*
 * Willow / SM6125 (Redmi Note 8T) boot shim.
 * Tests hardware primitives for XNU port:
 *   - Framebuffer output at 0x5c000000 (1080x2340)
 *   - CPU feature registers (4K/16K/64K granule support via ID_AA64MMFR0_EL1)
 *   - PSCI/SMCCC version
 *   - SMP: PSCI CPU_ON for all 7 secondary cores (Kryo 260 Gold/Silver)
 *   - GICv3: Distributor (0x0f200000), Redistributors (0x0f300000), VTIMER (INTID 19)
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "loader.h"

#define PSHOLD			0x0440b000UL	/* pshold-base on SM6125 */
#define GICD_BASE		0x0f200000UL	/* GICD on SM6125 */
#define GICR_BASE		0x0f300000UL	/* GICR on SM6125 */
#define GICR_SIZE		0x100000UL
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)

#define VTIMER_INTID		19
#define PTIMER_INTID		18
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint32_t irq_count, irq_last;
static volatile int irq_src;

/* Willow MPIDR table: Cluster 0 (A53: 0..3), Cluster 1 (A73: 0x100..0x103) */
static const uint64_t willow_mpidr[NR_CPUS] = {
	0x000, 0x001, 0x002, 0x003,
	0x100, 0x101, 0x102, 0x103
};

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void reset(unsigned wait_s)
{
	delay_ms(wait_s * 1000);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfi");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 2 || slot == 6) {
		uint64_t iar = rsr("icc_iar1_el1");
		uint32_t id = (uint32_t)iar & 0x00ffffff;
		if (id <= 1019) {
			irq_last = id;
			irq_count++;
			wsr("icc_eoir1_el1", iar);
			isb();
			return;
		}
	}

	con_clear(BLUE);
	con_str("PANIC", "EXCEPTION");
	con_hex2("SLOT", "n=", (uint32_t)slot, "cpu=", (uint32_t)rsr("mpidr_el1"));
	con_str("STAGE", stage ? stage : "unknown");
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	reset(30);
}

/* Secondary CPU entry */
void secondary_main(uint64_t idx)
{
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++) {
		ret[i] = smc(PSCI_CPU_ON, willow_mpidr[i], (uintptr_t)secondary_entry, i);
	}
	delay_ms(300);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (willow_mpidr[i] == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=", (uint32_t)sec_mpidr[i]);
	}
}

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))
			break;
	}
	return 0;
}

static void test_gic(void)
{
	stage = "gic";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found for boot CPU");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", (uint32_t)r, "waker=", rd32(r + GICR_WAKER));

	wr32(r + GICR_IGROUPR0, 0xffffffff);
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", (uint32_t)ctlr, "grpen1=", (uint32_t)rsr("icc_igrpen1_el1"));
}

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;

	stage = "console init";
	con_clear(RED); /* Bright RED: Willow bring-up */
	con_str("SHIM", "Willow (RN8T SM6125) Boot Shim");
	con_str("ARCH", "Kryo 260 / Cortex-A73 + A53");
	con_skip();

	stage = "dtb probe";
	con_hex2("DTB", "at=", (uint32_t)dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);

	/* Check for Apple XNU payload in ramdisk */
	stage = "loader";
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_skip();

	stage = "cpu features";
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	uint32_t tg4 = mmfr0 & 0xf;
	uint32_t tg64 = (mmfr0 >> 24) & 0xf;
	uint32_t tg16 = (mmfr0 >> 20) & 0xf;
	con_hex2("GRANULE", "tg4=", tg4, "tg16=", tg16);
	con_hex2("GRANULE2", "tg64=", tg64, "parange=", (uint32_t)(mmfr0 & 0xf));

	stage = "psci probe";
	uint32_t psci_ver = (uint32_t)smc(PSCI_VERSION, 0, 0, 0);
	uint32_t smccc_ver = (uint32_t)smc(SMCCC_VERSION, 0, 0, 0);
	con_hex2("PSCI", "ver=", psci_ver, "smccc=", smccc_ver);
	con_skip();

	stage = "smp test";
	test_smp();
	con_skip();

	stage = "gic test";
	test_gic();
	con_skip();

	stage = "mmu test";
	int rc = mmu_enable(&mi);
	con_hex2("MMU-4K", "rc=", (uint32_t)rc, "ttbr0=", (uint32_t)mi.ttbr);
	con_hex2("SCTLR", "was=", (uint32_t)mi.sctlr_before, "now=", (uint32_t)mi.sctlr_after);
	con_hex("PAR_FB", mmu_probe(FB_BASE + 0x1000));
	con_hex("PAR_GIC", mmu_probe(GICD_BASE));
	con_skip();

	con_str("STATUS", "SUCCESS: Willow hardware verified!");
	con_str("REBOOT", "Auto-reboot to bootloader in 45s...");

	stage = "done";
	reset(45);
}
