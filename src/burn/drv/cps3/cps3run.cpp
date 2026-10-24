/**********************************************************************

CPS3 Driver (preliminary)

Decryption by Andreas Naive

Driver by David Haywood
 with help from Tomasz Slanina and ElSemi

Sound emulation by Philip Bennett

SCSI code by ElSemi

Latest fixes by MetalliC

**********************************************************************

Port to FBA by OopsWare

**********************************************************************/

#include "cps3.h"
#include "sh2_intf.h"
#include "cps3_simd.h"

#define	BE_GFX		1
#define BE_GFX_CRAM 0   // do not touch!
//#define	FAST_BOOT	1
#define SPEED_HACK	1		// Default should be 1, if not FPS would drop.

static UINT8 *Mem = NULL, *MemEnd = NULL;
static UINT8 *RamStart, *RamEnd;

static UINT8 *RomBios;
static UINT8 *RomGame;
static UINT8 *RomGame_D;
static UINT8 *RomUser;

static UINT8 *RamMain;
static UINT32 *RamSpr;
static UINT32 *SprList;
static UINT16 *RamPal;

static UINT32 *RamCRam;

static UINT32 *RamSS;

static UINT32 *RamVReg;
static UINT32 *RamVRegBuf;

static UINT8 *RamC000;
static UINT8 *RamC000_D;

static UINT16 *EEPROM;

UINT16 *Cps3CurPal;
static UINT32 *RamScreen;

UINT8 cps3_reset = 0;
UINT8 cps3_palette_change = 0;

UINT32 cps3_key1, cps3_key2, cps3_isSpecial;
UINT32 cps3_speedup_ram_address, cps3_speedup_code_address;
UINT8 cps3_dip;
UINT8 cps3_fake_dip;
UINT32 cps3_region_address, cps3_ncd_address;

static UINT32 cps3_data_rom_size;

UINT8 Cps3But1[16];
UINT8 Cps3But2[16];
UINT8 Cps3But3[16];

static UINT16 Cps3Input[4] = {0, 0, 0, 0};

static ClearOpposite<2, UINT16> clear_opposite;

static UINT16 ss_vscroll = 0;
static UINT32 ss_pal_base = 0;

static UINT32 cram_bank = 0;
static UINT16 cps3_current_eeprom_read = 0;
static UINT32 gfxflash_bank = 0;

static UINT32 paldma_source = 0;
static UINT32 paldma_dest = 0;
static UINT32 paldma_fade = 0;
static UINT32 paldma_length = 0;

static UINT32 chardma_source = 0;
static UINT32 chardma_table_address = 0;

// DMA timing as in MAME: palette and character DMA set a busy bit, and irq 10
// fires 100us later (a guess, actual speed is unknown). The display list DMA
// is busy for 4us. Times are in SH2 cycles of the current frame, -1 = idle.
#define CPS3_DMA_IRQ_DELAY		(25000000 / 1000000 * 100)
#define CPS3_SPRITE_DMA_DELAY	(25000000 / 1000000 * 4)

static INT32 dma_irq_cycle = -1;
static INT32 spritelist_dma_cycle = -1;
static UINT16 dma_status = 0;

static UINT16 spritelist_dma = 0;
static UINT16 spritelist_dma_prev = 0;

// (re)start the delay until the DMA irq, the frame loop raises it
static void cps3_start_dma_timer()
{
	dma_irq_cycle = Sh2TotalCycles() + CPS3_DMA_IRQ_DELAY;
	Sh2StopRun();
}

static INT32 cps3_gfx_width, cps3_gfx_height;
static INT32 cps3_gfx_max_x, cps3_gfx_max_y;
static INT32 cps3_screen_clean_w = 0, cps3_screen_clean_h = 0; // RamScreen area known to be 0

static INT32 nExtraCycles;

// -- AMD/Fujitsu 29F016 --------------------------------------------------

enum {
	FM_NORMAL,	// normal read/write
	FM_READID,	// read ID
	FM_READSTATUS,	// read status
	FM_WRITEPART1,	// first half of programming, awaiting second
	FM_CLEARPART1,	// first half of clear, awaiting second
	FM_SETMASTER,	// first half of set master lock, awaiting on/off
	FM_READAMDID1,	// part 1 of alt ID sequence
	FM_READAMDID2,	// part 2 of alt ID sequence
	FM_READAMDID3,	// part 3 of alt ID sequence
	FM_ERASEAMD1,	// part 1 of AMD erase sequence
	FM_ERASEAMD2,	// part 2 of AMD erase sequence
	FM_ERASEAMD3,	// part 3 of AMD erase sequence
	FM_ERASEAMD4,	// part 4 of AMD erase sequence
	FM_BYTEPROGRAM
};

typedef struct
{
	INT32 status;
	INT32 flash_mode;
	INT32 flash_master_lock;
} cps3_flash_chip;

static cps3_flash_chip main_flash;

void cps3_flash_init(cps3_flash_chip * chip/*, void *data*/)
{
	memset(chip, 0, sizeof(cps3_flash_chip));
	chip->status = 0x80;
	chip->flash_mode = FM_NORMAL;
	chip->flash_master_lock = 0;
}

UINT32 cps3_flash_read(cps3_flash_chip * chip, UINT32 addr)
{
	switch( chip->flash_mode ) {
	case FM_READAMDID3:
	case FM_READID:
		
		switch (addr & 0x7fffff) {
			case 0:	return 0x04040404;	//(c->maker_id << 0) | (c->maker_id << 8) | (c->maker_id << 16) | (c->maker_id << 24);
			case 4: return 0xadadadad;	//(c->device_id << 0) | (c->device_id << 8) | (c->device_id << 16) | (c->device_id << 24);
			case 8: return 0x00000000;
			//case 12: return (c->flash_master_lock) ? 0x01010101 : 0x00000000
		}
		return 0;

	case FM_ERASEAMD4:
		chip->status ^= ( 1 << 6 ) | ( 1 << 2 );
	case FM_READSTATUS:
		return (chip->status << 0) | (chip->status << 8) | (chip->status << 16) | (chip->status << 24);

	case FM_NORMAL:
	default:
		//
		return 0;
	}
}

void cps3_flash_write(cps3_flash_chip * chip, UINT32 addr, UINT32 data)
{
	bprintf(1, _T("FLASH to write long value %8x to location %8x\n"), data, addr);
	
	switch( chip->flash_mode )	{
	case FM_NORMAL:
	case FM_READSTATUS:
	case FM_READID:
	case FM_READAMDID3:
		switch( data & 0xff ) {
		case 0xf0:
		case 0xff: chip->flash_mode = FM_NORMAL;	break;
		case 0x90: chip->flash_mode = FM_READID;	break;
		case 0x40:
		case 0x10: chip->flash_mode = FM_WRITEPART1;break;
		case 0x50:	// clear status reg
			chip->status = 0x80;
			chip->flash_mode = FM_READSTATUS;
			break;
		case 0x20: chip->flash_mode = FM_CLEARPART1;break;
		case 0x60: chip->flash_mode = FM_SETMASTER;	break;
		case 0x70: chip->flash_mode = FM_READSTATUS;break;
		case 0xaa:	// AMD ID select part 1
			if ((addr & 0xffff) == (0x555 << 2))
				chip->flash_mode = FM_READAMDID1;
			break;
		default:
			//logerror( "Unknown flash mode byte %x\n", data & 0xff );
			//bprintf(1, _T("FLASH to write long value %8x to location %8x\n"), data, addr);
			break;
		}	
		break;
	case FM_READAMDID1:
		if ((addr & 0xffff) == (0x2aa <<2) && (data & 0xff) == 0x55 ) {
			chip->flash_mode = FM_READAMDID2;
		} else {
			//logerror( "unexpected %08x=%02x in FM_READAMDID1\n", address, data & 0xff );
			chip->flash_mode = FM_NORMAL;
		}
		break;
	case FM_READAMDID2:
		if ((addr & 0xffff) == (0x555<<2) && (data & 0xff) == 0x90) {
			chip->flash_mode = FM_READAMDID3;
		} else if((addr & 0xffff) == (0x555<<2) && (data & 0xff) == 0x80) {
			chip->flash_mode = FM_ERASEAMD1;
		} else if((addr & 0xffff) == (0x555<<2) && (data & 0xff) == 0xa0) {
			chip->flash_mode = FM_BYTEPROGRAM;
		} else if((addr & 0xffff) == (0x555<<2) && (data & 0xff) == 0xf0) {
			chip->flash_mode = FM_NORMAL;
		} else {
			// logerror( "unexpected %08x=%02x in FM_READAMDID2\n", address, data & 0xff );
			chip->flash_mode = FM_NORMAL;
		}
		break;				
	}
}

// ------------------------------------------------------------------------

static UINT16 rotate_left(UINT16 value, INT32 n)
{
   INT32 aux = value>>(16-n);
   return ((value<<n)|aux) & 0xffff;
}

static UINT16 rotxor(UINT16 val, UINT16 x)
{
	UINT16 res;
	res = val + rotate_left(val,2);
	res = rotate_left(res,4) ^ (res & (val ^ x));
	return res;
}

static UINT32 cps3_mask(UINT32 address, UINT32 key1, UINT32 key2)
{
	UINT16 val;
	address ^= key1;
	val = (address & 0xffff) ^ 0xffff;
	val = rotxor(val, key2 & 0xffff);
	val ^= (address >> 16) ^ 0xffff;
	val = rotxor(val, key2 >> 16);
	val ^= (address & 0xffff) ^ (key2 & 0xffff);
	return val | (val << 16);
}

static void cps3_decrypt_bios(void)
{
	UINT32 * coderegion = (UINT32 *)RomBios;
	for (INT32 i=0; i<0x20000; i+=4) {
		UINT32 xormask = cps3_mask(i, cps3_key1, cps3_key2);
		coderegion[i/4] ^= xormask;
	}
}

// The SH2 DMA bypasses the decryption, so reapply the encryption to what it
// reads from the BIOS and program flash, as in MAME. This is how the flash
// commands in the BIOS and the ROM checks get the data they expect.
static UINT32 cps3_sh2_dma_read(UINT32 src, UINT32 data, INT32 size)
{
	UINT32 xormask;

	if (src < 0x80000)
		xormask = cps3_mask(src & ~3, cps3_key1, cps3_key2);
	else if (src >= 0x06000000 && src < 0x07000000 && !cps3_isSpecial)
		xormask = cps3_mask(src & ~3, cps3_key1, cps3_key2);
	else
		return data;

	switch (size) {
		case 0: return data ^ ((xormask >> ((~src & 3) * 8)) & 0xff);
		case 1: return data ^ (xormask & 0xffff); // both halves of the mask are the same
	}
	return data ^ xormask;
}

static void cps3_decrypt_game(void)
{
	UINT32 * coderegion = (UINT32 *)RomGame;
	UINT32 * decrypt_coderegion = (UINT32 *)RomGame_D;
	
	for (INT32 i=0; i<0x1000000; i+=4) {
		UINT32 xormask = cps3_mask(i + 0x06000000, cps3_key1, cps3_key2);
		decrypt_coderegion[i/4] = coderegion[i/4] ^ xormask;
	}
}


