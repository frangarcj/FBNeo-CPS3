// FBNeo replacement for PicoDrive's pico/pico_int.h.
//
// The SH2 recompiler in sh2/ and drc/ is taken unmodified (or with small,
// marked patches) from PicoDrive. This header provides the few PicoDrive
// internals it depends on, configured for the CPS3 memory map. The glue that
// implements the functions declared here lives in src/cpu/sh2/sh2_drc.inl.

#ifndef PICO_INT_H
#define PICO_INT_H

#include <stdio.h>
#include <string.h>
#include "pico_types.h"
#ifdef USE_LIBRETRO_VFS // no file i/o in here, don't pull in the VFS
#undef USE_LIBRETRO_VFS
#include "pico_port.h"
#define USE_LIBRETRO_VFS
#else
#include "pico_port.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PICO_INTERNAL
#define PICO_INTERNAL_ASM

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif

// logging
#define EL_SVP     0x00004000
#define EL_STATUS  0x40000000
#define EL_ANOMALY 0x80000000
void sh2_drc_log(const char *fmt, ...);
#define elprintf(w, f, ...) sh2_drc_log(f "\n", ##__VA_ARGS__)
#define elprintf_sh2(sh2, w, f, ...) elprintf(w, f, ##__VA_ARGS__)

// FBNeo maps SH2 memory in 64KB pages; the recompiler's lookup tables use the
// same granularity so they can mirror pSh2Ext->MemMap entry by entry.
#define SH2_READ_SHIFT  16
#define SH2_WRITE_SHIFT 16

// FBNeo keeps SH2 memory as host-endian 32 bit words (byte address ^ 3,
// word address ^ 2), whereas PicoDrive uses host-endian 16 bit words.
#define DRC_MEM_32LE    1

// RAM tracked for self-modifying code ("SDRAM" in PicoDrive's 32X map).
// On CPS3 this is the 512KB main RAM at 0x02000000 (plus its mirrors).
#define DRC_SDRAM_BASE  0x02000000
#define DRC_SDRAM_AMASK 0xc7f80000
#define DRC_SDRAM_SIZE  0x80000

// keep count of blocks marked in drcblk_ram/drcblk_da
#define DRC_COUNT_MARKED_BLOCKS 1

// call sh2_drc_idle_hook() before the instruction at sh2_drc_idle_pc
#define DRC_IDLE_HOOK 1

#define SH2_DRCBLK_RAM_SHIFT 1
#define SH2_DRCBLK_DA_SHIFT  1

#define POPT_EN_DRC      (1 << 17)
#define P32XF_DRC_ROM_C  (1 << 8)

struct PicoInterface {
  unsigned int opt;
};

struct Pico32x {
  unsigned int emu_flags;
};

struct Pico32xMem {
  unsigned char drcblk_ram[DRC_SDRAM_SIZE >> SH2_DRCBLK_RAM_SHIFT];
  unsigned char drclit_ram[DRC_SDRAM_SIZE >> SH2_DRCBLK_RAM_SHIFT];
  unsigned char drcblk_da[2][1 << (12 - SH2_DRCBLK_DA_SHIFT)];
  unsigned char drclit_da[2][1 << (12 - SH2_DRCBLK_DA_SHIFT)];
  int marked_blocks; // blocks in RAM or the data array, see DRC_COUNT_MARKED_BLOCKS
};

extern struct PicoInterface PicoIn;
extern struct Pico32x Pico32x;
extern struct Pico32xMem *Pico32xMem;

void memset32(void *dest, int c, int count);

// translation cache memory and host cache maintenance
void cache_flush_d_inval_i(void *start_addr, void *end_addr);
void *plat_mem_get_for_drc(size_t size);
int plat_mem_set_exec(void *ptr, size_t size);

// Hosts with W^X JIT memory (Apple Silicon) must switch the translation
// cache between writable and executable around code generation/patching.
void drc_jit_write_begin(void);
void drc_jit_write_end(void);

#ifdef __cplusplus
}
#endif

#endif // PICO_INT_H
