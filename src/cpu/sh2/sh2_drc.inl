// SH2 recompiler support, included from sh2.cpp.
//
// Runs PicoDrive's SH2 DRC (src/cpu/sh2/drc) on the state of the FBNeo SH2
// core. The DRC has its own context (sh2s[0]); registers are copied between
// it and FBNeo's SH2 struct at Sh2Run() boundaries, so savestates, the
// debugger and the drivers keep seeing the usual SH2 struct. Everything that
// can happen in the middle of a run (memory handlers, irq changes, timers,
// Sh2StopRun/Sh2BurnUntilInt) is forwarded to the DRC context from here.
//
// Only one SH2 is supported, which is all CPS3 needs.

#include <stdarg.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#elif defined(VITA)
#include <psp2/kernel/sysmem.h>
#include "drc/drc/cmn.h"
// RetroArch's bootstrap reserves an RWX block of this size, see getVMBlock()
extern "C" int _newlib_vm_size_user;
int _newlib_vm_size_user = DRC_TCACHE_SIZE;
extern "C" int getVMBlock();
#elif !defined(_WIN32)
#include <sys/mman.h>
#endif

SH2_DRC sh2s[2];
struct PicoInterface PicoIn;
struct Pico32x Pico32x;
static struct Pico32xMem drc_mem;
struct Pico32xMem *Pico32xMem = &drc_mem;

typedef struct { uintptr_t addr; uintptr_t mask; } sh2_drc_memmap;
#define DRC_MAP_MEMORY(m)  ((uintptr_t)(m) >> 1)
#define DRC_MAP_HANDLER(h) (((uintptr_t)(h) >> 1) | ((uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1)))

static sh2_drc_memmap drc_read8_map[SH2_PAGE_COUNT];
static sh2_drc_memmap drc_read16_map[SH2_PAGE_COUNT];
static sh2_drc_memmap drc_read32_map[SH2_PAGE_COUNT];
static const void *drc_write8_tab[SH2_PAGE_COUNT];
static const void *drc_write16_tab[SH2_PAGE_COUNT];
static const void *drc_write32_tab[SH2_PAGE_COUNT];
static UINT8 drc_page_is_rom[SH2_PAGE_COUNT];	// see p32x_sh2_mem_is_rom()

static INT32 drc_run_icount;	// sh2->sh2_icount when the current timeslice started
static INT32 drc_poll_count;

#define DRC_POLL_LIMIT	8		// polls of the same address before giving up the timeslice

static SH2_DRC *drc = &sh2s[0];

extern "C" void sh2_drc_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	bprintf(PRINT_NORMAL, _T("SH2 DRC: %s"), buf);
}

extern "C" void memset32(void *dest, int c, int count)
{
	UINT32 *d = (UINT32 *)dest;
	while (count--)
		*d++ = c;
}

// ---------------------------------------------------------------------------
// translation cache memory

#if defined(__APPLE__) && defined(__aarch64__)
static INT32 jit_write_depth;

extern "C" void drc_jit_write_begin(void)
{
	if (jit_write_depth++ == 0)
		pthread_jit_write_protect_np(0);
}

extern "C" void drc_jit_write_end(void)
{
	if (--jit_write_depth == 0)
		pthread_jit_write_protect_np(1);
}
#else
extern "C" void drc_jit_write_begin(void) {}
extern "C" void drc_jit_write_end(void) {}
#endif

#if defined(VITA)
static INT32 vita_block;
#endif

extern "C" void cache_flush_d_inval_i(void *start, void *end)
{
#if defined(VITA)
	sceKernelSyncVMDomain(vita_block, start, (char *)end - (char *)start);
#elif defined(__APPLE__)
	sys_icache_invalidate(start, (char *)end - (char *)start);
#else
	__builtin___clear_cache((char *)start, (char *)end);
#endif
}

extern "C" void *plat_mem_get_for_drc(size_t size)
{
	void *mem = NULL;
#if defined(VITA)
	vita_block = getVMBlock();
	if (vita_block <= 0 || sceKernelGetMemBlockBase(vita_block, &mem) < 0)
		mem = NULL;
#elif defined(__APPLE__) && defined(__aarch64__)
	mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	if (mem == MAP_FAILED)
		mem = NULL;
#elif !defined(_WIN32)
	mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mem == MAP_FAILED)
		mem = NULL;