static INT32 last_normal_byte = 0;

static UINT32 process_byte( UINT8 real_byte, UINT32 destination, INT32 max_length )
{
	UINT8 * dest = (UINT8 *) RamCRam;
	//printf("process byte for destination %08x\n", destination);
	destination &= 0x7fffff;

	if (real_byte&0x40) {
		INT32 tranfercount = 0;
		//printf("Set RLE Mode\n");
		INT32 cps3_rle_length = (real_byte&0x3f)+1;
		//printf("RLE Operation (length %08x\n", cps3_rle_length );
		while (cps3_rle_length) {
#if BE_GFX_CRAM
			dest[((destination+tranfercount)&0x7fffff)] = (last_normal_byte&0x3f);
#else
			dest[((destination+tranfercount)&0x7fffff)^3] = (last_normal_byte&0x3f);
#endif
			//cps3_char_ram_dirty[((destination+tranfercount)&0x7fffff)/0x100] = 1;
			//cps3_char_ram_is_dirty = 1;
			//printf("RLE WRite Byte %08x, %02x\n", destination+tranfercount, real_byte);

			tranfercount++;
			cps3_rle_length--;
			max_length--;
			if ((destination+tranfercount) > 0x7fffff)  return max_length;
	//      if (max_length==0) return max_length; // this is meant to abort the transfer if we exceed dest length,, not working
		}
		return tranfercount;
	} else {
		//printf("Write Normal Data\n");
#if BE_GFX_CRAM
		dest[(destination&0x7fffff)] = real_byte;
#else
		dest[(destination&0x7fffff)^3] = real_byte;
#endif
		last_normal_byte = real_byte;
		//cps3_char_ram_dirty[(destination&0x7fffff)/0x100] = 1;
		//cps3_char_ram_is_dirty = 1;
		return 1;
	}
}

static void cps3_do_char_dma( UINT32 real_source, UINT32 real_destination, UINT32 real_length )
{
	UINT8 * sourcedata = RomUser;
	INT32 length_remaining = real_length;
	last_normal_byte = 0;
	while (length_remaining) {
		UINT8 current_byte = sourcedata[ real_source ];
		real_source++;

		if (current_byte & 0x80) {
			UINT8 real_byte;
			UINT32 length_processed;
			current_byte &= 0x7f;

			real_byte = sourcedata[ (chardma_table_address+current_byte*2+0) ];
			//if (real_byte&0x80) return;
			length_processed = process_byte( real_byte, real_destination, length_remaining );
			length_remaining -= length_processed; // subtract the number of bytes the operation has taken
			real_destination += length_processed; // add it onto the destination
			if (real_destination>0x7fffff) return;
			if (length_remaining<=0) return; // if we've expired, exit

			real_byte = sourcedata[ (chardma_table_address+current_byte*2+1) ];
			//if (real_byte&0x80) return;
			length_processed = process_byte( real_byte, real_destination, length_remaining );
			length_remaining -= length_processed; // subtract the number of bytes the operation has taken
			real_destination += length_processed; // add it onto the destination
			if (real_destination>0x7fffff) return;
			if (length_remaining<=0) return;  // if we've expired, exit
		} else {
			UINT32 length_processed;
			length_processed = process_byte( current_byte, real_destination, length_remaining );
			length_remaining -= length_processed; // subtract the number of bytes the operation has taken
			real_destination += length_processed; // add it onto the destination
			if (real_destination>0x7fffff) return;
			if (length_remaining<=0) return;  // if we've expired, exit
		}
	}
}

static UINT16 lastb;
static UINT16 lastb2;

static UINT32 ProcessByte8(UINT8 b, UINT32 dst_offset)
{
	UINT8 * destRAM = (UINT8 *) RamCRam;
 	INT32 l=0;

 	if(lastb==lastb2) {	//rle
 		INT32 rle=(b+1)&0xff;

 		for(INT32 i=0;i<rle;++i) {
#if BE_GFX_CRAM
			destRAM[(dst_offset&0x7fffff)] = lastb;
#else
			destRAM[(dst_offset&0x7fffff)^3] = lastb;
#endif
			//cps3_char_ram_dirty[(dst_offset&0x7fffff)/0x100] = 1;
			//cps3_char_ram_is_dirty = 1;

			dst_offset++;
 			++l;
 		}
 		lastb2=0xffff;
 		return l;
 	} else {
 		lastb2=lastb;
 		lastb=b;
#if BE_GFX_CRAM
		destRAM[(dst_offset&0x7fffff)] = b;
#else
		destRAM[(dst_offset&0x7fffff)^3] = b;
#endif
		//cps3_char_ram_dirty[(dst_offset&0x7fffff)/0x100] = 1;
		//cps3_char_ram_is_dirty = 1;
 		return 1;
 	}
}

static void cps3_do_alt_char_dma(UINT32 src, UINT32 real_dest, UINT32 real_length )
{
	UINT8 * px = RomUser;
	UINT32 start = real_dest;
	UINT32 ds = real_dest;

	lastb=0xfffe;
	lastb2=0xffff;

	while(1) {
		UINT8 ctrl=px[ src ];
 		++src;

		for(INT32 i=0;i<8;++i) {
			UINT8 p = px[ src ];

			if(ctrl&0x80) {
				UINT8 real_byte;
				p &= 0x7f;
				real_byte = px[ (chardma_table_address+p*2+0) ];
				ds += ProcessByte8(real_byte,ds);
				real_byte = px[ (chardma_table_address+p*2+1) ];
				ds += ProcessByte8(real_byte,ds);
 			} else {
 				ds += ProcessByte8(p,ds);
 			}
 			++src;
 			ctrl<<=1;

			if((ds-start)>=real_length)
				return;
 		}
	}
}

static void cps3_process_character_dma(UINT32 address)
{
	for (INT32 i=0; i<0x1000; i+=3) {
		UINT32 dat1 = RamCRam[i+0+(address)];
		UINT32 dat2 = RamCRam[i+1+(address)];
		UINT32 dat3 = RamCRam[i+2+(address)];
		UINT32 real_source      = (dat3<<1)-0x400000;
		UINT32 real_destination =  dat2<<3;
		UINT32 real_length      = (((dat1&0x001fffff)+1)<<3);
		
		if (dat1 & 0x01000000) break;	// end of list marker
		
		switch ( dat1 & 0x00e00000 ) {
		case 0x00800000:
			chardma_table_address = real_source;
			break;
		case 0x00400000:
			cps3_do_char_dma( real_source, real_destination, real_length );
			break;
		case 0x00600000:
			//bprintf(PRINT_NORMAL, _T("Character DMA (alt) start %08x to %08x with %d\n"), real_source, real_destination, real_length);
			/* 8bpp DMA decompression
			   - this is used on SFIII NG Sean's Stage ONLY */
			cps3_do_alt_char_dma( real_source, real_destination, real_length );
			break;
		case 0x00000000:
			// Red Earth need this. 8192 byte trans to 0x00003000 (from 0x007ec000???)
			// seems some stars(6bit alpha) without compress
			//bprintf(PRINT_NORMAL, _T("Character DMA (redearth) start %08x to %08x with %d\n"), real_source, real_destination, real_length);

			//memcpy( (UINT8 *)RamCRam + real_destination, RomUser + real_source, real_length );
			for (INT32 j = 0; j < real_length; j++)
				((UINT8 *)RamCRam)[real_destination + j] = RomUser[(real_source + j) ^ 3];
			break;
		default:
			bprintf(PRINT_NORMAL, _T("Character DMA Unknown DMA List Command Type %08x\n"), dat1);
		}
	}

	dma_status |= 2;
	cps3_start_dma_timer();
}

static INT32 MemIndex()
{
	UINT8 *Next; Next = Mem;
	RomBios 	= Next; Next += 0x0080000;

	RomUser		= Next; Next += cps3_data_rom_size;	// 0x5000000;
	
	RomGame 	= Next; Next += 0x1000000;
	RomGame_D 	= Next; Next += 0x1000000;

	RamStart	= Next;
	
	RamC000		= Next; Next += 0x0000400;
	RamC000_D	= Next; Next += 0x0000400;

	RamMain		= Next; Next += 0x0080000;

	RamPal		= (UINT16 *) Next; Next += 0x0020000 * sizeof(UINT16);
	RamSpr		= (UINT32 *) Next; Next += (0x0080000/4) * sizeof(UINT32);
	SprList		= (UINT32 *) Next; Next += (0x0080000/4) * sizeof(UINT32);

	RamCRam		= (UINT32 *) Next; Next += 0x0200000 * sizeof(UINT32);
	RamSS		= (UINT32 *) Next; Next += 0x0004000 * sizeof(UINT32);
	
	RamVReg		= (UINT32 *) Next; Next += 0x0000040 * sizeof(UINT32);
	RamVRegBuf  = (UINT32 *) Next; Next += 0x0000040 * sizeof(UINT32);

	RamEnd		= Next;
	
	EEPROM		= (UINT16 *) Next; Next += 0x0000100 * sizeof(UINT16);
	
	Cps3CurPal  = (UINT16 *) Next; Next += 0x020002 * sizeof(UINT16); // iq_132 - layer disable, +1 to keep things aligned
	RamScreen	= (UINT32 *) Next; Next += (512 * 2) * (224 * 2 + 32) * sizeof(UINT32);

	MemEnd		= Next;
	return 0;
}

UINT8 __fastcall cps3ReadByte(UINT32 addr)
{
	addr &= 0xc7ffffff;
	
//	switch (addr) {
//
//	default:
//		bprintf(PRINT_NORMAL, _T("Attempt to read byte value of location %8x\n"), addr);
//	}
	return 0;
}

UINT16 __fastcall cps3ReadWord(UINT32 addr)
{
	addr &= 0xc7ffffff;
	
	switch (addr) {
	
	// redearth will read this !!!
#if 0
	case 0x040c0000:
		return RamVReg[0] >> 16;
	case 0x040c0002:
		return RamVReg[0] & 0xffff;
	case 0x040c0004:
		return RamVReg[1] >> 16;
	case 0x040c0006:
		return RamVReg[1] & 0xffff;
#else
	case 0x040c0000:
	case 0x040c0002:
	case 0x040c0004:
	case 0x040c0006:
		return 0;
#endif

	case 0x040c000c:
		if (spritelist_dma_cycle != -1 && Sh2TotalCycles() >= spritelist_dma_cycle) {
			spritelist_dma_cycle = -1;
			dma_status &= ~1;
		}
		return dma_status;

	case 0x040c000e: // ??
		return 0;

	case 0x05000000: return ~Cps3Input[1];
	case 0x05000002: return ~Cps3Input[0];
	case 0x05000004: return ~Cps3Input[3];
	case 0x05000006: return ~Cps3Input[2];

	case 0x05140000:
	case 0x05140002:
		// cd-rom read
		
		break;
		
	default:
		if ((addr >= 0x05000a00) && (addr < 0x05000a20)) {
			// cps3_unk_io_r
			return 0xffff;
		} else
		if ((addr >= 0x05001000) && (addr < 0x05001204)) {
			// EEPROM
			addr -= 0x05001000;
			if (addr >= 0x100 && addr < 0x180) {
#ifdef LSB_FIRST
			cps3_current_eeprom_read = EEPROM[((addr-0x100) >> 1) ^ 1];
#else
			cps3_current_eeprom_read = EEPROM[((addr-0x100) >> 1)];
#endif
			} else
			if (addr == 0x202)
				return cps3_current_eeprom_read;
		} else
		bprintf(PRINT_NORMAL, _T("Attempt to read word value of location %8x\n"), addr);
	}
	return 0;
}

