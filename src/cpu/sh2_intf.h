#include <stdint.h>

#ifndef FASTCALL
 #undef __fastcall
 #define __fastcall
#endif

typedef unsigned char (__fastcall *pSh2ReadByteHandler)(unsigned int a);
typedef void (__fastcall *pSh2WriteByteHandler)(unsigned int a, unsigned char d);
typedef unsigned short (__fastcall *pSh2ReadWordHandler)(unsigned int a);
typedef void (__fastcall *pSh2WriteWordHandler)(unsigned int a, unsigned short d);
typedef unsigned int (__fastcall *pSh2ReadLongHandler)(unsigned int a);
typedef void (__fastcall *pSh2WriteLongHandler)(unsigned int a, unsigned int d);

extern int has_sh2;
extern INT32 cps3speedhack;

void __fastcall Sh2WriteByte(unsigned int a, unsigned char d);
unsigned char __fastcall Sh2ReadByte(unsigned int a);

int Sh2Init(int nCount);
void Sh2Exit();

void Sh2Open(const int i);
void Sh2Close();
int Sh2GetActive();

void Sh2Reset();
void Sh2Reset(unsigned int pc, unsigned r15); // hack
int Sh2Run(int cycles);

int Sh2MapMemory(unsigned char* pMemory, unsigned int nStart, unsigned int nEnd, int nType);
int Sh2MapHandler(uintptr_t nHandler, unsigned int nStart, unsigned int nEnd, int nType);

int Sh2SetReadByteHandler(int i, pSh2ReadByteHandler pHandler);
int Sh2SetWriteByteHandler(int i, pSh2WriteByteHandler pHandler);
int Sh2SetReadWordHandler(int i, pSh2ReadWordHandler pHandler);
int Sh2SetWriteWordHandler(int i, pSh2WriteWordHandler pHandler);
int Sh2SetReadLongHandler(int i, pSh2ReadLongHandler pHandler);
int Sh2SetWriteLongHandler(int i, pSh2WriteLongHandler pHandler);

void Sh2SetIRQLine(const int line, const int state);

unsigned int Sh2GetPC(int n);
void Sh2StopRun();
void Sh2SetVBR(unsigned int i);

void Sh2BurnUntilInt(int);

// lets the driver modify data read by the on-chip DMA (size: 0 byte, 1 word,
// 2 long), e.g. on CPS3 where it bypasses the decryption
typedef UINT32 (*pSh2DmaReadCallback)(UINT32 src, UINT32 data, INT32 size);
void Sh2SetDmaReadCallback(pSh2DmaReadCallback pCallback);

// Idle loop skipping: pCallback is called whenever the cpu (interpreter or
// recompiler) is about to execute the instruction at nPc. It can look at the
// registers with Sh2GetRegister() and skip time with Sh2BurnCycles().
void Sh2SetIdleHook(UINT32 nPc, void (*pCallback)());
UINT32 Sh2GetRegister(INT32 nReg); // r0-r15

// code in [start, start + len) was changed by other means than SH2 stores
// to RAM, e.g. by a driver writing to a ROM region
void Sh2InvalidateCode(UINT32 start, UINT32 len);

INT32 Sh2TotalCycles();
void Sh2NewFrame();
void Sh2BurnCycles(int cycles);
void Sh2Idle(int cycles);
void Sh2SetEatCycles(int i);

int Sh2Scan(int);


void Sh2CheatWriteByte(UINT32 a, UINT8 d); // cheat core
UINT8 Sh2CheatReadByte(UINT32 a);

extern struct cpu_core_config Sh2Config;

// depreciate this and use BurnTimerAttach directly!
#define BurnTimerAttachSh2(clock)	\
	BurnTimerAttach(&Sh2Config, clock)
