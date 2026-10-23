// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
/***************************************************************************

    tms57002.h

    Texas Instruments TMS57002 "DASP" audio DSP emulator (ported from MAME).

    Only the DSP engine: host interface (PLOAD/CLOAD program and coefficient
    upload, parameter updates), the instruction cache and the per-sample
    execution model. The caller feeds 4 serial inputs, runs one program pass
    per sample and reads the 4 serial outputs. The external data memory
    (delay lines, 256 KB) is owned by the emulator.

***************************************************************************/

#ifndef __TMS57002_H__
#define __TMS57002_H__

#include "../../stdtype.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tms57002_state TMS57002;

TMS57002* tms57002_create(void);
void      tms57002_destroy(TMS57002* dsp);
void      tms57002_reset(TMS57002* dsp);			/* equivalent of MAME device_reset() */

/* host interface */
void      tms57002_pload_w(TMS57002* dsp, int state);	/* PLOAD pin level (0 = program load active) */
void      tms57002_cload_w(TMS57002* dsp, int state);	/* CLOAD pin level (0 = coefficient load active) */
void      tms57002_data_w(TMS57002* dsp, UINT8 data);	/* host data write */
UINT8     tms57002_data_r(TMS57002* dsp);				/* host data read */

/* serial audio interface: 24-bit samples (sign-extended in INT32) */
void      tms57002_set_input(TMS57002* dsp, UINT8 channel, INT32 sample);
INT32     tms57002_get_output(const TMS57002* dsp, UINT8 channel);
UINT8     tms57002_sim(const TMS57002* dsp);			/* ST0 SIM bit: 1 = inputs are 16-bit (scaled x256) */

/* one sample period: sync the program counter (MAME sync_w) and run up to 'cycles' instruction cycles */
void      tms57002_sync(TMS57002* dsp);
void      tms57002_run(TMS57002* dsp, int cycles);
/* run 'periods' sample periods with silent inputs (output discarded): VGM command 0xE4 sub 8 */
void      tms57002_prerun(TMS57002* dsp, UINT32 periods, int cycles);

/* complete state image (VGM data block 0x98 of the ZSG-2 device): little-endian, version 1:
   u32 version; u32 st0, st1, sti, aacc, xoa, xba, xwr, xrd, txrd, creg; s64 macc, macc_read, macc_write;
   u8 pc, hpc, ca, id, ba0, ba1, rptc, rptc_next, sa, hidx, allow_update, update_head, update_tail, pad[3];
   u8 host[4]; u32 xm_adr; u32 update[16]; u8 update_addr[16]; u32 si[4], so[4];
   u32 pgm[256]; u32 cmem[256]; u32 dmem0[256]; u32 dmem1[32];
   u32 nspans; then nspans x { u32 offset; u32 length; u8 data[length] } of the external data memory
   (the bytes not covered are zero). */
UINT32    tms57002_state_save(const TMS57002* dsp, UINT8* buf, UINT32 bufSize);	/* returns the size needed; writes when buf != NULL and bufSize is enough */
UINT8     tms57002_state_load(TMS57002* dsp, const UINT8* buf, UINT32 size);		/* 0 = ok */

#ifdef __cplusplus
}
#endif

#endif	// __TMS57002_H__