UINT32 __fastcall cps3ReadLong(UINT32 addr)
{
	addr &= 0xc7ffffff;
		
	switch (addr) {
	case 0x04200000:
		bprintf(PRINT_NORMAL, _T("GFX Read Flash ID, cram bank %04x gfx flash bank: %04x\n"), cram_bank, gfxflash_bank);
		return 0x0404adad;

	default:
		bprintf(PRINT_NORMAL, _T("Attempt to read long value of location %8x\n"), addr);
	}
	return 0;
}

void __fastcall cps3WriteByte(UINT32 addr, UINT8 data)
{
	addr &= 0xc7ffffff;
	
	switch (addr) {
	
	// SS registers (as in MAME, only bytes 1 and 3 of each long are connected)
	case 0x05050021: ss_vscroll = (ss_vscroll & 0xff00) | data; break;
	case 0x05050023: ss_vscroll = (ss_vscroll & 0x00ff) | (data << 8); break;
	case 0x05050025: ss_pal_base = data; break;

	default:
		if ((addr >= 0x05050000) && (addr < 0x05060000)) {
			// VideoReg

		} else
			bprintf(PRINT_NORMAL, _T("Attempt to write byte value   %02x to location %8x\n"), data, addr);
	}
}

inline static UINT8 cps3_get_fade(INT32 c, INT32 f)
{
	// bit 7 unused, explicit masked out
	// bit 6 fade enable / disable
	// bit 5 fade mode (1 = invert input values and output)
	// bit 4-0 fade value
	if (f & 0x40) // Fading enable / disable
		c = (f & 0x20) ? ((((c ^ 0x1f) * (~f & 0x1f)) >> 5) ^ 0x1f) : (c * (f & 0x1f) >> 5);
	return c;
}

static void bankswitch_cram()
{
	Sh2MapMemory(((UINT8 *)RamCRam) + (cram_bank << 20), 0x04100000, 0x041fffff, MAP_RAM);
}

void __fastcall cps3WriteWord(UINT32 addr, UINT16 data)
{
	addr &= 0xc7ffffff;
	
	switch (addr) {

	case 0x040c0080: break;
	case 0x040c0082:
		spritelist_dma_prev = spritelist_dma;
		spritelist_dma = data;
		if (spritelist_dma_cycle != -1 && Sh2TotalCycles() >= spritelist_dma_cycle) {
			spritelist_dma_cycle = -1;
			dma_status &= ~1;
		}
		if (!(dma_status & 1) && (spritelist_dma & 9) == 8 && (spritelist_dma_prev & 9) == 9) // 0->1
		{
			for (int i = 0; i < 0x2000/4; i += 4)
			{
				memcpy(SprList + i, RamSpr + i, 4*sizeof(UINT32)); // copy main list record
				UINT32 dat = RamSpr[i];
				if (dat & 0x80000000)
					break;
				UINT32 offs =   (dat & 0x00007fff) << 2;
				UINT32 length = (dat & 0x01ff0000) >> 16;
				memcpy(SprList + offs, RamSpr + offs, length*4*sizeof(UINT32)); // copy sublist
			}
			memcpy(RamVRegBuf, RamVReg, 0x40 * sizeof(UINT32));

			// busy for a moment, skips the repeated 8/9 writes
			dma_status |= 1;
			spritelist_dma_cycle = Sh2TotalCycles() + CPS3_SPRITE_DMA_DELAY;
		}
		break;

	case 0x040c0084: break;
	case 0x040c0086:
		if (cram_bank != (data & 7)) {
			cram_bank = data & 7;
			//bprintf(PRINT_NORMAL, _T("CRAM bank set to %d\n"), data);
			bankswitch_cram();
		}
		break;

	case 0x040c0088:
	//case 0x040c008a:
		gfxflash_bank = data - 2;
		//bprintf(PRINT_NORMAL, _T("gfxflash bank set to %04x\n"), data);
		break;
	
	// cps3_characterdma_w
	//case 0x040c0094:
	case 0x040c0096: 
		chardma_source = data; 
		break;
	case 0x040c0098:
	//case 0x040c009a:
		if (data & 0x0040)
			cps3_process_character_dma( chardma_source | ((data & 0x003f) << 16) );
		break;

	// cps3_palettedma_w
	case 0x040c00a0: paldma_source = (paldma_source & 0x0000ffff) | (data << 16); break;
	case 0x040c00a2: paldma_source = (paldma_source & 0xffff0000) | (data <<  0); break;
	case 0x040c00a4: paldma_dest = (paldma_dest & 0x0000ffff) | (data << 16); break;
	case 0x040c00a6: paldma_dest = (paldma_dest & 0xffff0000) | (data <<  0); break;
	case 0x040c00a8: paldma_fade = (paldma_fade & 0x0000ffff) | (data << 16); break;
	case 0x040c00aa: paldma_fade = (paldma_fade & 0xffff0000) | (data <<  0); break;
	case 0x040c00ac: paldma_length = data; break;
	case 0x040c00ae:
		//bprintf(PRINT_NORMAL, _T("palettedma [%04x]  from %08x to %08x fade %08x size %d\n"), data, (paldma_source << 1), paldma_dest, paldma_fade, paldma_length);
		if (data & 0x0002) {
			UINT32 length = paldma_length | ((data & 1) << 16); // 17 bits, as in MAME
			for (UINT32 i=0; i<length; i++) {
				UINT16 * src = (UINT16 *)RomUser;
				UINT16 coldata = src[(paldma_source - 0x200000 + i)];
				
#ifdef LSB_FIRST
				coldata = (coldata << 8) | (coldata >> 8);
#endif

				UINT32 r = (coldata & 0x001F) >>  0;
				UINT32 g = (coldata & 0x03E0) >>  5;
				UINT32 b = (coldata & 0x7C00) >> 10;
				if (paldma_fade & 0x40400040) {
					r = cps3_get_fade(r, (paldma_fade & 0x7f000000)>>24);
					g = cps3_get_fade(g, (paldma_fade & 0x007f0000)>>16);
					b = cps3_get_fade(b, (paldma_fade & 0x0000007f)>>0);
					coldata = (coldata & 0x8000) | (r << 0) | (g << 5) | (b << 10);
				}
				
				r = r << 3;
				g = g << 3;
				b = b << 3;

				UINT32 colournum = (paldma_dest + i) & 0x1ffff;
#ifdef LSB_FIRST
				RamPal[colournum ^ 1] = coldata;
#else
				RamPal[colournum] = coldata;
#endif
				Cps3CurPal[colournum] = BurnHighCol(r, g, b, 0);
			}

			dma_status |= 4;
			cps3_start_dma_timer();
		}
		break;
	
	case 0x04200554:
	case 0x04200aaa:
		// gfx flash command 
		// "0xaa 0x55 0x90" set flash to get id 
		// "0xaa 0x55 0xf0" set flash to normal read
		break;
	
	// SS registers (as in MAME, only bytes 1 and 3 of each long are connected)
	case 0x05050020: ss_vscroll = (ss_vscroll & 0xff00) | (data & 0xff); break;
	case 0x05050022: ss_vscroll = (ss_vscroll & 0x00ff) | ((data & 0xff) << 8); break;
	case 0x05050024: ss_pal_base = data & 0xff; break;
	case 0x05050026: break;
			
	case 0x05100000:
		Sh2SetIRQLine(12, CPU_IRQSTATUS_NONE);
		break;
	case 0x05110000:
		Sh2SetIRQLine(10, CPU_IRQSTATUS_NONE);
		break;

	case 0x05140000:
	case 0x05140002:
		// cd-rom read
		
		break;
			
	default:
		if ((addr >= 0x040c0000) && (addr < 0x040c0100)) {
			// 0x040C0000 ~ 0x040C001f : cps3_unk_vidregs
			// 0x040C0020 ~ 0x040C002b : tilemap20_regs_base
			// 0x040C0030 ~ 0x040C003b : tilemap30_regs_base
			// 0x040C0040 ~ 0x040C004b : tilemap40_regs_base
			// 0x040C0050 ~ 0x040C005b : tilemap50_regs_base
			// 0x040C0060 ~ 0x040C007f : cps3_fullscreenzoom

			addr &= 0xff;
#ifdef LSB_FIRST
			((UINT16 *)RamVReg)[ (addr >> 1) ^ 1 ] = data;
#else
			((UINT16 *)RamVReg)[ (addr >> 1) ] = data;
#endif
			
		} else
		if ((addr >= 0x05000000) && (addr < 0x05001000)) {
			
			
		} else 
		if ((addr >= 0x05001000) && (addr < 0x05001204)) {
			// EEPROM
			addr -= 0x05001000;
			if ((addr>=0x080) && (addr<0x100))
#ifdef LSB_FIRST
			EEPROM[((addr-0x080) >> 1) ^ 1] = data;
#else
			EEPROM[((addr-0x080) >> 1)] = data;
#endif
		} else
		if ((addr >= 0x05050000) && (addr < 0x05060000)) {
			// unknow i/o

		} else
				
		bprintf(PRINT_NORMAL, _T("Attempt to write word value %04x to location %8x\n"), data, addr);
	}
}

void __fastcall cps3WriteLong(UINT32 addr, UINT32 data)
{
	addr &= 0xc7ffffff;
	
	switch (addr) {
	case 0x07ff000c:
	case 0x07ff0048:
		// some unknown data write by DMA 0 while bootup
		break;

	default:
		bprintf(PRINT_NORMAL, _T("Attempt to write long value %8x to location %8x\n"), data, addr);
	}
}

void __fastcall cps3C0WriteByte(UINT32 addr, UINT8 data)
{
	bprintf(PRINT_NORMAL, _T("C0 Attempt to write byte value %2x to location %8x\n"), data, addr);
}

void __fastcall cps3C0WriteWord(UINT32 addr, UINT16 data)
{
	bprintf(PRINT_NORMAL, _T("C0 Attempt to write word value %4x to location %8x\n"), data, addr);
}