#endif
	if (mem == NULL)
		bprintf(PRINT_ERROR, _T("SH2 DRC: can't allocate translation cache\n"));
	return mem;
}

extern "C" int plat_mem_set_exec(void *ptr, size_t size)
{
#if defined(VITA)
	return sceKernelOpenVMDomain();
#else
	return 0;
#endif
}

// ---------------------------------------------------------------------------
// cycles and timeslices

// SR bits 12-31 hold the cycles left in the timeslice, minus one
static inline INT32 drc_cycles_left()
{
	return ((INT32)drc->sr >> 12) + 1;
}

// make sh2->sh2_icount (and thus sh2_GetTotalCycles()) valid inside a timeslice
static inline void drc_sync_icount()
{
	sh2->sh2_icount = drc_run_icount - ((INT32)drc->cycles_timeslice - drc_cycles_left());
}

// stop at the next block boundary. With burn != 0 the cycles left are consumed,
// else they are handed back to Sh2Run(). SR must have been saved by the caller.
static void drc_end_timeslice(INT32 burn)
{
	INT32 left = (INT32)drc->sr >> 12;
	if (left > 0) {
		if (!burn)
			drc->cycles_timeslice -= left;
		drc->sr -= left << 12;
	}
}

// called from sh2.cpp when something outside of the translated code needs it to stop
static void Sh2DrcEndRun()
{
	if (!drc_in_run)
		return;

	DRC_SAVE_SR(drc);
	drc_end_timeslice(0);
	DRC_RESTORE_SR(drc);
}

// ---------------------------------------------------------------------------
// interrupts

static INT32 drc_pending_level()
{
	INT32 level = -1;
	for (INT32 i = 15; i >= 0; i--) {
		if (sh2->pending_irq & (1 << i)) {
			level = i;
			break;
		}
	}
	if (sh2->internal_irq_level != -1 && sh2->internal_irq_level > level)
		level = sh2->internal_irq_level;
	return level;
}

// irq state changed (sh2->pending_irq or internal irq). The DRC tests for irqs
// on entry and after SR changes, so leave the translated code if one is due.
static void Sh2DrcIrqChanged()
{
	drc->pending_level = drc_pending_level();

	if (drc_in_run) {
		DRC_SAVE_SR(drc);
		if (drc->pending_level > (INT32)((drc->sr >> 4) & 0x0f))
			drc_end_timeslice(0);
		DRC_RESTORE_SR(drc);
	}
}

static int REGPARM(2) drc_irq_callback(SH2_DRC *, int level)
{
	int vector;

	if (sh2->internal_irq_level == level)
		vector = sh2->internal_irq_vector;
	else if (level == 16)
		vector = 11;
	else
		vector = 64 + level / 2;

	if (level < 16 && sh2->irq_line_hold[level]) {
		sh2->irq_line_hold[level] = 0;
		Sh2SetIRQLine_Internal(level, CPU_IRQSTATUS_NONE);
	}

	return vector;
}

// ---------------------------------------------------------------------------
// memory interface used by the translated code

static inline unsigned char *drc_page(UINT32 a, INT32 map)
{
	return pSh2Ext->MemMap[(a >> SH2_SHIFT) + map * SH2_WADD];
}

#define DRC_HANDLER_CALL(call)			\
	DRC_SAVE_SR(drc);					\
	drc_sync_icount();					\
	call;								\
	DRC_RESTORE_SR(drc);

extern "C" u32 REGPARM(2) p32x_sh2_read8(u32 a, SH2_DRC *)
{
	unsigned char *pr = drc_page(a, 0);
	if ((uintptr_t)pr >= SH2_MAXHANDLER)
		return (s8)pr[(a ^ 3) & SH2_PAGEM];

	UINT8 d;
	DRC_HANDLER_CALL(d = pSh2Ext->ReadByte[(uintptr_t)pr](a));
	return (s8)d;
}

extern "C" u32 REGPARM(2) p32x_sh2_read16(u32 a, SH2_DRC *)
{
	unsigned char *pr = drc_page(a, 0);
	if ((uintptr_t)pr >= SH2_MAXHANDLER)
		return (s16)*(UINT16 *)(pr + ((a ^ 2) & SH2_PAGEM));

	UINT16 d;
	DRC_HANDLER_CALL(d = pSh2Ext->ReadWord[(uintptr_t)pr](a));
	return (s16)d;
}

