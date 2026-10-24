// FBNeo replacement for PicoDrive's cpu/sh2/sh2.h.
//
// Same context layout as upstream (the generated code addresses it by offset),
// but named SH2_DRC so it can coexist with FBNeo's own SH2 struct in sh2.cpp.
// Only the recompiler is used, PicoDrive's interpreter is not built.

#ifndef __SH2_H__
#define __SH2_H__

#include <pico/pico_types.h>
#include <pico/pico_port.h>

#ifdef __cplusplus
extern "C" {
#endif

// registers - matches structure order
typedef enum {
  SHR_R0 = 0, SHR_SP = 15,
  SHR_PC,  SHR_PPC, SHR_PR,   SHR_SR,
  SHR_GBR, SHR_VBR, SHR_MACH, SHR_MACL,
  SH2_REGS, // register set size
  SHR_T = 29, SHR_MEM = 30, SHR_TMP = 31, // drc specific pseudo regs
} sh2_reg_e;
#define	SHR_R(n)	(SHR_R0+(n))

typedef struct SH2_DRC_
{
	// registers. this MUST correlate with enum sh2_reg_e.
	uint32_t	r[16] ALIGNED(32);
	uint32_t	pc;		// 40
	uint32_t	ppc;
	uint32_t	pr;
	uint32_t	sr;
	uint32_t	gbr, vbr;	// 50
	uint32_t	mach, macl;	// 58

	// common
	const void	*read8_map;
	const void	*read16_map;
	const void	*read32_map;
	const void	**write8_tab;
	const void	**write16_tab;
	const void	**write32_tab;

	// drc stuff
	int		drc_tmp;
	int		irq_cycles;
	void		*p_bios;	// convenience pointers
	void		*p_da;
	void		*p_sdram;
	void		*p_rom;
	void		*p_dram;
	void		*p_drcblk_da;
	void		*p_drcblk_ram;
	unsigned int	pdb_io_csum[2];

#define SH2_STATE_RUN   (1 << 0)	// to prevent recursion
#define SH2_STATE_SLEEP (1 << 1)	// temporarily stopped (DMA, IO, ...)
#define SH2_STATE_CPOLL (1 << 2)	// polling comm regs
#define SH2_STATE_VPOLL (1 << 3)	// polling VDP
#define SH2_STATE_RPOLL (1 << 4)	// polling address in SDRAM
#define SH2_TIMER_RUN   (1 << 6)	// SOC WDT timer is running
#define SH2_IN_DRC      (1 << 7)	// DRC in use
	unsigned int	state;
	uint32_t	poll_addr;
	unsigned int	poll_cycles;
	int		poll_cnt;
// NB MUST be a bit unused in SH2 SR, see also cpu/sh2/compiler.c!
#define SH2_NO_POLLING	(1 << 10)	// poll detection control
	int		no_polling;

	// DRC branch cache. size must be 2^n and <=128
	int rts_cache_idx;
	struct { uint32_t pc; void *code; } rts_cache[16];
	struct { uint32_t pc; void *code; } branch_cache[128];

	// interpreter stuff
	int		icount;		// cycles left in current timeslice
	unsigned int	ea;
	unsigned int	delay;
	unsigned int	test_irq;

	int	pending_level;		// MAX(pending_irl, pending_int_irq)
	int	pending_irl;
	int	pending_int_irq;	// internal irq
	int	pending_int_vector;
	int	REGPARM(2) (*irq_callback)(struct SH2_DRC_ *sh2, int level);
	int	is_slave;

	unsigned int	cycles_timeslice;

	struct SH2_DRC_	*other_sh2;
	int		(*run)(struct SH2_DRC_ *, int);

	// we use 68k reference cycles for easier sync
	unsigned int	m68krcycles_done;
	unsigned int	mult_m68k_to_sh2;
	unsigned int	mult_sh2_to_m68k;

	uint8_t		data_array[0x1000]; // cache (can be used as RAM)
	uint32_t	peri_regs[0x200/4]; // peripheral regs
} SH2_DRC;

#ifndef SH2_DRC_NO_SH2_TYPEDEF
typedef SH2_DRC SH2;
#endif

// the recompiler addresses both cpus of the 32X; FBNeo only uses sh2s[0]
extern SH2_DRC sh2s[2];

int  sh2_execute_drc(SH2_DRC *sh2c, int cycles);

// memory interface, implemented by the FBNeo glue
u32 REGPARM(2) p32x_sh2_read8(u32 a, SH2_DRC *sh2);
u32 REGPARM(2) p32x_sh2_read16(u32 a, SH2_DRC *sh2);
u32 REGPARM(2) p32x_sh2_read32(u32 a, SH2_DRC *sh2);
void REGPARM(3) p32x_sh2_write8 (u32 a, u32 d, SH2_DRC *sh2);
void REGPARM(3) p32x_sh2_write16(u32 a, u32 d, SH2_DRC *sh2);
void REGPARM(3) p32x_sh2_write32(u32 a, u32 d, SH2_DRC *sh2);
u32 REGPARM(3) p32x_sh2_poll_memory8(u32 a, u32 d, SH2_DRC *sh2);
u32 REGPARM(3) p32x_sh2_poll_memory16(u32 a, u32 d, SH2_DRC *sh2);
u32 REGPARM(3) p32x_sh2_poll_memory32(u32 a, u32 d, SH2_DRC *sh2);
// direct pointer to the memory region containing a (data reads)
void *p32x_sh2_get_mem_ptr(u32 a, u32 *mask, SH2_DRC *sh2);
// same, for instruction fetches (FBNeo has separate fetch maps)
void *p32x_sh2_get_code_ptr(u32 a, u32 *mask, SH2_DRC *sh2);
int p32x_sh2_mem_is_rom(u32 a, SH2_DRC *sh2);

// idle loop hook: called before executing the instruction at sh2_drc_idle_pc,
// with the guest registers and cycles stored in the context
extern u32 sh2_drc_idle_pc;
void REGPARM(1) sh2_drc_idle_hook(SH2_DRC *sh2);

#ifdef __cplusplus
}
#endif

#endif /* __SH2_H__ */