void __fastcall cps3C0WriteLong(UINT32 addr, UINT32 data)
{
	if (addr < 0xc0000400) {
		*(UINT32 *)(RamC000 + (addr & 0x3ff)) = data;
		*(UINT32 *)(RamC000_D + (addr & 0x3ff)) = data ^ cps3_mask(addr, cps3_key1, cps3_key2);
		return ;
	}
	bprintf(PRINT_NORMAL, _T("C0 Attempt to write long value %8x to location %8x\n"), data, addr);
}

// Program flash. Like MAME, data reads return decrypted data, except on
// sfiii2 which encrypts code and data differently. In read array mode the
// flash is mapped directly (see cps3_map_game_flash()), these handlers are
// only used while a flash command is in progress.

static inline UINT8 *cps3_game_flash_data()
{
	return cps3_isSpecial ? RomGame : RomGame_D;
}

UINT8 __fastcall cps3RomReadByte(UINT32 addr)
{
	UINT32 retvalue = cps3_flash_read(&main_flash, addr & 0x00fffffc);
	if ( main_flash.flash_mode == FM_NORMAL ) {
#ifdef LSB_FIRST
		addr ^= 0x03;
#endif
		return cps3_game_flash_data()[addr & 0x00ffffff];
	}
	return retvalue >> ((~addr & 3) * 8);
}

UINT16 __fastcall cps3RomReadWord(UINT32 addr)
{
	UINT32 retvalue = cps3_flash_read(&main_flash, addr & 0x00fffffc);
	if ( main_flash.flash_mode == FM_NORMAL ) {
#ifdef LSB_FIRST
		addr ^= 0x02;
#endif
		return *(UINT16 *)(cps3_game_flash_data() + (addr & 0x00ffffff));
	}
	return retvalue >> ((~addr & 2) * 8);
}

UINT32 __fastcall cps3RomReadLong(UINT32 addr)
{
	UINT32 retvalue = cps3_flash_read(&main_flash, addr & 0x00ffffff);
	if ( main_flash.flash_mode == FM_NORMAL )
		retvalue = *(UINT32 *)(cps3_game_flash_data() + (addr & 0x00ffffff));
	return retvalue;
}

static void cps3_map_game_flash()
{
	if (main_flash.flash_mode == FM_NORMAL)
		Sh2MapMemory(cps3_game_flash_data(), 0x06000000, 0x06ffffff, MAP_READ);
	else
		Sh2MapHandler(2, 0x06000000, 0x06ffffff, MAP_READ);
}

void __fastcall cps3RomWriteByte(UINT32 addr, UINT8 data)
{
	bprintf(PRINT_NORMAL, _T("Rom Attempt to write byte value %2x to location %8x\n"), data, addr);
}

void __fastcall cps3RomWriteWord(UINT32 addr, UINT16 data)
{
	bprintf(PRINT_NORMAL, _T("Rom Attempt to write word value %4x to location %8x\n"), data, addr);
}

void __fastcall cps3RomWriteLong(UINT32 addr, UINT32 data)
{
//	bprintf(1, _T("Rom Attempt to write long value %8x to location %8x\n"), data, addr);
	addr &= 0x00ffffff;
	INT32 was_normal = main_flash.flash_mode == FM_NORMAL;
	cps3_flash_write(&main_flash, addr, data);
	
	if ( main_flash.flash_mode == FM_NORMAL ) {
		bprintf(1, _T("Rom Attempt to write long value %8x to location %8x\n"), data, addr);
		*(UINT32 *)(RomGame + addr) = data;
		*(UINT32 *)(RomGame_D + addr) = data ^ cps3_mask(addr + 0x06000000, cps3_key1, cps3_key2);
		Sh2InvalidateCode(0x06000000 + addr, 4);
	}

	if (was_normal != (main_flash.flash_mode == FM_NORMAL))
		cps3_map_game_flash();
}

//------------------

UINT8 __fastcall cps3VidReadByte(UINT32 addr)
{
	bprintf(PRINT_NORMAL, _T("Video Attempt to read byte value of location %8x\n"), addr);
//	addr &= 0xc7ffffff;
	return 0;
}

UINT16 __fastcall cps3VidReadWord(UINT32 addr)
{
	bprintf(PRINT_NORMAL, _T("Video Attempt to read word value of location %8x\n"), addr);
//	addr &= 0xc7ffffff;
	return 0;
}

UINT32 __fastcall cps3VidReadLong(UINT32 addr)
{
	bprintf(PRINT_NORMAL, _T("Video Attempt to read long value of location %8x\n"), addr);
//	addr &= 0xc7ffffff;
	return 0;
}

void __fastcall cps3VidWriteByte(UINT32 addr, UINT8 data)
{
	bprintf(PRINT_NORMAL, _T("Video Attempt to write byte value %2x to location %8x\n"), data, addr);
}

void __fastcall cps3VidWriteWord(UINT32 addr, UINT16 data)
{
	addr &= 0xc7ffffff;
	if ((addr >= 0x04080000) && (addr < 0x040c0000)) {
		// Palette
		UINT32 palindex = (addr - 0x04080000) >> 1;
#ifdef LSB_FIRST
		RamPal[palindex ^ 1] = data;
#else
		RamPal[palindex] = data;
#endif

		INT32 r = (data & 0x001F) << 3;	// Red
		INT32 g = (data & 0x03E0) >> 2;	// Green
		INT32 b = (data & 0x7C00) >> 7;	// Blue
		
		r |= r >> 5;
		g |= g >> 5;
		b |= b >> 5;
			
		Cps3CurPal[palindex] = BurnHighCol(r, g, b, 0);
	
	} else
	bprintf(PRINT_NORMAL, _T("Video Attempt to write word value %4x to location %8x\n"), data, addr);
}

void __fastcall cps3VidWriteLong(UINT32 addr, UINT32 data)
{
	addr &= 0xc7ffffff;
	if ((addr >= 0x04080000) && (addr < 0x040c0000)) {

		if ( data != 0 )
			bprintf(PRINT_NORMAL, _T("Video Attempt to write long value %8x to location %8x\n"), data, addr);
		
		
	} else 
	bprintf(PRINT_NORMAL, _T("Video Attempt to write long value %8x to location %8x\n"), data, addr);
}


// Speedup: the SH2 is idle when its task dispatcher looks at the state of this
// task, skip to the end of the current slice of the frame. (Not until the next
// irq: the dispatcher runs with irqs masked, boot would only get on once per
// frame.) Checked right before the dispatcher's mov.x @Rm,Rn that reads it,
// or before the branch whose delay slot it is.
//
// That read starts each lap of the dispatcher over its tasks. Only skip when
// two laps in a row took exactly the same time: then no task ran and no irq
// came, so nothing can change until the next irq. Skipping a lap where a task
// ran would delay the tasks it wakes, and the games' random numbers with them
// (sfiii3 seeds its RNG from the frame counter in a task).
static INT32 cps3_slice_end = 0;
static INT32 cps3_speedup_reg = 0;
static INT32 cps3_lap_start = 0, cps3_lap_cycles = -1;

static void cps3_speedup_check()
{
	if ((Sh2GetRegister(cps3_speedup_reg) & 0xc7ffffff) == cps3_speedup_ram_address) {
		INT32 lap = Sh2TotalCycles() - cps3_lap_start;
		if (lap == cps3_lap_cycles) {
			INT32 left = cps3_slice_end - Sh2TotalCycles();
			if (left > 0)
				Sh2BurnCycles(left);
		}
		cps3_lap_cycles = lap;
		cps3_lap_start = Sh2TotalCycles();
	}
}

// instructions are always fetched decrypted, even on sfiii2
static UINT16 cps3_program_op(UINT32 pc)
{
#ifdef LSB_FIRST
	return *(UINT16 *)(RomGame_D + ((pc & 0x00ffffff) ^ 2));
#else
	return *(UINT16 *)(RomGame_D + (pc & 0x00ffffff));
#endif
}

// Rm of mov.b/w/l @Rm,Rn or @Rm+,Rn, else -1
static INT32 cps3_read_op_reg(UINT16 op)
{
	if ((op & 0xf000) != 0x6000 || (op & 0xf) > 6 || (op & 0xf) == 3) return -1;
	return (op >> 4) & 0x0f;
}

static void cps3_install_speedup()
{
	UINT32 code = cps3_speedup_code_address; // the pc after the read
	if ((code & 0xff000000) != 0x06000000) return;

	UINT32 pc = code - 2;
	INT32 reg = cps3_read_op_reg(cps3_program_op(pc));

	if (reg < 0) {
		// the read is in the delay slot of a bra to that address (redearth):
		// check before the bra, Rm is already set there
		for (UINT32 a = code - 0x1000; a < code + 0x1000; a += 2) {
			UINT16 op = cps3_program_op(a);
			if ((op & 0xf000) != 0xa000) continue;
			INT32 disp = (op & 0x800) ? (INT32)(op & 0xfff) - 0x1000 : (op & 0xfff);
			if (a + 4 + disp * 2 != code) continue;
			reg = cps3_read_op_reg(cps3_program_op(a + 2));
			if (reg >= 0) {
				pc = a;
				break;
			}
		}
	}

	if (reg < 0) {
		bprintf(PRINT_ERROR, _T("CPS3: no read found for speedup address %08x\n"), code);
		return;
	}

	cps3_speedup_reg = reg;
	Sh2SetIdleHook(pc, cps3_speedup_check);
}

// CPS3 Region Patch
static void Cps3PatchRegion()
{
	if ( cps3_region_address ) {

		bprintf(0, _T("Region: %02x -> %02x\n"), RomBios[cps3_region_address], (RomBios[cps3_region_address] & 0xf0) | (cps3_dip & 0x0f));				

		RomBios[cps3_region_address] = (RomBios[cps3_region_address] & 0xf0) | (cps3_dip & 0x7f);

		if ( cps3_ncd_address ) {
			if (cps3_dip & 0x10)
				RomBios[cps3_ncd_address] |= 0x01;
			else
				RomBios[cps3_ncd_address] &= 0xfe;
		}
	}
}

// Screen mode dip: the setting of the game's test menu (sfiii2: Configuration
// -> Game -> Screen Mode) is bit 8 of EEPROM words 0x11 and 0x29. Also set
// after the EEPROM is loaded from the saved NVRAM, which comes after the reset.
static void cps3_apply_screen_mode()
{
	if (cps3_dip & 0x80) {
		EEPROM[0x11] = 0x100 + (EEPROM[0x11] & 0xff);
		EEPROM[0x29] = 0x100 + (EEPROM[0x29] & 0xff);
	} else {
		EEPROM[0x11] = 0x000 + (EEPROM[0x11] & 0xff);
		EEPROM[0x29] = 0x000 + (EEPROM[0x29] & 0xff);
	}
}