extern "C" u32 REGPARM(2) p32x_sh2_read32(u32 a, SH2_DRC *)
{
	unsigned char *pr = drc_page(a, 0);
	if ((uintptr_t)pr >= SH2_MAXHANDLER)
		return *(UINT32 *)(pr + (a & SH2_PAGEM));

	UINT32 d;
	DRC_HANDLER_CALL(d = pSh2Ext->ReadLong[(uintptr_t)pr](a));
	return d;
}

// invalidate translated code overwritten by a store to RAM or the data array
static inline void drc_write_check(u32 a, unsigned len)
{
	if (!drc_mem.marked_blocks) // usually no code runs from RAM at all
		return;

	if ((a & DRC_SDRAM_AMASK) == DRC_SDRAM_BASE) {
		if (drc_mem.drcblk_ram[(a & (DRC_SDRAM_SIZE - 1)) >> SH2_DRCBLK_RAM_SHIFT])
			sh2_drc_wcheck_ram(a, len, drc);
	} else if ((a & 0xfffff000) == 0xc0000000) {
		if (drc_mem.drcblk_da[0][(a & 0xfff) >> SH2_DRCBLK_DA_SHIFT])
			sh2_drc_wcheck_da(a, len, drc);
	}
}

// stores done outside of the translated code (DMA)
static void Sh2DrcWriteCheck(UINT32 a, UINT32 len)
{
	for (UINT32 i = 0; i < len; i += 2)
		drc_write_check(a + i, 2);
}

extern "C" void REGPARM(3) p32x_sh2_write8(u32 a, u32 d, SH2_DRC *)
{
	unsigned char *pr = drc_page(a, 1);
	if ((uintptr_t)pr >= SH2_MAXHANDLER)
		pr[(a ^ 3) & SH2_PAGEM] = d;
	else {
		DRC_HANDLER_CALL(pSh2Ext->WriteByte[(uintptr_t)pr](a, d));
	}
	drc_write_check(a, 1);
}

extern "C" void REGPARM(3) p32x_sh2_write16(u32 a, u32 d, SH2_DRC *)
{
	unsigned char *pr = drc_page(a, 1);
	if ((uintptr_t)pr >= SH2_MAXHANDLER)
		*(UINT16 *)(pr + ((a ^ 2) & SH2_PAGEM)) = d;
	else {
		DRC_HANDLER_CALL(pSh2Ext->WriteWord[(uintptr_t)pr](a, d));
	}
	drc_write_check(a, 2);
}

extern "C" void REGPARM(3) p32x_sh2_write32(u32 a, u32 d, SH2_DRC *)
{
	unsigned char *pr = drc_page(a, 1);
	if ((uintptr_t)pr >= SH2_MAXHANDLER)
		*(UINT32 *)(pr + (a & SH2_PAGEM)) = d;
	else {
		DRC_HANDLER_CALL(pSh2Ext->WriteLong[(uintptr_t)pr](a, d));
	}
	drc_write_check(a, 4);
}

// The DRC routes reads in loops it identified as polling through these.
// Whatever is polled can only change at a timeslice boundary (irqs, DMA and
// timers are all handled there), so after a few polls give the rest away.
static inline u32 drc_poll(u32 a, u32 d)
{
	if (a == drc->poll_addr) {
		if (++drc_poll_count >= DRC_POLL_LIMIT) {
			DRC_SAVE_SR(drc);
			drc_end_timeslice(1);
			DRC_RESTORE_SR(drc);
			drc_poll_count = 0;
		}
	} else {
		drc->poll_addr = a;
		drc_poll_count = 0;
	}
	return d;
}

extern "C" u32 REGPARM(3) p32x_sh2_poll_memory8(u32 a, u32 d, SH2_DRC *)  { return drc_poll(a, d); }
extern "C" u32 REGPARM(3) p32x_sh2_poll_memory16(u32 a, u32 d, SH2_DRC *) { return drc_poll(a, d); }
extern "C" u32 REGPARM(3) p32x_sh2_poll_memory32(u32 a, u32 d, SH2_DRC *) { return drc_poll(a, d); }

