#include "cps3.h"
#include "sh2_intf.h"
#include "stream.h"

// CPS-3 sound emulation by Philip Bennett
//
// As in MAME, the chip is rendered at its own rate and kept in sync with the
// SH2: register accesses first bring the output up to the current cycle, so
// key on/off and parameter changes happen at the right time within a frame.

#define CPS3_VOICES		16
#define CPS3_SND_RATE	(42954545 / 3 / 384)
#define CPS3_SND_MAX_SAMPLES	2048

typedef struct {
	UINT16 regs[16];
	UINT32 pos;
	UINT16 frac;
} cps3_voice;

typedef struct {
	cps3_voice voice[CPS3_VOICES];
	UINT16 key;

	UINT8 * rombase;

	double gain[2];
	INT32 output_dir[2];

} cps3snd_chip;

static cps3snd_chip * chip;
static Stream stream;

static void cps3snd_render(INT16 **streams, INT32 samples)
{
	static INT32 mix[2][CPS3_SND_MAX_SAMPLES];

	if (samples > CPS3_SND_MAX_SAMPLES) samples = CPS3_SND_MAX_SAMPLES;
	memset(mix, 0, sizeof(mix[0][0]) * 2 * CPS3_SND_MAX_SAMPLES);

	INT8 * base = (INT8 *)chip->rombase;
	cps3_voice *vptr = &chip->voice[0];

	for(INT32 i=0; i<CPS3_VOICES; i++, vptr++) {
		if (chip->key & (1 << i)) {

			UINT32 start = ((vptr->regs[ 3] << 16) | vptr->regs[ 2]) - 0x400000;
			UINT32 end   = ((vptr->regs[11] << 16) | vptr->regs[10]) - 0x400000;
			UINT32 loop  = ((vptr->regs[ 9] << 16) | vptr->regs[ 7]) - 0x400000;
			UINT32 step  = vptr->regs[ 6];
			INT32 loop_enable = vptr->regs[5] & 1;

			INT32 vol_l = (INT16)vptr->regs[15];
			INT32 vol_r = (INT16)vptr->regs[14];

			UINT32 pos = vptr->pos;
			UINT32 frac = vptr->frac;

			/* Go through the buffer and add voice contributions */
			for (INT32 j=0; j<samples; j++) {
				INT32 sample;

				pos += (frac >> 12);
				frac &= 0xfff;

				if (start + pos >= end) {
					if (loop_enable) {
						pos = pos + loop - end; // keeps the overshoot, as in MAME
					} else {
						// don't force key off [hap 5/31/14]
						break;
					}
				}

				// 8bit sample store with 16bit bigend ???
				sample = base[(start + pos) ^ 1];
				frac += step;

				INT32 nLeftSample = 0, nRightSample = 0;

				if ((chip->output_dir[BURN_SND_CPS3SND_ROUTE_1] & BURN_SND_ROUTE_LEFT) == BURN_SND_ROUTE_LEFT) {
					nLeftSample += (INT32)(((sample * vol_l) >> 8) * chip->gain[BURN_SND_CPS3SND_ROUTE_1]);
				}
				if ((chip->output_dir[BURN_SND_CPS3SND_ROUTE_1] & BURN_SND_ROUTE_RIGHT) == BURN_SND_ROUTE_RIGHT) {
					nRightSample += (INT32)(((sample * vol_l) >> 8) * chip->gain[BURN_SND_CPS3SND_ROUTE_1]);
				}

				if ((chip->output_dir[BURN_SND_CPS3SND_ROUTE_2] & BURN_SND_ROUTE_LEFT) == BURN_SND_ROUTE_LEFT) {
					nLeftSample += (INT32)(((sample * vol_r) >> 8) * chip->gain[BURN_SND_CPS3SND_ROUTE_2]);
				}
				if ((chip->output_dir[BURN_SND_CPS3SND_ROUTE_2] & BURN_SND_ROUTE_RIGHT) == BURN_SND_ROUTE_RIGHT) {
					nRightSample += (INT32)(((sample * vol_r) >> 8) * chip->gain[BURN_SND_CPS3SND_ROUTE_2]);
				}

				// "left" volume register ends up on the right speaker and vice versa
				mix[0][j] += nRightSample;
				mix[1][j] += nLeftSample;
			}

			vptr->pos = pos;
			vptr->frac = frac;
		}
	}

	for (INT32 j = 0; j < samples; j++) {
		streams[0][j] = BURN_SND_CLIP(mix[0][j]);
		streams[1][j] = BURN_SND_CLIP(mix[1][j]);
	}
}