static INT32 Cps3Reset()
{
	memset(RamStart, 0, RamEnd - RamStart);

	// re-map cram_bank
	cram_bank = 0;
	bankswitch_cram();

	Cps3PatchRegion();
	
	// [CD-ROM not emulated] All CHD drivers cause a Guru Meditation with the normal bios boot.
	if ((!BurnDrvGetHardwareCode()) & HARDWARE_CAPCOM_CPS3_NO_CD){
		// normal boot
		Sh2Reset();
	} else {
		// fast boot
		if (cps3_isSpecial) {
			Sh2Reset( *(UINT32 *)(RomGame + 0), *(UINT32 *)(RomGame + 4) );
			Sh2SetVBR(0x06000000);
		} else {
			Sh2Reset( *(UINT32 *)(RomGame_D + 0), *(UINT32 *)(RomGame_D + 4) );
			Sh2SetVBR(0x06000000);
		}
	}

	cps3_apply_screen_mode();

	ss_vscroll = 0;
	ss_pal_base = 0;

	cps3_lap_cycles = -1;

	cram_bank = 0;
	cps3_current_eeprom_read = 0;
	gfxflash_bank = 0;

	paldma_source = 0;
	paldma_dest = 0;
	paldma_fade = 0;
	paldma_length = 0;

	chardma_source = 0;
	chardma_table_address = 0;

	dma_irq_cycle = -1;
	spritelist_dma_cycle = -1;
	dma_status = 0;

	cps3_screen_clean_w = cps3_screen_clean_h = 0;

	spritelist_dma = 0;
	spritelist_dma_prev = 0;

	cps3SndReset();

	cps3_reset = 0;

	nExtraCycles = 0;

	clear_opposite.reset();

	HiscoreReset();

	return 0;
}

static void be_to_le(UINT8 * p, INT32 size)
{
	UINT8 c;
	for(INT32 i=0; i<size; i+=4, p+=4) {
		c = *(p+0);	*(p+0) = *(p+3);	*(p+3) = c;
		c = *(p+1);	*(p+1) = *(p+2);	*(p+2) = c;
	}
}

INT32 cps3Init()
{
	INT32 nRet, ii, offset;
	struct BurnRomInfo pri;

	BurnSetRefreshRate(59.59949);

	// calc graphic and sound roms size
	ii = 0; cps3_data_rom_size = 0;
	while (BurnDrvGetRomInfo(&pri, ii) == 0) {
		if (pri.nType & (BRF_GRA | BRF_SND))
			cps3_data_rom_size += pri.nLen;
		ii++;
	}
	
	// CHD games 
	if (cps3_data_rom_size == 0) cps3_data_rom_size = 0x5000000;	
	
	Mem = NULL;
	MemIndex();
	INT32 nLen = MemEnd - (UINT8 *)0;
	if ((Mem = (UINT8 *)BurnMalloc(nLen)) == NULL) return 1;
	memset(Mem, 0, nLen);										// blank all memory
	MemIndex();
	SprList[0] = 0x80000000;
	
	// load and decode bios roms
	ii = 0; offset = 0;
	while (BurnDrvGetRomInfo(&pri, ii) == 0) {
		if (pri.nType & BRF_BIOS) {
			nRet = BurnLoadRom(RomBios + offset, ii, 1); 
			if (nRet != 0) return 1;
			offset += pri.nLen;
		}
		ii++;
	}

#ifdef LSB_FIRST
	be_to_le( RomBios, 0x080000 );
#else
	cps3_region_address ^= 0x03;
#endif
	cps3_decrypt_bios();

	// load and decode sh-2 program roms
	ii = 0;	offset = 0;
	while (BurnDrvGetRomInfo(&pri, ii) == 0) {
		if (pri.nType & BRF_PRG) {
			if (pri.nLen == 0x800000) // sfiii4n
			{
				nRet = BurnLoadRom(RomGame + offset, ii, 1); if (nRet != 0) return 1;
				offset += pri.nLen;
				ii++;
			}
			else
			{
				nRet = BurnLoadRom(RomGame + offset + 0, ii + 0, 4); if (nRet != 0) return 1;
				nRet = BurnLoadRom(RomGame + offset + 1, ii + 1, 4); if (nRet != 0) return 1;
				nRet = BurnLoadRom(RomGame + offset + 2, ii + 2, 4); if (nRet != 0) return 1;
				nRet = BurnLoadRom(RomGame + offset + 3, ii + 3, 4); if (nRet != 0) return 1;
				offset += pri.nLen * 4;
				ii += 4;
			}
		} else {
			ii++;
		}
	}
#ifdef LSB_FIRST
	be_to_le( RomGame, 0x1000000 );
#endif
	cps3_decrypt_game();
	
	// load graphic and sound roms
	ii = 0;	offset = 0;
	while (BurnDrvGetRomInfo(&pri, ii) == 0) {
		if (pri.nType & (BRF_GRA | BRF_SND)) {
			if (pri.nLen == 0x800000) // sfiii4n
			{
				BurnLoadRom(RomUser + offset, ii, 1);
				offset += pri.nLen;
				ii++;
			}
			else
			{
				BurnLoadRom(RomUser + offset + 0, ii + 0, 2);
				BurnLoadRom(RomUser + offset + 1, ii + 1, 2);
				offset += pri.nLen * 2;
				ii += 2;
			}
		} else {
			ii++;
		}
	}

	{
		Sh2Init(1);
		Sh2SetDmaReadCallback(cps3_sh2_dma_read);
		Sh2Open(0);

		// Map sh-2 memory:
		Sh2MapMemory(RomBios,		0x00000000, 0x0007ffff, MAP_ROM);	// BIOS
		Sh2MapMemory(RamMain,		0x02000000, 0x0207ffff, MAP_RAM);	// Main RAM
		Sh2MapMemory((UINT8 *) RamSpr,	0x04000000, 0x0407ffff, MAP_RAM);
//		Sh2MapMemory(RamCRam,		0x04100000, 0x041fffff, MAP_RAM);	// map this while reset
//		Sh2MapMemory(RamGfx,		0x04200000, 0x043fffff, MAP_WRITE);
		Sh2MapMemory((UINT8 *) RamSS,	0x05040000, 0x0504ffff, MAP_RAM);	// 'SS' RAM (Score Screen) (text tilemap + toles)
		
		Sh2SetReadByteHandler (0, cps3ReadByte);
		Sh2SetReadWordHandler (0, cps3ReadWord);
		Sh2SetReadLongHandler (0, cps3ReadLong);
		Sh2SetWriteByteHandler(0, cps3WriteByte);
		Sh2SetWriteWordHandler(0, cps3WriteWord);
		Sh2SetWriteLongHandler(0, cps3WriteLong);

		Sh2MapMemory(RamC000_D,		0xc0000000, 0xc00003ff, MAP_FETCH);	// Executes code from here
		Sh2MapMemory(RamC000,		0xc0000000, 0xc00003ff, MAP_READ);
		Sh2MapHandler(1,		0xc0000000, 0xc00003ff, MAP_WRITE);

		Sh2SetWriteByteHandler(1, cps3C0WriteByte);
		Sh2SetWriteWordHandler(1, cps3C0WriteWord);
		Sh2SetWriteLongHandler(1, cps3C0WriteLong);

		Sh2MapMemory(RomGame_D,		0x06000000, 0x06ffffff, MAP_FETCH);	// Decrypted SH2 Code
		Sh2MapHandler(2,		0x06000000, 0x06ffffff, MAP_WRITE);
		cps3_map_game_flash();

		Sh2SetReadByteHandler (2, cps3RomReadByte);
		Sh2SetReadWordHandler (2, cps3RomReadWord);
		Sh2SetReadLongHandler (2, cps3RomReadLong);
		Sh2SetWriteByteHandler(2, cps3RomWriteByte);
		Sh2SetWriteWordHandler(2, cps3RomWriteWord);
		Sh2SetWriteLongHandler(2, cps3RomWriteLong);

		Sh2MapHandler(3,			0x040e0000, 0x040e02ff, MAP_RAM);
		Sh2SetReadByteHandler (3, cps3SndReadByte);
		Sh2SetReadWordHandler (3, cps3SndReadWord);
		Sh2SetReadLongHandler (3, cps3SndReadLong);
		Sh2SetWriteByteHandler(3, cps3SndWriteByte);
		Sh2SetWriteWordHandler(3, cps3SndWriteWord);
		Sh2SetWriteLongHandler(3, cps3SndWriteLong);
		
		Sh2MapMemory((UINT8 *)RamPal,		0x04080000, 0x040bffff, MAP_READ);	// 16bit BE Colors
		Sh2MapHandler(4,			0x04080000, 0x040bffff, MAP_WRITE);

		Sh2SetReadByteHandler (4, cps3VidReadByte);
		Sh2SetReadWordHandler (4, cps3VidReadWord);
		Sh2SetReadLongHandler (4, cps3VidReadLong);
		Sh2SetWriteByteHandler(4, cps3VidWriteByte);
		Sh2SetWriteWordHandler(4, cps3VidWriteWord);
		Sh2SetWriteLongHandler(4, cps3VidWriteLong);

#ifdef SPEED_HACK
		cps3_install_speedup();
#endif

	}
	
	BurnDrvGetVisibleSize(&cps3_gfx_width, &cps3_gfx_height);	
	RamScreen	+= (512 * 2) * 16 + 16; // safe draw	
	cps3SndInit(RomUser);
	cps3SndSetRoute(BURN_SND_CPS3SND_ROUTE_1, 1.00, BURN_SND_ROUTE_LEFT);
	cps3SndSetRoute(BURN_SND_CPS3SND_ROUTE_2, 1.00, BURN_SND_ROUTE_RIGHT);
	
	pBurnDrvPalette = (UINT32*)Cps3CurPal;
		
	Cps3Reset();
	return 0;
}

INT32 cps3Exit()
{
	bprintf(0, _T("CPS-3 Driver exit....\n"));
	Sh2Exit();
	
	BurnFree(Mem);

	cps3SndExit();	
	
	return 0;
}


// SS layer: draw row 'row' of 8x8 tile 'code' at (x, line), clipped to the screen.
// Only bytes 1 and 3 of each long of SS RAM exist; 4 of them make a tile row.
static void cps3_draw_ss_tile_row(UINT32 code, UINT32 pal, INT32 flipx, INT32 flipy, INT32 x, INT32 line, INT32 row)
{
	if (x <= -8 || x >= cps3_gfx_width) return;

	UINT8 *src = (UINT8 *)RamSS + code * 64 + (flipy ? 7 - row : row) * 8;
#ifdef LSB_FIRST
	UINT32 pixels = src[2] | (src[0] << 8) | (src[6] << 16) | (src[4] << 24);
#else
	UINT32 pixels = src[1] | (src[3] << 8) | (src[5] << 16) | (src[7] << 24);
#endif
	if (pixels == 0) return; // 8 transparent pixels, most of the layer

	UINT16 *dst = (UINT16 *)pBurnDraw + line * cps3_gfx_width + x;
	UINT16 *color = Cps3CurPal + (pal << 4);
	INT32 step = 1;

	if (flipx) {
		dst += 7;
		step = -1;
	}

	if (x >= 0 && x + 8 <= cps3_gfx_width) {
		for (INT32 i = 0; i < 8; i++, dst += step, pixels >>= 4)
			if (pixels & 0xf) *dst = color[pixels & 0xf];
	} else {
		for (INT32 i = 0, px = flipx ? x + 7 : x; i < 8; i++, dst += step, px += step, pixels >>= 4)
			if ((pixels & 0xf) && px >= 0 && px < cps3_gfx_width) *dst = color[pixels & 0xf];
	}
}

