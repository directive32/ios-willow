/*
 * Milestone 3: EL1 stage-1 MMU with 4K granule for Willow (Cortex-A73/A53).
 */
#include <stdint.h>
#include "mmu.h"
#include "console.h"

#define PG_SHIFT	12
#define PG_SIZE		(1UL << PG_SHIFT)		/* 4K */
#define BLK_SHIFT	21
#define BLK_SIZE	(1UL << BLK_SHIFT)		/* 2MB level-2 block */
#define L1_SHIFT	30
#define L1_SIZE		(1UL << L1_SHIFT)		/* 1GB level-1 block */
#define NENT		512

#define ATTR_DEV	0	/* MAIR attr0 = 0x00 Device-nGnRnE */
#define ATTR_WB		1	/* MAIR attr1 = 0xff Normal WB RW-alloc */
#define ATTR_NC		2	/* MAIR attr2 = 0x44 Normal non-cacheable */
#define MAIR_VAL	0x44ff00UL

#define D_BLOCK		1UL
#define D_TABLE		3UL
#define D_PAGE		3UL
#define D_ATTR(i)	((uint64_t)(i) << 2)
#define D_SH_OUTER	(2UL << 8)
#define D_SH_INNER	(3UL << 8)
#define D_AF		(1UL << 10)
#define D_PXN		(1UL << 53)
#define D_UXN		(1UL << 54)

#define DEV_FLAGS	(D_ATTR(ATTR_DEV) | D_AF | D_PXN | D_UXN)
#define WB_FLAGS	(D_ATTR(ATTR_WB) | D_SH_INNER | D_AF | D_UXN)
#define NC_FLAGS	(D_ATTR(ATTR_NC) | D_SH_OUTER | D_AF | D_PXN | D_UXN)

/*
 * TCR: T0SZ=25 (39-bit VA, start at L1), TG0=4K (0), IRGN0/ORGN0=WBWA, SH0=inner,
 * EPD1=1 (disable TTBR1), TG1=4K (2), IPS=36-bit (2).
 */
#define TCR_VAL		(25UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | (0UL << 14) | \
			 (25UL << 16) | (1UL << 23) | (2UL << 30) | (2UL << 32))

#define SCTLR_M		(1UL << 0)
#define SCTLR_A		(1UL << 1)
#define SCTLR_C		(1UL << 2)
#define SCTLR_I		(1UL << 12)
#define SCTLR_WXN	(1UL << 19)

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

extern char _head[], _end[];

/* Tables: L1 + up to 4 L2 + 2 L3 tables, 4096-aligned */
static uint64_t pt_pool[8 * NENT] __attribute__((aligned(4096)));