UINT8 __fastcall cps3SndReadByte(UINT32 addr)
{
	addr &= 0x000003ff;
	bprintf(PRINT_NORMAL, _T("SND Attempt to read byte value of location %8x\n"), addr);
	return 0;
}

UINT16 __fastcall cps3SndReadWord(UINT32 addr)
{
	addr &= 0x000003ff;

	stream.update();

	if (addr < 0x200)	{
		return chip->voice[addr >> 5].regs[(addr>>1) & 0xf];
	} else
	if (addr == 0x200)	{
		return chip->key;
	} else

	bprintf(PRINT_NORMAL, _T("SND Attempt to read word value of location %8x\n"), addr);
	return 0;
}

UINT32 __fastcall cps3SndReadLong(UINT32 addr)
{
	addr &= 0x000003ff;

	bprintf(PRINT_NORMAL, _T("SND Attempt to read long value of location %8x\n"), addr);
	return 0;
}

void __fastcall cps3SndWriteByte(UINT32 addr, UINT8 data)
{
	addr &= 0x000003ff;
	bprintf(PRINT_NORMAL, _T("SND Attempt to write byte value %2x to location %8x\n"), data, addr);
}

void __fastcall cps3SndWriteWord(UINT32 addr, UINT16 data)
{
	addr &= 0x000003ff;

	stream.update();

	if (addr < 0x200) {
		chip->voice[addr >> 5].regs[(addr>>1) & 0xf] = data;
		//bprintf(PRINT_NORMAL, _T("SND Attempt to write word value %4x to Chip[%02d][%02d] %s\n"), data, addr >> 5, (addr>>2) & 7, (addr & 0x02) ? "lo" : "hi" );
	} else
	if (addr == 0x200) {
		UINT16 key = data;
		for (INT32 i = 0; i < CPS3_VOICES; i++) {
			// Key off -> Key on
			if ((key & (1 << i)) && !(chip->key & (1 << i)))	{
				chip->voice[i].frac = 0;
				chip->voice[i].pos = 0;
			}
		}
		chip->key = key;
	} else
		bprintf(PRINT_NORMAL, _T("SND Attempt to write word value %4x to location %8x\n"), data, addr);

}

void __fastcall cps3SndWriteLong(UINT32 addr, UINT32 data)
{
	//addr &= 0x000003ff;
	if (addr == 0x240e0210) return; // NOP (jojoba*)
	bprintf(PRINT_NORMAL, _T("SND Attempt to write long value %8x to location %8x\n"), data, addr);
}

INT32 cps3SndInit(UINT8 * sndrom)
{
	chip = (cps3snd_chip *)BurnMalloc( sizeof(cps3snd_chip) );
	if ( chip ) {
		memset( chip, 0, sizeof(cps3snd_chip) );
		chip->rombase = sndrom;

		/* CPS-3 Sound chip clock: 42954545 / 3 / 384 = 37286.89 */
		stream.init(CPS3_SND_RATE, nBurnSoundRate, 2, 0, cps3snd_render);
		stream.set_buffered(Sh2TotalCycles, 25000000);

		chip->gain[BURN_SND_CPS3SND_ROUTE_1] = 1.00;
		chip->gain[BURN_SND_CPS3SND_ROUTE_2] = 1.00;
		chip->output_dir[BURN_SND_CPS3SND_ROUTE_1] = BURN_SND_ROUTE_LEFT;
		chip->output_dir[BURN_SND_CPS3SND_ROUTE_2] = BURN_SND_ROUTE_RIGHT;

		return 0;
	}
	return 1;
}

void cps3SndSetRoute(INT32 nIndex, double nVolume, INT32 nRouteDir)
{
	chip->gain[nIndex] = nVolume;
	chip->output_dir[nIndex] = nRouteDir;
}

void cps3SndReset()
{
}

void cps3SndExit()
{
	stream.exit();
	BurnFree( chip );
}

void cps3SndUpdate()
{
	if (!pBurnSoundOut) {
		// TODO: ???
		// chip->key = 0;
		return;
	}

	stream.render(pBurnSoundOut, nBurnSoundLen);
}

INT32 cps3SndScan(INT32 nAction)
{
	if (nAction & ACB_DRIVER_DATA) {

		SCAN_VAR( chip->voice );
		SCAN_VAR( chip->key );

	}
	return 0;
}