// one line of a 16x16 tile to the render buffer
static void cps3_drawgfxzoom_1(UINT32 code, UINT32 pal, INT32 flipx, INT32 flipy, INT32 x, INT32 y, INT32 drawline)
{
	UINT32 * dst = RamScreen + drawline * 1024 + x;
	UINT8 * src = (UINT8 *) RamCRam + code * 256 + 16 * (flipy ? 15 - (drawline - y) : (drawline - y));

	cps3_line16(dst, src, pal, flipx);
}

enum
{
	CPS3_TRANSPARENCY_PEN_INDEX,
	CPS3_TRANSPARENCY_PEN_INDEX_BLEND
};

static void cps3_drawgfxzoom_2(UINT32 code, UINT32 pal, INT32 flipx, INT32 flipy, INT32 sx, INT32 sy, INT32 scalex, INT32 scaley, INT32 alpha, INT32 transparency)
{
	if (!scalex || !scaley) return;

	UINT8 * source_base = (UINT8 *) RamCRam + code * 256;
	
	INT32 sprite_screen_height = (scaley * 16 + 0x8000) >> 16;
	INT32 sprite_screen_width  = (scalex * 16 + 0x8000) >> 16;
	if (sprite_screen_width && sprite_screen_height) {
		// compute sprite increment per screen pixel
		INT32 dx = (16 << 16) / sprite_screen_width;
		INT32 dy = (16 << 16) / sprite_screen_height;

		INT32 ex = sx + sprite_screen_width;
		INT32 ey = sy + sprite_screen_height;

		INT32 x_index_base;
		INT32 y_index;

		if( flipx )	{
			x_index_base = (sprite_screen_width - 1) * dx;
			dx = -dx;
		} else
			x_index_base = 0;

		if( flipy )	{
			y_index = (sprite_screen_height - 1) * dy;
			dy = -dy;
		} else 
			y_index = 0;

		{
			if( sx < 0)
			{ /* clip left */
				INT32 pixels = 0-sx;
				sx += pixels;
				x_index_base += pixels*dx;
			}
			if( sy < 0 )
			{ /* clip top */
				INT32 pixels = 0-sy;
				sy += pixels;
				y_index += pixels*dy;
			}
			if( ex > cps3_gfx_max_x+1 )
			{ /* clip right */
				INT32 pixels = ex-cps3_gfx_max_x-1;
				ex -= pixels;
			}
			if( ey > cps3_gfx_max_y+1 )
			{ /* clip bottom */
				INT32 pixels = ey-cps3_gfx_max_y-1;
				ey -= pixels;
			}
		}

		if( ex > sx ) {
			switch( transparency ) {
			case CPS3_TRANSPARENCY_PEN_INDEX:
				if (dx == (flipx ? -0x10000 : 0x10000) && ex - sx == 16) {
					// not zoomed horizontally and fully visible: whole lines at once
					for( INT32 y=sy; y<ey; y++ ) {
						cps3_line16(RamScreen + y * 512 * 2 + sx, source_base + (y_index>>16) * 16, pal<<alpha, flipx);
						y_index += dy;
					}
					break;
				}
				for( INT32 y=sy; y<ey; y++ ) {
					UINT8 * source = source_base + (y_index>>16) * 16;
					UINT32 * dest = RamScreen + y * 512 * 2;
					INT32 x_index = x_index_base;
					for(INT32 x=sx; x<ex; x++ ) {
#if BE_GFX_CRAM
						UINT8 c = source[ (x_index>>16) ];
#else
						UINT8 c = source[ (x_index>>16) ^ 3 ];
#endif
						if( c )	dest[x] = pal<<alpha | c;
						x_index += dx;
					}
					y_index += dy;
				}
				break;
			case CPS3_TRANSPARENCY_PEN_INDEX_BLEND:
				for( INT32 y=sy; y<ey; y++ ) {
					UINT8 * source = source_base + (y_index>>16) * 16;
					UINT32 * dest = RamScreen + y * 512 * 2;
					INT32 x_index = x_index_base;
					for(INT32 x=sx; x<ex; x++ ) {
#if BE_GFX_CRAM
						UINT8 c = source[ (x_index>>16)];
#else
						UINT8 c = source[ (x_index>>16) ^ 3 ];
#endif
						if (c) {
							if (alpha == 6)
								dest[x] |= (c & 0xf) << 13;
							else
								dest[x] |= ((c & 1) << 15) | ((pal & 0x1) << 16);
						}
						x_index += dx;
					}
					y_index += dy;
				}
				break;
			}
		}
	}
}

// one line of a 16x16 tile, alpha blended (shadows, highlights) as in MAME
static void cps3_draw_tile_line_blend(UINT32 code, UINT32 pal, INT32 flipx, INT32 flipy, INT32 x, INT32 y, INT32 drawline, INT32 bpp6)
{
	UINT32 *dst = RamScreen + drawline * 1024;
	UINT8 *src = (UINT8 *)RamCRam + code * 256 + 16 * (flipy ? 15 - (drawline - y) : (drawline - y));

	for (INT32 i = 0; i < 16; i++) {
		INT32 px = x + (flipx ? 15 - i : i);
#if BE_GFX_CRAM
		UINT8 c = src[i];
#else
		UINT8 c = src[i ^ 3];
#endif
		if (c && px >= 0 && px <= cps3_gfx_max_x) {
			if (bpp6)
				dst[px] |= (c & 0xf) << 13;
			else
				dst[px] |= ((c & 1) << 15) | ((pal & 1) << 16);
		}
	}
}

static void cps3_draw_tilemapsprite_line(INT32 drawline, UINT32 * regs )
{
	if (!(regs[1]&0x00008000)) return;

	INT32 scrolly =  ((regs[0]&0x0000ffff)>>0)+4;
	INT32 line = (drawline + scrolly) & 0x3ff;

	UINT32 mapbase =  (regs[2]&0x007f0000)>>16;
	UINT32 linebase=  (regs[2]&0x7f000000)>>24;
	INT32 linescroll_enable = (regs[1]&0x00004000);
	INT32 global_flip_x = (regs[1] & 0x00000800); // warzard special moves
	INT32 global_flip_y = (regs[1] & 0x00000400); // sfiii2 loading screens

	if (global_flip_y) line ^= 0x3ff;
	INT32 xflip_mask = global_flip_x ? 0x3f : 0;

	INT32 scrollx;
	INT32 tileline = (line/16)+1;
	INT32 tilesubline = line % 16;

	mapbase = mapbase << 10;
	linebase = linebase << 10;

	scrollx =  (regs[0]&0xffff0000)>>16;
	if (linescroll_enable)
		scrollx+= (RamSpr[linebase+((line+16)&0x3ff)]>>16)&0x3ff; // test case: sfiii Ryu's stage 2nd round floor

	for (INT32 x=0;x<(cps3_gfx_max_x/16)+2;x++) {
		UINT32 dat = RamSpr[mapbase+((tileline&63)*64)+(((x+scrollx/16)&63) ^ xflip_mask)];
		INT32 tileno = (dat & 0xfffe0000)>>17;
		INT32 colour = (dat & 0x000001ff)>>0;
		INT32 bpp =    (dat & 0x00000200)>>9;
		INT32 alpha =  (dat & 0x00000400)>>10; // jojo's "country town" and "in air plane" stages
		INT32 yflip  = (dat & 0x00000800)>>11;
		INT32 xflip  = ((dat & 0x00001000)>>12) ^ (xflip_mask & 1);

		if (alpha) {
			cps3_draw_tile_line_blend(tileno,colour,xflip,yflip,(x*16)-scrollx%16,drawline-tilesubline, drawline, bpp);
			continue;
		}

		if (!bpp) colour <<= 8;
		else colour <<= 6;

		cps3_drawgfxzoom_1(tileno,colour,xflip,yflip,(x*16)-scrollx%16,drawline-tilesubline, drawline);
	}
}