// Find the largest naturally aligned region around a whose pages are mapped
// contiguously in the given map. Returns its host address (for a & ~mask).
static void *drc_mem_region(u32 a, u32 *mask, INT32 map)
{
	unsigned char **mm = pSh2Ext->MemMap + map * SH2_WADD;
	UINT32 page = a >> SH2_SHIFT;

	if ((uintptr_t)mm[page] < SH2_MAXHANDLER)
		return (void *)-1;

	for (UINT32 pages = 0x100; pages > 1; pages >>= 1) { // 16MB down to 128KB
		UINT32 first = page & ~(pages - 1);
		UINT32 i;
		for (i = 0; i < pages; i++)
			if (mm[first + i] != mm[first] + (i << SH2_SHIFT))
				break;
		if (i == pages) {
			*mask = (pages << SH2_SHIFT) - 1;
			return mm[first];
		}
	}

	*mask = SH2_PAGEM;
	return mm[page];
}

extern "C" void *p32x_sh2_get_mem_ptr(u32 a, u32 *mask, SH2_DRC *)
{
	return drc_mem_region(a, mask, 0);
}

extern "C" void *p32x_sh2_get_code_ptr(u32 a, u32 *mask, SH2_DRC *)
{
	return drc_mem_region(a, mask, 2);
}

// Data may be folded into the translated code at compile time if it can't
// change: directly mapped for reads and fetches from the same memory, and
// not directly writable.
extern "C" int p32x_sh2_mem_is_rom(u32 a, SH2_DRC *)
{
	return drc_page_is_rom[a >> SH2_SHIFT];
}

// ---------------------------------------------------------------------------
// memory map

// returns non-zero if the page was treated as ROM before
static INT32 drc_map_update_page(UINT32 page)
{
	unsigned char *pr = pSh2Ext->MemMap[page];
	INT32 was_rom = drc_page_is_rom[page];

	drc_page_is_rom[page] = (uintptr_t)pr >= SH2_MAXHANDLER && pr == pSh2Ext->MemMap[page + SH2_WADD * 2]
		&& (uintptr_t)pSh2Ext->MemMap[page + SH2_WADD] < SH2_MAXHANDLER;

	if ((uintptr_t)pr >= SH2_MAXHANDLER) {
		drc_read8_map[page].addr  = drc_read16_map[page].addr  = drc_read32_map[page].addr  = DRC_MAP_MEMORY(pr);
		drc_read8_map[page].mask  = drc_read16_map[page].mask  = drc_read32_map[page].mask  = SH2_PAGEM;
	} else {
		drc_read8_map[page].addr  = DRC_MAP_HANDLER(p32x_sh2_read8);
		drc_read16_map[page].addr = DRC_MAP_HANDLER(p32x_sh2_read16);
		drc_read32_map[page].addr = DRC_MAP_HANDLER(p32x_sh2_read32);
	}
	drc_write8_tab[page]  = (const void *)p32x_sh2_write8;
	drc_write16_tab[page] = (const void *)p32x_sh2_write16;
	drc_write32_tab[page] = (const void *)p32x_sh2_write32;

	return was_rom;
}

static void drc_map_update_pointers()
{
	u32 mask;
	void *p;

	// regions the DRC may access directly when the address is a constant
	p = drc_mem_region(0x00000000, &mask, 0);
	drc->p_bios = (p == (void *)-1) ? NULL : p;
	p = drc_mem_region(DRC_SDRAM_BASE, &mask, 0);
	drc->p_sdram = (p == (void *)-1) ? NULL : p;
	p = drc_mem_region(0x06000000, &mask, 0);
	drc->p_rom = (p == (void *)-1) ? NULL : p;
	drc->p_da = NULL; // CPS3 has no data array in the context
}

// pSh2Ext->MemMap changed for [nStart, nEnd] (and its user space mirrors)
static void Sh2DrcMapChanged(UINT32 nStart, UINT32 nEnd, INT32 nType)
{
	if (!drc_initted)
		return;

	// translated code may have constants folded from pages treated as ROM
	INT32 flush = nType & 0x04 /*SM_FETCH*/;

	for (UINT64 a = nStart & ~SH2_PAGEM; a <= nEnd; a += SH2_PAGE_SIZE) {
		flush |= drc_map_update_page(a >> SH2_SHIFT);
		if (nStart < 0x08000000)
			for (UINT32 m = 1; m < 8; m++)
				flush |= drc_map_update_page((a >> SH2_SHIFT) + m * (0x08000000 >> SH2_SHIFT));
	}
	drc_map_update_pointers();

	if (flush)
		sh2_drc_flush_all();
}

// ---------------------------------------------------------------------------