static void dcache_inval_range(uintptr_t start, uintptr_t end)
{
	uint64_t line = 4UL << ((rsr("ctr_el0") >> 16) & 0xf);

	for (uintptr_t a = start & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void zero_table(uint64_t *t)
{
	for (int i = 0; i < NENT; i++)
		t[i] = 0;
}

int mmu_enable(struct mmu_info *info)
{
	uintptr_t img_lo = (uintptr_t)_head & ~(PG_SIZE - 1);
	uintptr_t img_hi = ((uintptr_t)_end + PG_SIZE - 1) & ~(PG_SIZE - 1);
	uintptr_t pool = ((uintptr_t)pt_pool + PG_SIZE - 1) & ~(PG_SIZE - 1);

	uint64_t *l1 = (uint64_t *)pool;
	uint64_t *l2_fb = (uint64_t *)(pool + 1 * PG_SIZE);
	uint64_t *l2_img = (uint64_t *)(pool + 2 * PG_SIZE);
	uint64_t *l3_img = (uint64_t *)(pool + 3 * PG_SIZE);

	info->img_lo = img_lo;
	info->img_hi = img_hi;
	info->ttbr = pool;
	info->n_l3 = 1;

	/* 1. L1: 8 entries covering first 8GB as Device by default */
	zero_table(l1);
	for (uint64_t i = 0; i < 8; i++)
		l1[i] = (i << L1_SHIFT) | DEV_FLAGS | D_BLOCK;

	/* 2. Framebuffer is in GB 1 (0x40000000 - 0x80000000). Point l1[1] to l2_fb */
	zero_table(l2_fb);
	for (uint64_t i = 0; i < NENT; i++)
		l2_fb[i] = (1UL << L1_SHIFT) + (i << BLK_SHIFT) | DEV_FLAGS | D_BLOCK;

	/* Mark FB blocks as Normal Non-Cacheable */
	uint64_t fb_start_blk = (FB_BASE - (1UL << L1_SHIFT)) >> BLK_SHIFT;
	uint64_t fb_end_blk = (FB_BASE + 0x1000000UL - (1UL << L1_SHIFT) + BLK_SIZE - 1) >> BLK_SHIFT;
	for (uint64_t b = fb_start_blk; b < fb_end_blk && b < NENT; b++)
		l2_fb[b] = ((1UL << L1_SHIFT) + (b << BLK_SHIFT)) | NC_FLAGS | D_BLOCK;

	l1[1] = (uintptr_t)l2_fb | D_TABLE;

	/* 3. Image mapping */
	uint64_t img_gb = img_lo >> L1_SHIFT;
	if (img_gb == 1) {
		/* Image is also in GB 1 */
		uint64_t img_blk = (img_lo - (1UL << L1_SHIFT)) >> BLK_SHIFT;
		zero_table(l3_img);
		uint64_t blk_base = (1UL << L1_SHIFT) + (img_blk << BLK_SHIFT);
		for (int i = 0; i < NENT; i++) {
			uint64_t pa = blk_base + ((uint64_t)i << PG_SHIFT);
			if (pa >= img_lo && pa < img_hi)
				l3_img[i] = pa | WB_FLAGS | D_PAGE;
			else
				l3_img[i] = pa | DEV_FLAGS | D_PAGE;
		}
		l2_fb[img_blk] = (uintptr_t)l3_img | D_TABLE;
	} else {
		/* Image in GB img_gb (e.g. GB 2: 0x80000000) */
		zero_table(l2_img);
		for (uint64_t i = 0; i < NENT; i++)
			l2_img[i] = (img_gb << L1_SHIFT) + (i << BLK_SHIFT) | DEV_FLAGS | D_BLOCK;

		uint64_t img_blk = (img_lo - (img_gb << L1_SHIFT)) >> BLK_SHIFT;
		zero_table(l3_img);
		uint64_t blk_base = (img_gb << L1_SHIFT) + (img_blk << BLK_SHIFT);
		for (int i = 0; i < NENT; i++) {
			uint64_t pa = blk_base + ((uint64_t)i << PG_SHIFT);
			if (pa >= img_lo && pa < img_hi)
				l3_img[i] = pa | WB_FLAGS | D_PAGE;
			else
				l3_img[i] = pa | DEV_FLAGS | D_PAGE;
		}
		l2_img[img_blk] = (uintptr_t)l3_img | D_TABLE;
		l1[img_gb] = (uintptr_t)l2_img | D_TABLE;
	}

	info->sctlr_before = rsr("sctlr_el1");

	/* Clean page tables to PoC so MMU sees them */
	dcache_inval_range(pool, pool + 4 * PG_SIZE);

	wsr("mair_el1", MAIR_VAL);
	wsr("tcr_el1", TCR_VAL);
	wsr("ttbr0_el1", (uint64_t)pool);
	__asm__ volatile("isb; tlbi vmalle1; dsb sy; isb" ::: "memory");

	uint64_t sctlr = info->sctlr_before | SCTLR_M | SCTLR_C | SCTLR_I;
	sctlr &= ~SCTLR_A;
	sctlr &= ~SCTLR_WXN;
	wsr("sctlr_el1", sctlr);
	__asm__ volatile("isb" ::: "memory");

	info->sctlr_after = rsr("sctlr_el1");
	return (info->sctlr_after & SCTLR_M) ? 0 : -2;
}

uint64_t mmu_probe(uintptr_t va)
{
	__asm__ volatile("at s1e1r, %0" : : "r"(va));
	__asm__ volatile("isb" ::: "memory");
	return rsr("par_el1");
}