#ifdef CPS3_PROFILE
// Average time spent per frame in the SH2, sound and video, logged every 600
// frames. Build with CPS3_PROFILE=1.
#ifdef VITA
#include <psp2/kernel/processmgr.h>
static inline UINT64 cps3_profile_us() { return sceKernelGetProcessTimeWide(); }
#else
#include <time.h>
static inline UINT64 cps3_profile_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (UINT64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

// sh2, sound, video: layers, final copy, SS layer
// Also the real time per frame (frontend included) and the slowest frame in
// the core.
static UINT64 cps3_profile_time[6], cps3_profile_frames;
static UINT64 cps3_profile_period_start, cps3_profile_frame_sum, cps3_profile_worst;

static void cps3_profile_add(INT32 what, UINT64 start)
{
	cps3_profile_time[what] += cps3_profile_us() - start;
}

static void cps3_profile_frame()
{
	UINT64 *t = cps3_profile_time;
	UINT64 sum = t[0] + t[1] + t[3] + t[4] + t[5];
	if (sum - cps3_profile_frame_sum > cps3_profile_worst) cps3_profile_worst = sum - cps3_profile_frame_sum;
	cps3_profile_frame_sum = sum;

	if (++cps3_profile_frames < 600) return;
	UINT64 now = cps3_profile_us();
	if (cps3_profile_period_start)
		bprintf(PRINT_NORMAL, _T("CPS3 profile, us per frame: sh2 %d, sound %d, video %d (layers %d, copy %d, text %d), worst %d, real %d\n"),
			(INT32)(t[0] / 600), (INT32)(t[1] / 600), (INT32)((t[3] + t[4] + t[5]) / 600),
			(INT32)(t[3] / 600), (INT32)(t[4] / 600), (INT32)(t[5] / 600),
			(INT32)cps3_profile_worst, (INT32)((now - cps3_profile_period_start) / 600));
	cps3_profile_period_start = now;
	memset(cps3_profile_time, 0, sizeof(cps3_profile_time));
	cps3_profile_frames = cps3_profile_frame_sum = cps3_profile_worst = 0;
}
#define CPS3_PROFILE_START()		UINT64 profile_start = cps3_profile_us()
#define CPS3_PROFILE_END(what)		cps3_profile_add(what, profile_start); profile_start = cps3_profile_us()
#define CPS3_PROFILE_FRAME()		cps3_profile_frame()
#else
#define CPS3_PROFILE_START()
#define CPS3_PROFILE_END(what)
#define CPS3_PROFILE_FRAME()
#endif

INT32 DrvCps3Draw()
{
	CPS3_PROFILE_START();
	UINT32 *crtc = RamVReg + 0x60 / 4;

	BurnDrvGetVisibleSize(&cps3_gfx_width, &cps3_gfx_height);

	// screen width as set in the CRTC (384, or 496 for the wide modes), as in MAME
	INT32 width = ((crtc[1] & 0xffff0000) >> 16) - (crtc[0] & 0xffff);
	if (strcmp(BurnDrvGetTextA(DRV_NAME), "sfiii3ws") == 0) width = 496; // hack set
	if (width <= 0 || width > 512) width = cps3_gfx_width;

	if (cps3_gfx_width != width) {
		BurnDrvSetVisibleSize(width, 224);
		if (width > 384) BurnDrvSetAspect(16, 9); // 495 in the games' wide mode
		else BurnDrvSetAspect(4, 3);
		ReinitialiseVideo();
		return 1;
	}

	// full screen zoom, 0x40 = 1:1
	UINT32 fullscreenzoomx = crtc[3] & 0xff;
	UINT32 fullscreenzoomy = crtc[7] & 0xff;
	if (fullscreenzoomx > 0x80) fullscreenzoomx = 0x80;
	if (fullscreenzoomy > 0x80) fullscreenzoomy = 0x80;
	UINT32 fszx = (fullscreenzoomx << 16) / 0x40;
	UINT32 fszy = (fullscreenzoomy << 16) / 0x40;

	cps3_gfx_max_x = ((cps3_gfx_width  * fszx + 0x8000) >> 16) - 1;
	cps3_gfx_max_y = ((cps3_gfx_height * fszy + 0x8000) >> 16) - 1;

	if (nBurnLayer & 1)
	{
		// the last frame's composite may have cleared it already
		if (cps3_gfx_max_x + 1 > cps3_screen_clean_w || cps3_gfx_max_y + 1 > cps3_screen_clean_h) {
			UINT32 * pscr = RamScreen;
			INT32 clrsz = (cps3_gfx_max_x + 1) * sizeof(INT32);
			for(INT32 yy = 0; yy<=cps3_gfx_max_y; yy++, pscr += 512*2)
				memset(pscr, 0, clrsz);
		}
	}
	else
	{
		Cps3CurPal[0x20000] = BurnHighCol(0xff, 0x00, 0xff, 0);

		INT32 i;
		for (i = 0; i < 1024 * 448; i++) {
			RamScreen[i] = 0x20000;
		}
	}

	cps3_screen_clean_w = cps3_screen_clean_h = 0;

	UINT32 *SprSrc = (cps3_fake_dip & 1 ? RamSpr : SprList);

	// Draw Sprites
	{
		for (INT32 i=0x00000/4;i<0x2000/4;i+=4) {

			if (SprSrc[i+0]&0x80000000) break;

			INT32 gscroll		= (SprSrc[i+0]&0x70000000)>>28;
			INT32 length		= (SprSrc[i+0]&0x01ff0000)>>14; // how many entries in the sprite table
			UINT32 start		= (SprSrc[i+0]&0x00007ff0)>>4;

			INT32 xpos			= (SprSrc[i+1]&0x03ff0000)>>16;
			INT32 ypos			= (SprSrc[i+1]&0x000003ff)>>0;

			INT32 whichbpp		= (SprSrc[i+2]&0x40000000)>>30; // not 100% sure if this is right, jojo title / characters
			INT32 whichpal		= (SprSrc[i+2]&0x20000000)>>29;
			INT32 global_xflip	= (SprSrc[i+2]&0x10000000)>>28;
			INT32 global_yflip	= (SprSrc[i+2]&0x08000000)>>27;
			INT32 global_alpha	= (SprSrc[i+2]&0x04000000)>>26; // alpha / shadow? set on sfiii2 shadows, and big black image in jojo intro
			INT32 global_bpp	= (SprSrc[i+2]&0x02000000)>>25;
			INT32 global_pal	= (SprSrc[i+2]&0x01ff0000)>>16;

			INT32 gscrollx		= (RamVRegBuf[gscroll]&0x03ff0000)>>16;
			INT32 gscrolly		= (RamVRegBuf[gscroll]&0x000003ff)>>0;
			
			start = (start * 0x100) >> 2;
		
			for (INT32 j=0; j<length; j+=4) {
				
				UINT32 value1 = (SprSrc[start+j+0]);
				UINT32 value2 = (SprSrc[start+j+1]);
				UINT32 value3 = (SprSrc[start+j+2]);

				INT32 tilestable[4] = { 8,1,2,4 };

				UINT32 tileno = (value1&0xfffe0000)>>17;
				INT32 flipx = (value1 & 0x00001000)>>12;
				INT32 flipy = (value1 & 0x00000800)>>11;
				INT32 alpha = (value1 & 0x00000400)>>10; //? this one is used for alpha effects on warzard
				INT32 bpp =   (value1 & 0x00000200)>>9;
				INT32 pal =   (value1 & 0x000001ff);

				INT32 count;
				INT32 xpos2 = (value2 & 0x03ff0000)>>16;
				INT32 ypos2 = (value2 & 0x000003ff)>>0;

				INT32 ysizedraw2 = ((value3 & 0x7f000000)>>24) + 1;
				INT32 xsizedraw2 = ((value3 & 0x007f0000)>>16) + 1;
				INT32 xx,yy;

				INT32 ysize2 = ((value3 & 0x0000000c)>>2);
				INT32 xsize2 = ((value3 & 0x00000003)>>0);
				UINT32 xinc,yinc;

				if (ysize2==0) continue;

				if (xsize2==0)
				{
					if (nBurnLayer & 1)
					{
						INT32 tilemapnum = ((value3 & 0x00000030)>>4);
						UINT32 * regs = RamVReg + 8 + tilemapnum * 4;

						for (yy = 0; yy < ysizedraw2; yy++)
						{
							INT32 cury_pos = ypos2 + gscrolly - yy;
							cury_pos = ~cury_pos;
							cury_pos -= 18;
							cury_pos &= 0x3ff;

							if (cury_pos >= 0 && cury_pos <= cps3_gfx_max_y)
								cps3_draw_tilemapsprite_line(cury_pos, regs);
						}
					}
				} else {
					if (~nSpriteEnable & 1) continue;

					ysize2 = tilestable[ysize2];
					xsize2 = tilestable[xsize2];

					xinc = ((xsizedraw2)<<16) / ((xsize2));
					yinc = ((ysizedraw2)<<16) / ((ysize2));

					UINT32 xscale = xinc / 16;
					UINT32 yscale = yinc / 16;

					/* Let's approximate to the nearest greater integer value
					 to avoid holes in between tiles */
					if (xscale & 0xffff)    xscale += (1<<16) / 16;
					if (yscale & 0xffff)    yscale += (1<<16) / 16;

					xsize2-=1;
					ysize2-=1;

					flipx ^= global_xflip;
					flipy ^= global_yflip;

					if (!flipx) xpos2 += (xsizedraw2 / 2);
					else xpos2 -= (xsizedraw2 / 2);

					ypos2 += (ysizedraw2 / 2);

					if (!flipx) xpos2 -= ((xsize2 + 1) * xinc) >> 16;
					else  xpos2 += (xsize2 * xinc) >> 16;

					if (flipy) ypos2 -= (ysize2 * yinc) >> 16;

					/* use the palette value from the main list or the sublists? */
					INT32 actualpal = whichpal ? global_pal : pal;

					/* use the bpp value from the main list or the sublists? */
					INT32 color_granularity = ((whichbpp ? global_bpp : bpp) ? 6 : 8);

					INT32 trans = (global_alpha || alpha) ? CPS3_TRANSPARENCY_PEN_INDEX_BLEND : CPS3_TRANSPARENCY_PEN_INDEX;

					{
						count = 0;
						for (xx=0;xx<xsize2+1;xx++) {
							INT32 current_xpos;

							if (!flipx) current_xpos = (xpos + xpos2 + ((xx * xinc) >> 16));
							else current_xpos = (xpos + xpos2 - ((xx * xinc) >> 16));

							current_xpos += gscrollx;
							current_xpos += 1;
							current_xpos &=0x3ff;
							if (current_xpos&0x200) current_xpos-=0x400;

							for (yy=0;yy<ysize2+1;yy++) {
								INT32 current_ypos;

								if (flipy) current_ypos = (ypos + ypos2 + ((yy * yinc) >> 16));
								else current_ypos = (ypos + ypos2 - ((yy * yinc) >> 16));

								current_ypos += gscrolly;
								current_ypos = 0x3ff-current_ypos;
								current_ypos -= 17;
								current_ypos &=0x3ff;

								if (current_ypos&0x200) current_ypos-=0x400;

								{
									cps3_drawgfxzoom_2(tileno+count,actualpal,flipx,flipy,current_xpos,current_ypos,xscale,yscale, color_granularity, trans);
									count++;
								}
							}
						}
					}
				}
			}
		}
	}
	
	CPS3_PROFILE_END(3);

	if (fszx == 0x10000 && fszy == 0x10000 && (nBurnLayer & 1))
	{
		// no zoom: also clear the buffer for the next frame on the way
		UINT16 * dstbitmap = (UINT16 * )pBurnDraw;

		for (INT32 rendery = 0; rendery < cps3_gfx_height; rendery++, dstbitmap += cps3_gfx_width)
			cps3_copy_line(dstbitmap, RamScreen + rendery * 1024, cps3_gfx_width, Cps3CurPal);
		cps3_screen_clean_w = cps3_gfx_width;
		cps3_screen_clean_h = cps3_gfx_height;
	}
	else
	{
		UINT32 srcx, srcy = 0;
		UINT32 * srcbitmap;
		UINT16 * dstbitmap = (UINT16 * )pBurnDraw;

		for (INT32 rendery=0; rendery<224; rendery++) {
			srcbitmap = RamScreen + (srcy >> 16) * 1024;
			srcx=0;
			for (INT32 renderx=0; renderx<cps3_gfx_width; renderx++, dstbitmap ++) {
				*dstbitmap = Cps3CurPal[ srcbitmap[srcx>>16] ];
				srcx += fszx;
			}
			srcy += fszy;
		}
	}
	
	CPS3_PROFILE_END(4);

	if (nBurnLayer & 2)
	{
		// SS layer, as in MAME. The vertical scroll only selects the page
		// (sfiii2 intro), rowscroll is used by the 'combo meter' in JoJo games.
		INT32 scrolly = (-ss_vscroll) & 0x100;

		INT32 row_offset = -1;
		UINT64 row_used = 0;

		for (INT32 line = 0; line < cps3_gfx_height; line++) {
			INT32 offset = (((line + scrolly) / 8) * 64) & 0xfff;
			INT32 rowscroll = (RamSS[0x4000 / 4 + ((line + scrolly - 1) & 0x1ff)] >> 16) & 0xff;

			if (offset != row_offset) {
				// new row of tiles: find the ones with something to draw
				row_offset = offset;
				row_used = 0;
				for (INT32 x = 0; x < 64; x++) {
					UINT32 data = RamSS[offset + x];
					UINT32 tile = ((data >> 16) & 0xff) | ((data & 1) << 8);
					if (tile == 0) continue;

					const UINT32 *gfx = RamSS + (tile + 0x200) * 16;
					UINT32 pixels = 0;
					for (INT32 i = 0; i < 16; i++)
						pixels |= gfx[i];
					if (pixels & 0x00ff00ff) // only bytes 1 and 3 exist
						row_used |= (UINT64)1 << x;
				}
			}
			if (row_used == 0) continue;

			// the layer is 512 pixels wide and wraps, only visit the visible columns
			for (INT32 col = 0; col <= cps3_gfx_width / 8; col++) {
				INT32 x = (col + rowscroll / 8) & 63;
				if (!((row_used >> x) & 1)) continue;

				UINT32 data = RamSS[offset + x];
				data = ((data >> 16) & 0xff) | ((data & 0xff) << 8);
				UINT32 tile = data & 0x1ff;
				if (tile == 0) continue; // empty

				INT32 pal = ((data & 0x3e00) >> 9) + (ss_pal_base << 5);
				INT32 flipy = data & 0x4000;
				INT32 flipx = data & 0x8000;
				INT32 sx = (col * 8) - (rowscroll & 7);

				cps3_draw_ss_tile_row(tile + 0x200, pal, flipx, flipy, sx, line, line & 7);
			}
		}
	}
	CPS3_PROFILE_END(5);

	return 0;
}


// run the SH2 to the given cycle of the frame in slices of nSlice cycles,
// raising the DMA irq on time
static void cps3_run_to(INT32 nCycles, INT32 nSlice)
{
	while (Sh2TotalCycles() < nCycles) {
		INT32 end = (Sh2TotalCycles() / nSlice + 1) * nSlice;
		if (end > nCycles)
			end = nCycles;
		if (dma_irq_cycle != -1 && dma_irq_cycle < end)
			end = dma_irq_cycle;
		cps3_slice_end = end;

		if (end > Sh2TotalCycles())
			Sh2Run(end - Sh2TotalCycles());

		if (dma_irq_cycle != -1 && Sh2TotalCycles() >= dma_irq_cycle) {
			dma_irq_cycle = -1;
			dma_status &= ~6;
			Sh2SetIRQLine(10, CPU_IRQSTATUS_ACK);
		}
	}
}

INT32 cps3Frame()
{
	// feels a bit hacky, and sfiii2 can change mode without resetting through its service menu,
	// so maybe there is a better way to do this ?
	// sfiii3 got that mode too, but it's heavily glitched so let's keep it hidden -barbudreadmon
	if ( cps3_region_address ) {
		if ((cps3_dip&0x80) == 0x80 && (RomBios[cps3_region_address]&0x80) != 0x80) {
			cps3_reset = 1;
			RomBios[cps3_region_address] |= 0x80;
		}
		else if ((cps3_dip&0x80) != 0x80 && (RomBios[cps3_region_address]&0x80) == 0x80) {
			cps3_reset = 1;
			RomBios[cps3_region_address] &= ~0x80;
		}
	}

	if (cps3_reset)
		Cps3Reset();
		
	if (cps3_palette_change) {
		for(INT32 i=0;i<0x0020000;i++) {
#ifdef LSB_FIRST
			INT32 data = RamPal[i ^ 1];
#else
			INT32 data = RamPal[i];
#endif
			INT32 r = (data & 0x001F) << 3;	// Red
			INT32 g = (data & 0x03E0) >> 2;	// Green
			INT32 b = (data & 0x7C00) >> 7;	// Blue
			r |= r >> 5;
			g |= g >> 5;
			b |= b >> 5;
			Cps3CurPal[i] = BurnHighCol(r, g, b, 0);
		}
		cps3_palette_change = 0;
	}

//	EEPROM[0x11] = 0x100 + (EEPROM[0x11] & 0xff);
//	EEPROM[0x29] = 0x100 + (EEPROM[0x29] & 0xff);

	Cps3Input[0] = 0;
	Cps3Input[1] = 0;
	//Cps3Input[2] = 0;
	Cps3Input[3] = 0;
	for (INT32 i=0; i<16; i++) {
		Cps3Input[0] |= (Cps3But1[i] & 1) << i;
		Cps3Input[1] |= (Cps3But2[i] & 1) << i;
		Cps3Input[3] |= (Cps3But3[i] & 1) << i;
	}

	// Hack to press all three attack buttons with one button
	if (strncmp(BurnDrvGetTextA(DRV_NAME), "jojo", 4) == 0) {
		if (Cps3Input[3] & (1 << 2)) { // p1 'all attacks' button
			Cps3Input[3] &= ~(1 << 2); // clear 'all attacks' button
			Cps3Input[0] |= (1 << 4) | (1 << 5) | (1 << 6); // press Weak, Medium, and Strong attack buttons
		}
		
		if (Cps3Input[3] & (1 << 5)) { // p2 'all attacks' button
			Cps3Input[3] &= ~(1 << 5); // clear 'all attacks' button
			Cps3Input[0] |= (1 << 12) | (1 << 13) | (1 << 14); // press Weak, Medium, and Strong attack buttons
		}
	}

	// Clear Opposites
	clear_opposite.check(0, Cps3Input[0], 0x0001, 0x0002, 0x0004, 0x0008, nSocd[0]);
	clear_opposite.check(1, Cps3Input[0], 0x0100, 0x0200, 0x0400, 0x0800, nSocd[1]);

	Sh2NewFrame();

	INT32 nCyclesTotal = (INT32)((double)25000000 * 100 / nBurnFPS);

	Sh2Idle(nExtraCycles);

	// The only irqs are vblank (12) and DMA end (10), as in MAME. The frame
	// starts at vblank: the irq is raised at its end, right after drawing.
	CPS3_PROFILE_START();

	cps3_run_to(nCyclesTotal, nCyclesTotal / 4);
	Sh2SetIRQLine(12, CPU_IRQSTATUS_ACK);
	CPS3_PROFILE_END(0);

	nExtraCycles = Sh2TotalCycles() - nCyclesTotal;

	// pending events carry over to the next frame
	if (dma_irq_cycle != -1) dma_irq_cycle = (dma_irq_cycle > nCyclesTotal) ? dma_irq_cycle - nCyclesTotal : 0;
	if (spritelist_dma_cycle != -1) spritelist_dma_cycle = (spritelist_dma_cycle > nCyclesTotal) ? spritelist_dma_cycle - nCyclesTotal : 0;
	cps3_lap_start -= nCyclesTotal;

	cps3SndUpdate();
	CPS3_PROFILE_END(1);

	if (pBurnDraw) DrvCps3Draw(); // profiled inside
	CPS3_PROFILE_FRAME();

	return 0;
}

INT32 cps3Scan(INT32 nAction, INT32 *pnMin)
{
	if (pnMin) *pnMin =  0x029672;

	struct BurnArea ba;
	
	if (nAction & ACB_NVRAM) {
		// Save EEPROM configuration
		ba.Data		= EEPROM;
		ba.nLen		= 0x0000100 * sizeof(UINT16);
		ba.nAddress = 0;
		ba.szName	= "EEPROM RAM";
		BurnAcb(&ba);

		if (nAction & ACB_WRITE)
			cps3_apply_screen_mode();
	}
	
	if (nAction & ACB_MEMORY_RAM) {
		
		ba.Data		= RamMain;
		ba.nLen		= 0x0080000;
		ba.nAddress = 0;
		ba.szName	= "Main RAM";
		BurnAcb(&ba);

		ba.Data		= RamSpr;
		ba.nLen		= (0x0080000/4) * sizeof(UINT32);
		ba.nAddress = 0;
		ba.szName	= "Sprite RAM";
		BurnAcb(&ba);

		ba.Data		= SprList;
		ba.nLen		= (0x0080000/4) * sizeof(UINT32);
		ba.nAddress = 0;
		ba.szName	= "Sprite List";
		BurnAcb(&ba);

		ba.Data		= RamSS;
		ba.nLen		= 0x0004000 * sizeof(UINT32);
		ba.nAddress = 0;
		ba.szName	= "Char ROM";
		BurnAcb(&ba);
		
		ba.Data		= RamVReg;
		ba.nLen		= 0x0000040 * sizeof(UINT32);
		ba.nAddress = 0;
		ba.szName	= "Video REG";
		BurnAcb(&ba);

		ba.Data		= RamVRegBuf;
		ba.nLen		= 0x0000040 * sizeof(UINT32);
		ba.nAddress = 0;
		ba.szName	= "Video REG_BUF";
		BurnAcb(&ba);
		
		ba.Data		= RamC000;
		ba.nLen		= 0x0000400 * 2; // includes _D (!)
		ba.nAddress = 0;
		ba.szName	= "RAM C000";
		BurnAcb(&ba);
		
		ba.Data		= RamPal;
		ba.nLen		= 0x0020000 * sizeof(UINT16);
		ba.nAddress = 0;
		ba.szName	= "Palette";
		BurnAcb(&ba);

		if (~nAction & ACB_NET_OPT && ~nAction & ACB_RUNAHEAD) {
			ba.Data		= RamCRam;
			ba.nLen		= 0x0200000 * sizeof(UINT32);
			ba.nAddress = 0;
			ba.szName	= "Sprite ROM";
			BurnAcb(&ba);
		}

/*		// so huge. need not backup it while NOCD
		// otherwize, need backup gfx also
		ba.Data		= RomGame;
		ba.nLen		= 0x1000000;
		ba.nAddress = 0;
		ba.szName	= "Game ROM";
		BurnAcb(&ba);
*/
	}
	
	if (nAction & ACB_DRIVER_DATA) {
		
		Sh2Scan(nAction);
		cps3SndScan(nAction);

		SCAN_VAR(ss_vscroll);
		SCAN_VAR(ss_pal_base);
		SCAN_VAR(cram_bank);
		SCAN_VAR(cps3_current_eeprom_read);
		SCAN_VAR(gfxflash_bank);

		SCAN_VAR(paldma_source);
		SCAN_VAR(paldma_dest);
		SCAN_VAR(paldma_fade);
		SCAN_VAR(paldma_length);

		SCAN_VAR(chardma_source);
		SCAN_VAR(chardma_table_address);

		SCAN_VAR(spritelist_dma);
		SCAN_VAR(spritelist_dma_prev);

		SCAN_VAR(dma_status);
		SCAN_VAR(dma_irq_cycle);
		SCAN_VAR(spritelist_dma_cycle);

		SCAN_VAR(main_flash);
		
		SCAN_VAR(last_normal_byte);
		SCAN_VAR(lastb);
		SCAN_VAR(lastb2);
		

		SCAN_VAR(nExtraCycles);

		clear_opposite.scan();

		if (nAction & ACB_WRITE) {
			
			// rebuild current palette
			cps3_palette_change = 1;
			
			// remap RamCRam
			bankswitch_cram();

			cps3_map_game_flash();

			cps3_lap_cycles = -1;
		}
	}
	
	return 0;
}