static void Sh2DrcInit()
{
	memset(drc, 0, sizeof(*drc));
	memset(&drc_mem, 0, sizeof(drc_mem));
	PicoIn.opt = POPT_EN_DRC;

	for (UINT32 page = 0; page < SH2_PAGE_COUNT; page++)
		drc_map_update_page(page);
	drc_map_update_pointers();

	drc->read8_map = drc_read8_map;
	drc->read16_map = drc_read16_map;
	drc->read32_map = drc_read32_map;
	drc->write8_tab = drc_write8_tab;
	drc->write16_tab = drc_write16_tab;
	drc->write32_tab = drc_write32_tab;
	drc->irq_callback = drc_irq_callback;
	drc->pending_level = -1;

	if (sh2_drc_init(drc)) {
		bprintf(PRINT_ERROR, _T("SH2 DRC: init failed, using the interpreter\n"));
		return;
	}
	sh2_drc_mem_setup(drc);
	drc_initted = 1;
}

static void Sh2DrcExit()
{
	if (drc_initted)
		sh2_drc_finish(drc);
	drc_initted = 0;
}

static void Sh2DrcFlush()
{
	if (drc_initted)
		sh2_drc_flush_all();
}

static INT32 Sh2DrcRun(INT32 cycles)
{
	sh2->sh2_icount = cycles;
	sh2->sh2_cycles_to_run = cycles;
	sh2->end_run = 0;

	memcpy(drc->r, sh2->r, sizeof(drc->r));
	drc->pc = sh2->pc;
	drc->pr = sh2->pr;
	drc->sr = sh2->sr;
	drc->gbr = sh2->gbr;
	drc->vbr = sh2->vbr;
	drc->mach = sh2->mach;
	drc->macl = sh2->macl;

	while (sh2->sh2_icount > 0 && !sh2->end_run) {
		if (pSh2Ext->suspend && cps3speedhack) {
			sh2->sh2_total_cycles += sh2->sh2_icount;
			sh2->sh2_icount = 0;
			break;
		}

		// run up to the next timer/dma event
		INT32 slice = sh2->sh2_icount;
		UINT32 now = sh2_GetTotalCycles();
		if (sh2->timer_active) {
			INT32 delta = (INT32)(sh2->timer_base + sh2->timer_cycles - now);
			if (delta < slice) slice = delta;
		}
		for (INT32 i = 0; i < 2; i++) {
			if (sh2->dma_timer_active[i]) {
				INT32 delta = (INT32)(sh2->dma_timer_base[i] + sh2->dma_timer_cycles[i] - now);
				if (delta < slice) slice = delta;
			}
		}
		if (slice > 0x40000) slice = 0x40000; // SR holds 20 bits of cycles
		if (slice < 1) slice = 1;

		drc->pending_level = drc_pending_level();
		drc_run_icount = sh2->sh2_icount;
		drc->cycles_timeslice = slice;

		drc_in_run = 1;
		INT32 left = sh2_execute_drc(drc, slice);
		drc_in_run = 0;

		INT32 done = (INT32)drc->cycles_timeslice - left;
		sh2->sh2_icount = drc_run_icount - done;
		sh2->sh2_total_cycles += done;

		UINT32 cy = sh2_GetTotalCycles();
		if (sh2->dma_timer_active[0] && (cy - sh2->dma_timer_base[0]) >= sh2->dma_timer_cycles[0])
			sh2_dmac_callback(0);
		if (sh2->dma_timer_active[1] && (cy - sh2->dma_timer_base[1]) >= sh2->dma_timer_cycles[1])
			sh2_dmac_callback(1);
		if (sh2->timer_active && (cy - sh2->timer_base) >= sh2->timer_cycles)
			sh2_timer_callback();
	}

	memcpy(sh2->r, drc->r, sizeof(sh2->r));
	sh2->pc = drc->pc;
	sh2->ppc = drc->pc;
	sh2->pr = drc->pr;
	sh2->sr = drc->sr & FLAGS;
	sh2->gbr = drc->gbr;
	sh2->vbr = drc->vbr;
	sh2->mach = drc->mach;
	sh2->macl = drc->macl;
	change_pc(sh2->pc & AM);

	cycles = cycles - sh2->sh2_icount;
	sh2->cycle_counts += cycles;
	sh2->sh2_cycles_to_run = sh2->sh2_icount = 0;

	return cycles;
}
