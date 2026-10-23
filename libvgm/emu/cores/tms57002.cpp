// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
/***************************************************************************

    tms57002.cpp

    Texas Instruments TMS57002 "DASP" audio DSP emulator.
    Port of MAME's src/devices/cpu/tms57002 (tms57002.cpp, tms57kdec.cpp,
    tmsops.cpp) to a self-contained class with a C interface. The opcode
    implementations are generated from tmsinstr.lst (tms57002_ops.hxx).

    Differences from MAME: the program memory is an internal 256-word array,
    the external data memory is a 256 KB byte array (the Taito Zoom board RAM,
    unmapped beyond), the DREADY/PC0/EMPTY output pins are not reported (the
    host CPU is not emulated).

***************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include "../../stdtype.h"
#include "tms57002.h"

typedef UINT8 u8;
typedef UINT16 u16;
typedef UINT32 u32;
typedef UINT64 u64;
typedef INT8 s8;
typedef INT16 s16;
typedef INT32 s32;
typedef INT64 s64;
typedef UINT8 uint8_t_;

// the generated code uses the <stdint.h> names
#include <stdint.h>

class tms57002
{
public:
	tms57002();
	~tms57002();

	void reset();

	void pload_w(int state);
	void cload_w(int state);
	void data_w(u8 data);
	u8 data_r();
	void sync_w(int state);

	void run(int cycles);
	u32 state_save(u8* buf, u32 bufSize) const;
	bool state_load(const u8* buf, u32 size);

	u32 si[4], so[4];
	u32 st0;

private:
	enum {
		IN_PLOAD = 0x00000001,
		IN_CLOAD = 0x00000002,
		SU_CVAL  = 0x00000004,
		SU_MASK  = 0x00000018, SU_ST0 = 0x00, SU_ST1 = 0x08, SU_PRG = 0x10,
		S_IDLE   = 0x00000020,
		S_READ   = 0x00000040,
		S_WRITE  = 0x00000080,
		S_BRANCH = 0x00000100,
		S_HOST   = 0x00000200,
		S_UPDATE = 0x00000400
	};

	enum {
		ST0_INCS = 0x000001,
		ST0_DIRI = 0x000002,
		ST0_FI   = 0x000004,
		ST0_SIM  = 0x000008,
		ST0_PLRI = 0x000020,
		ST0_PBCI = 0x000040,
		ST0_DIRO = 0x000080,
		ST0_FO   = 0x000100,
		ST0_SOM  = 0x000600,
		ST0_PLRO = 0x000800,
		ST0_PBCO = 0x001000,
		ST0_CNS  = 0x002000,
		ST0_WORD = 0x004000,
		ST0_SEL  = 0x008000,
		ST0_M    = 0x030000, ST0_M_64K = 0x000000, ST0_M_256K = 0x010000, ST0_M_1M = 0x020000,
		ST0_SRAM = 0x200000,

		ST1_AOV  = 0x000001,
		ST1_SFAI = 0x000002,
		ST1_SFAO = 0x000004,
		ST1_AOVM = 0x000008, // undocumented!
		ST1_MOVM = 0x000020,
		ST1_MOV  = 0x000040,
		ST1_SFMA = 0x000180, ST1_SFMA_SHIFT = 7,
		ST1_SFMO = 0x001800, ST1_SFMO_SHIFT = 11,
		ST1_RND  = 0x038000, ST1_RND_SHIFT = 15,
		ST1_CRM  = 0x0C0000, ST1_CRM_SHIFT = 18, ST1_CRM_32 = 0x000000, ST1_CRM_16H = 0x040000, ST1_CRM_16L = 0x080000,
		ST1_DBP  = 0x100000,
		ST1_CAS  = 0x200000,

		ST1_CACHE = ST1_SFAI|ST1_SFAO|ST1_MOVM|ST1_SFMA|ST1_SFMO|ST1_RND|ST1_CRM|ST1_DBP
	};

	enum { BR_UB, BR_CB, BR_IDLE };

	enum { IBS = 8192, HBS = 4096 };

	enum { INC_CA = 1, INC_ID = 2 };

	enum { DMEM_SIZE = 0x40000 };	// external data memory (bytes): 256 KB on the Taito Zoom board; accesses beyond it are unmapped (read 0, write ignored) like in MAME

	struct icd {
		u16 op;
		s16 next;
		u8 param;
	};

	struct hcd {
		u32 st1;
		s16 ipc;
		s16 next;
	};

	struct cd {
		s16 hashbase[256];
		hcd hashnode[HBS];
		icd inst[IBS];
		int hused, iused;
	};

	struct cstate {
		int branch;
		int inc;
		s16 hnode;
		s16 ipc;
	};

	s64 macc, macc_read, macc_write;

	u32 cmem[256];
	u32 dmem0[256];
	u32 dmem1[32];

	u32 pgm[256];		// program memory (MAME: internal_pgm address space)
	u8* dmem;			// external data memory (MAME: AS_DATA address space)

	u32 st1, sti;
	u32 aacc, xoa, xba, xwr, xrd, txrd, creg;

	u8 pc, hpc, ca, id, ba0, ba1, rptc, rptc_next, sa;

	u32 xm_adr;

	u8 host[4], hidx, allow_update;

	u32 update[16];
	u8 update_addr[16];	// coefficient address of each queued update (see get_cmem)
	u8 update_counter_head, update_counter_tail;

	cd cache;

	int icount;
	int unsupported_inst_warning;

	void decode_error(u32 opcode);
	void decode_cat1(u32 opcode, u16 *op, cstate *cs);
	void decode_cat2_pre(u32 opcode, u16 *op, cstate *cs);
	void decode_cat3(u32 opcode, u16 *op, cstate *cs);
	void decode_cat2_post(u32 opcode, u16 *op, cstate *cs);

	inline int xmode(u32 opcode, char type, cstate *cs);
	inline int sfao(u32 st1);
	inline int dbp(u32 st1);
	inline int crm(u32 st1);
	inline int sfai(u32 st1);
	inline int sfmo(u32 st1);
	inline int rnd(u32 st1);
	inline int movm(u32 st1);
	inline int sfma(u32 st1);

	void update_dready();
	void update_pc0();
	void update_empty();

	void xm_init();
	void xm_step_read();
	void xm_step_write();
	s64 macc_to_output_0(s64 rounding, u64 rmask);
	s64 macc_to_output_1(s64 rounding, u64 rmask);
	s64 macc_to_output_2(s64 rounding, u64 rmask);
	s64 macc_to_output_3(s64 rounding, u64 rmask);
	s64 macc_to_output_0s(s64 rounding, u64 rmask);
	s64 macc_to_output_1s(s64 rounding, u64 rmask);
	s64 macc_to_output_2s(s64 rounding, u64 rmask);
	s64 macc_to_output_3s(s64 rounding, u64 rmask);
	s64 check_macc_overflow_0();
	s64 check_macc_overflow_1();
	s64 check_macc_overflow_2();
	s64 check_macc_overflow_3();
	s64 check_macc_overflow_0s();
	s64 check_macc_overflow_1s();
	s64 check_macc_overflow_2s();
	s64 check_macc_overflow_3s();
	void cache_flush();
	void add_one(cstate *cs, u16 op, u8 param);
	void decode_one(u32 opcode, cstate *cs, void (tms57002::*dec)(u32 opcode, u16 *op, cstate *cs));
	s16 get_hash(u8 adr, u32 st1, s16 *pnode);
	s16 get_hashnode(u8 adr, u32 st1, s16 pnode);
	int decode_get_pc();
	u32 get_cmem(u8 addr);

#define CINTRPDECL
#include "tms57002_ops.hxx"
#undef CINTRPDECL
};

tms57002::tms57002()
	: macc(0), macc_read(0), macc_write(0), st0(0), st1(0), sti(0), txrd(0)
{
	memset(cmem, 0, sizeof(cmem));
	memset(dmem0, 0, sizeof(dmem0));
	memset(dmem1, 0, sizeof(dmem1));
	memset(pgm, 0, sizeof(pgm));
	memset(si, 0, sizeof(si));
	memset(so, 0, sizeof(so));
	memset(update, 0, sizeof(update));
	memset(update_addr, 0, sizeof(update_addr));
	memset(host, 0, sizeof(host));
	dmem = (u8*)calloc(DMEM_SIZE, 1);
	aacc = xoa = xba = xwr = xrd = creg = 0;
	pc = hpc = ca = id = ba0 = ba1 = rptc = rptc_next = sa = 0;
	xm_adr = 0;
	hidx = 0;
	allow_update = 0;
	update_counter_head = update_counter_tail = 0;
	icount = 0;
	unsupported_inst_warning = 0;
	sti = S_IDLE;
	cache_flush();
}

tms57002::~tms57002()
{
	free(dmem);
}

void tms57002::pload_w(int state)
{
	u8 olds = sti;
	if (state)
		sti &= ~IN_PLOAD;
	else
		sti |= IN_PLOAD;
	if (olds ^ sti)
	{
		if (sti & IN_PLOAD)
		{
			hidx = 0;
			pc = 0;
			ca = 0;
			sti &= ~(SU_MASK);
		}
	}
}

void tms57002::cload_w(int state)
{
	u8 olds = sti;
	if (state)
		sti &= ~IN_CLOAD;
	else
		sti |= IN_CLOAD;
	if (olds ^ sti)
	{
		if (sti & IN_CLOAD)
		{
			hidx = 0;
			//ca = 0; // Seems extremely dubious
		}
	}
}

void tms57002::reset()
{
	sti = (sti & ~(SU_MASK|S_READ|S_WRITE|S_BRANCH|S_HOST|S_UPDATE)) | (SU_ST0|S_IDLE);
	pc = 0;
	ca = 0;
	hidx = 0;
	id = 0;
	ba0 = 0;
	ba1 = 0;
	sa = 0;
	rptc = 0;
	rptc_next = 0;
	update_counter_tail = 0;
	update_counter_head = 0;
	st0 &= ~(ST0_INCS | ST0_DIRI | ST0_FI | ST0_SIM | ST0_PLRI |
			ST0_PBCI | ST0_DIRO | ST0_FO | ST0_SOM | ST0_PLRO |
			ST0_PBCO | ST0_CNS);
	st1 &= ~(ST1_AOV | ST1_SFAI | ST1_SFAO | ST1_AOVM | ST1_MOVM | ST1_MOV |
			ST1_SFMA | ST1_SFMO | ST1_RND | ST1_CRM | ST1_DBP);
	update_dready();
	update_pc0();
	update_empty();

	xba = 0;
	xoa = 0;
	cache_flush();
}

void tms57002::data_w(u8 data)
{
	switch (sti & (IN_PLOAD|IN_CLOAD))
	{
	case 0:
		hidx = 0;
		sti &= ~SU_CVAL;
		break;
	case IN_PLOAD:
		host[hidx++] = data;
		if (hidx >= 3)
		{
			u32 val = (host[0]<<16) | (host[1]<<8) | host[2];
			hidx = 0;

			switch (sti & SU_MASK)
			{
			case SU_ST0:
				st0 = val;
				sti = (sti & ~SU_MASK) | SU_ST1;
				break;
			case SU_ST1:
				st1 = val;
				sti = (sti & ~SU_MASK) | SU_PRG;
				break;
			case SU_PRG:
				pgm[pc++] = val;
				update_pc0();
				break;
			}
		}
		break;
	case IN_CLOAD:
		if (sti & SU_CVAL)
		{
			host[hidx++] = data;
			if (hidx >= 4)
			{
				u32 val = (host[0]<<24) | (host[1]<<16) | (host[2]<<8) | host[3];
				sti &= ~SU_CVAL;
				update[update_counter_head] = val;
				update_addr[update_counter_head] = sa;
				update_counter_head = (update_counter_head + 1) & 0x0f;
				hidx = 1; // the write shouldn't really happen until CLOAD is high though
				update_empty();
			}
		}
		else
		{
			sa = data;
			hidx = 0;
			sti |= SU_CVAL;
		}

		break;
	case IN_PLOAD|IN_CLOAD:
		host[hidx++] = data;
		if (hidx >= 4)
		{
			u32 val = (host[0]<<24) | (host[1]<<16) | (host[2]<<8) | host[3];
			hidx = 0;
			cmem[ca++] = val;
		}
		break;
	};
}

u8 tms57002::data_r()
{
	u8 res;
	if (!(sti & S_HOST))
		return 0xff;

	res = host[hidx];
	hidx++;
	if (hidx == 4)
	{
		hidx = 0;
		sti &= ~S_HOST;
		update_dready();
	}

	return res;
}

void tms57002::update_dready()
{
	// DREADY pin: not connected (host CPU not emulated)
}

void tms57002::update_pc0()
{
	// PC0 pin: not connected
}

void tms57002::update_empty()
{
	// EMPTY pin: not connected
}

void tms57002::sync_w(int state)
{
	if (sti & (IN_PLOAD /*| IN_CLOAD*/))
		return;

	allow_update = 1;
	pc = 0;
	ca = 0;
	id = 0;
	if (!(st0 & ST0_INCS))
	{
		ba0--;
		ba1++;
	}
	xba = (xba-1) & 0x7ffff;
	st1 &= ~(ST1_AOV | ST1_MOV);
	sti &= ~S_IDLE;
}

void tms57002::xm_init()
{
	u32 adr = xoa + xba;
	u32 mask = 0;

	switch (st0 & ST0_M)
	{
	case ST0_M_64K:  mask = 0x0ffff; break;
	case ST0_M_256K: mask = 0x3ffff; break;
	case ST0_M_1M:   mask = 0xfffff; break;
	}
	if (st0 & ST0_WORD)
		adr <<= 2;
	else
		adr <<= 1;

	if (!(st0 & ST0_SEL))
		adr <<= 1;

	xm_adr = adr & mask;
}

inline void tms57002::xm_step_read()
{
	u32 adr = xm_adr;
	u8 v = (adr < DMEM_SIZE) ? dmem[adr] : 0;
	int done;
	if (st0 & ST0_WORD)
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 3) << 3);
			txrd = (txrd & ~(0xff << off)) | (v << off);
			done = off == 0;
		}
		else
		{
			int off = 20 - ((adr & 7) << 2);
			txrd = (txrd & ~(0xf << off)) | ((v & 0xf) << off);
			done = off == 0;
		}
	}
	else
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 1) << 3);
			txrd = (txrd & ~(0xff << off)) | (v << off);
			done = off == 8;
			if (done)
				txrd &= 0xffff00;
		}
		else
		{
			int off = 20 - ((adr & 3) << 2);
			txrd = (txrd & ~(0xf << off)) | ((v & 0xf) << off);
			done = off == 8;
			if (done)
				txrd &= 0xffff00;
		}
	}
	if (done)
	{
		xrd = txrd;
		sti &= ~S_READ;
		xm_adr = 0;
	}
	else
		xm_adr = adr+1;
}

inline void tms57002::xm_step_write()
{
	u32 adr = xm_adr;
	u8 v;
	int done;
	if (st0 & ST0_WORD)
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 3) << 3);
			v = xwr >> off;
			done = off == 0;
		}
		else
		{
			int off = 20 - ((adr & 7) << 2);
			v = (xwr >> off) & 0xf;
			done = off == 0;
		}
	}
	else
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 1) << 3);
			v = xwr >> off;
			done = off == 8;
		}
		else
		{
			int off = 20 - ((adr & 3) << 2);
			v = (xwr >> off) & 0xf;
			done = off == 8;
		}
	}
	if (adr < DMEM_SIZE)
		dmem[adr] = v;
	if (done)
	{
		sti &= ~S_WRITE;
		xm_adr = 0;
	}
	else
		xm_adr = adr+1;
}

s64 tms57002::macc_to_output_0(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002::macc_to_output_1(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
		over = true;
	m <<= 2;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002::macc_to_output_2(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
		over = true;
	m <<= 4;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002::macc_to_output_3(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m >>= 8;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002::macc_to_output_0s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002::macc_to_output_1s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
		over = true;
	m <<= 2;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002::macc_to_output_2s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
		over = true;
	m <<= 4;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002::macc_to_output_3s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m >>= 8;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002::check_macc_overflow_0()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002::check_macc_overflow_1()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002::check_macc_overflow_2()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002::check_macc_overflow_3()
{
	return macc_read;
}

s64 tms57002::check_macc_overflow_0s()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002::check_macc_overflow_1s()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002::check_macc_overflow_2s()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002::check_macc_overflow_3s()
{
	return macc_read;
}

u32 tms57002::get_cmem(u8 addr)
{
	// Coefficient update through the host CLOAD path: the queued value is written when the program
	// reads the coefficient it was sent for. MAME applies the queue to the reads that follow the read
	// of the last address sent, which depends on how the host writes interleave with the program; a
	// VGM delivers a whole burst between two passes, so the queue is matched by address instead
	// (this is what the hardware does, and what MAME's timing produces when a burst is spread out).
	if (update_counter_head != update_counter_tail && update_addr[update_counter_tail] == addr)
	{
		cmem[addr] = update[update_counter_tail];
		update_counter_tail = (update_counter_tail + 1) & 0x0f;
		update_empty();
		return cmem[addr]; // The value of crm is ignored during an update.
	}
	else
	{
		int crm = (st1 & ST1_CRM) >> ST1_CRM_SHIFT;
		u32 cvar = cmem[addr];
		if (crm == 1)
			return (cvar & 0xffff0000);
		else if (crm == 2)
			return (cvar << 16);
		return cvar;
	}
}

void tms57002::cache_flush()
{
	int i;
	cache.hused = cache.iused = 0;
	for (i = 0; i != 256; i++)
		cache.hashbase[i] = -1;
	for (i = 0; i != HBS; i++)
	{
		cache.hashnode[i].st1 = 0;
		cache.hashnode[i].ipc = -1;
		cache.hashnode[i].next = -1;
	}
	for (i = 0; i != IBS; i++)
	{
		cache.inst[i].op = 0;
		cache.inst[i].next = -1;
		cache.inst[i].param = 0;
	}
}

void tms57002::add_one(cstate *cs, u16 op, u8 param)
{
	s16 ipc = cache.iused++;
	cache.inst[ipc].op = op;
	cache.inst[ipc].param = param;
	cache.inst[ipc].next = -1;
	if (cs->ipc != -1)
		cache.inst[cs->ipc].next = ipc;
	cs->ipc = ipc;
	if (cs->hnode != -1)
	{
		cache.hashnode[cs->hnode].ipc = ipc;
		cs->hnode = -1;
	}
}

void tms57002::decode_one(u32 opcode, cstate *cs, void (tms57002::*dec)(u32 opcode, u16 *op, cstate *cs))
{
	u16 op = 0;
	(this->*dec)(opcode, &op, cs);
	if (!op)
		return;
	add_one(cs, op, opcode & 0xff);
}

s16 tms57002::get_hash(u8 adr, u32 st1, s16 *pnode)
{
	s16 hnode;
	st1 &= ST1_CACHE;
	*pnode = -1;
	hnode = cache.hashbase[adr];
	while(hnode != -1)
	{
		if (cache.hashnode[hnode].st1 == st1)
			return cache.hashnode[hnode].ipc;
		*pnode = hnode;
		hnode = cache.hashnode[hnode].next;
	}
	return -1;
}

s16 tms57002::get_hashnode(u8 adr, u32 st1, s16 pnode)
{
	s16 hnode = cache.hused++;
	cache.hashnode[hnode].st1 = st1 & ST1_CACHE;
	cache.hashnode[hnode].ipc = -1;
	cache.hashnode[hnode].next = -1;
	if (pnode == -1)
		cache.hashbase[adr] = hnode;
	else
		cache.hashnode[pnode].next = hnode;
	return hnode;
}

int tms57002::decode_get_pc()
{
	s16 pnode, res;
	cstate cs;
	u8 adr = pc;

	res = get_hash(adr, st1, &pnode);
	if (res != -1)
		return res;

	if (HBS - cache.hused < 256 || IBS - cache.iused < 256*3)
	{
		cache_flush();
		pnode = -1;
	}

	cs.hnode = res = get_hashnode(adr, st1, pnode);
	cs.ipc = -1;
	cs.branch = 0;

	for (;;)
	{
		s16 ipc;
		u32 opcode = pgm[adr];

		cs.inc = 0;

		if ((opcode & 0xfc0000) == 0xfc0000)
			decode_one(opcode, &cs, &tms57002::decode_cat3);
		else {
			decode_one(opcode, &cs, &tms57002::decode_cat2_pre);
			decode_one(opcode, &cs, &tms57002::decode_cat1);
			decode_one(opcode, &cs, &tms57002::decode_cat2_post);
		}
		add_one(&cs, cs.inc, 0);

		if (cs.branch)
			break;

		adr++;
		ipc = get_hash(adr, st1, &pnode);
		if (ipc != -1)
		{
			cache.inst[cs.ipc].next = ipc;
			break;
		}
		cs.hnode = get_hashnode(adr, st1, pnode);
	}

	return cache.hashnode[res].ipc;
}

void tms57002::run(int cycles)
{
	int ipc = -1;

	icount = cycles;
	while(icount > 0 && !(sti & (S_IDLE | IN_PLOAD /*| IN_CLOAD*/)))
	{
		int iipc;

		if (ipc == -1)
			ipc = decode_get_pc();

		iipc = ipc;

		if (sti & (S_READ|S_WRITE))
		{
			if (sti & S_READ)
				xm_step_read();
			else
				xm_step_write();
		}

		macc_read = macc_write;
		macc_write = macc;

		for (;;)
		{
			const icd *i = cache.inst + ipc;

			ipc = i->next;
			switch (i->op)
			{
			case 0:
				goto inst;

			case 1:
				++ca;
				goto inst;

			case 2:
				++id;
				goto inst;

			case 3:
				++ca, ++id;
				goto inst;

#define CINTRPSWITCH
#include "tms57002_ops.hxx"
#undef CINTRPSWITCH

			default:
				// unhandled opcode in the instruction cache
				goto inst;
			}
		}
	inst:
		icount--;

		if (rptc)
		{
			rptc--;
			ipc = iipc;
		}
		else if (sti & S_BRANCH)
		{
			sti &= ~S_BRANCH;
			ipc = -1;
		}
		else
		{
			pc++; // Wraps if it reaches 256, next wraps too
			update_pc0();
		}

		if (rptc_next)
		{
			rptc = rptc_next;
			rptc_next = 0;
		}
	}

	if (icount > 0)
		icount = 0;
}

/***************************************************************************
    decoder (tms57kdec.cpp)
***************************************************************************/

inline int tms57002::xmode(u32 opcode, char type, cstate *cs)
{
	if (((opcode & 0x400) && (type == 'c')) || (!(opcode & 0x400) && (type == 'd')))
	{
		if (opcode & 0x100)
			return 0;
		else if (opcode & 0x80)
			cs->inc |= type == 'c' ? INC_CA : INC_ID;

		return 1;
	}
	else if (opcode & 0x200)
		cs->inc |= type == 'c' ? INC_CA : INC_ID;

	return 1;
}

inline int tms57002::sfao(u32 st1)
{
	return st1 & ST1_SFAO ? 1 : 0;
}

inline int tms57002::dbp(u32 st1)
{
	return st1 & ST1_DBP ? 1 : 0;
}

inline int tms57002::crm(u32 st1)
{
	// value overridden during cvar update (as in MAME; the queue is consumed within the pass that reads the coefficients)
	if (update_counter_head != update_counter_tail)
		return 0;

	int crm = (st1 & ST1_CRM) >> ST1_CRM_SHIFT;
	return crm <= 2 ? crm : 0;
}

inline int tms57002::sfai(u32 st1)
{
	return st1 & ST1_SFAI ? 1 : 0;
}

inline int tms57002::sfmo(u32 st1)
{
	return (st1 & ST1_SFMO) >> ST1_SFMO_SHIFT;
}

inline int tms57002::rnd(u32 st1)
{
	int rnd = (st1 & ST1_RND) >> ST1_RND_SHIFT;
	return rnd <= 4 ? rnd : 0;
}

inline int tms57002::movm(u32 st1)
{
	return st1 & ST1_MOVM ? 1 : 0;
}

inline int tms57002::sfma(u32 st1)
{
	return (st1 & ST1_SFMA) >> ST1_SFMA_SHIFT;
}

void tms57002::decode_error(u32 opcode)
{
	if (unsupported_inst_warning)
		return;

	unsupported_inst_warning = 1;
}

void tms57002::decode_cat1(u32 opcode, unsigned short *op, cstate *cs)
{
	switch (opcode >> 18)
	{
	case 0x00: // nop
		break;

#define CDEC1
#include "tms57002_ops.hxx"
#undef CDEC1

	default:
		decode_error(opcode);
		break;
	}
}

void tms57002::decode_cat2_pre(u32 opcode, unsigned short *op, cstate *cs)
{
	switch ((opcode >> 11) & 0x7f)
	{
	case 0x00: // nop
		break;

#define CDEC2A
#include "tms57002_ops.hxx"
#undef CDEC2A

	default:
		decode_error(opcode);
		break;
	}
}

void tms57002::decode_cat2_post(u32 opcode, unsigned short *op, cstate *cs)
{
	switch ((opcode >> 11) & 0x7f)
	{
	case 0x00: // nop
		break;

#define CDEC2B
#include "tms57002_ops.hxx"
#undef CDEC2B

	default:
		decode_error(opcode);
		break;
	}
}

void tms57002::decode_cat3(u32 opcode, unsigned short *op, cstate *cs)
{
	switch ((opcode >> 11) & 0x7f)
	{
	case 0x00: // nop
		break;

#define CDEC3
#include "tms57002_ops.hxx"
#undef CDEC3

	default:
		decode_error(opcode);
		break;
	}
}

/***************************************************************************
    opcode implementations (tmsops.cpp)
***************************************************************************/

#define CINTRP
#include "tms57002_ops.hxx"
#undef CINTRP

/***************************************************************************
    state image
***************************************************************************/

namespace {
struct Writer
{
	u8* p; u32 size, pos;
	Writer(u8* b, u32 s) : p(b), size(s), pos(0) {}
	void u8w(u32 v) { if (p && pos < size) p[pos] = (u8)v; pos++; }
	void u32w(u32 v) { u8w(v); u8w(v >> 8); u8w(v >> 16); u8w(v >> 24); }
	void s64w(s64 v) { u32w((u32)(u64)v); u32w((u32)((u64)v >> 32)); }
	void bytes(const u8* b, u32 n) { for (u32 i = 0; i < n; i++) u8w(b[i]); }
};
struct Reader
{
	const u8* p; u32 size, pos; bool ok;
	Reader(const u8* b, u32 s) : p(b), size(s), pos(0), ok(true) {}
	u32 u8r() { if (pos >= size) { ok = false; return 0; } return p[pos++]; }
	u32 u32r() { u32 v = u8r(); v |= u8r() << 8; v |= u8r() << 16; v |= u8r() << 24; return v; }
	s64 s64r() { u64 lo = u32r(); u64 hi = u32r(); return (s64)(lo | (hi << 32)); }
};
}

u32 tms57002::state_save(u8* buf, u32 bufSize) const
{
	Writer w(buf, bufSize);
	w.u32w(1);
	w.u32w(st0); w.u32w(st1); w.u32w(sti); w.u32w(aacc); w.u32w(xoa); w.u32w(xba); w.u32w(xwr); w.u32w(xrd); w.u32w(txrd); w.u32w(creg);
	w.s64w(macc); w.s64w(macc_read); w.s64w(macc_write);
	w.u8w(pc); w.u8w(hpc); w.u8w(ca); w.u8w(id); w.u8w(ba0); w.u8w(ba1); w.u8w(rptc); w.u8w(rptc_next); w.u8w(sa); w.u8w(hidx);
	w.u8w(allow_update); w.u8w(update_counter_head); w.u8w(update_counter_tail); w.u8w(0); w.u8w(0); w.u8w(0);
	w.bytes(host, 4);
	w.u32w(xm_adr);
	for (int i = 0; i < 16; i++) w.u32w(update[i]);
	w.bytes(update_addr, 16);
	for (int i = 0; i < 4; i++) w.u32w(si[i]);
	for (int i = 0; i < 4; i++) w.u32w(so[i]);
	for (int i = 0; i < 256; i++) w.u32w(pgm[i]);
	for (int i = 0; i < 256; i++) w.u32w(cmem[i]);
	for (int i = 0; i < 256; i++) w.u32w(dmem0[i]);
	for (int i = 0; i < 32; i++) w.u32w(dmem1[i]);
	// external memory: spans of non-zero bytes (gaps of up to 64 zero bytes are kept inside a span)
	u32 nspans = 0;
	u32 npos = w.pos;
	w.u32w(0);
	for (u32 i = 0; i < DMEM_SIZE; )
	{
		if (dmem[i] == 0) { i++; continue; }
		u32 start = i, end = i + 1;
		while (end < DMEM_SIZE)
		{
			u32 k = end;
			while (k < DMEM_SIZE && k < end + 64 && dmem[k] == 0) k++;
			if (k >= DMEM_SIZE || dmem[k] == 0) break;
			end = k + 1;
		}
		w.u32w(start); w.u32w(end - start); w.bytes(dmem + start, end - start);
		nspans++;
		i = end;
	}
	if (buf && npos + 4 <= bufSize) { buf[npos] = (u8)nspans; buf[npos + 1] = (u8)(nspans >> 8); buf[npos + 2] = (u8)(nspans >> 16); buf[npos + 3] = (u8)(nspans >> 24); }
	return w.pos;
}

bool tms57002::state_load(const u8* buf, u32 size)
{
	Reader r(buf, size);
	if (r.u32r() != 1) return false;
	st0 = r.u32r(); st1 = r.u32r(); sti = r.u32r(); aacc = r.u32r(); xoa = r.u32r(); xba = r.u32r(); xwr = r.u32r(); xrd = r.u32r(); txrd = r.u32r(); creg = r.u32r();
	macc = r.s64r(); macc_read = r.s64r(); macc_write = r.s64r();
	pc = r.u8r(); hpc = r.u8r(); ca = r.u8r(); id = r.u8r(); ba0 = r.u8r(); ba1 = r.u8r(); rptc = r.u8r(); rptc_next = r.u8r(); sa = r.u8r(); hidx = r.u8r();
	allow_update = r.u8r(); update_counter_head = r.u8r() & 0x0f; update_counter_tail = r.u8r() & 0x0f; r.u8r(); r.u8r(); r.u8r();
	for (int i = 0; i < 4; i++) host[i] = r.u8r();
	xm_adr = r.u32r();
	for (int i = 0; i < 16; i++) update[i] = r.u32r();
	for (int i = 0; i < 16; i++) update_addr[i] = r.u8r();
	for (int i = 0; i < 4; i++) si[i] = r.u32r();
	for (int i = 0; i < 4; i++) so[i] = r.u32r();
	for (int i = 0; i < 256; i++) pgm[i] = r.u32r();
	for (int i = 0; i < 256; i++) cmem[i] = r.u32r();
	for (int i = 0; i < 256; i++) dmem0[i] = r.u32r();
	for (int i = 0; i < 32; i++) dmem1[i] = r.u32r();
	memset(dmem, 0, DMEM_SIZE);
	u32 nspans = r.u32r();
	for (u32 s = 0; s < nspans && r.ok; s++)
	{
		u32 ofs = r.u32r(), len = r.u32r();
		for (u32 i = 0; i < len; i++) { u32 b = r.u8r(); if (ofs + i < DMEM_SIZE) dmem[ofs + i] = (u8)b; }
	}
	cache_flush();	// the decoded instruction cache is derived from the program and ST1
	return r.ok;
}

/***************************************************************************
    C interface
***************************************************************************/

struct tms57002_state
{
	tms57002 dsp;
};

extern "C" {

TMS57002* tms57002_create(void)
{
	return new tms57002_state;
}

void tms57002_destroy(TMS57002* dsp)
{
	delete dsp;
}

void tms57002_reset(TMS57002* dsp)
{
	dsp->dsp.reset();
}

void tms57002_pload_w(TMS57002* dsp, int state)
{
	dsp->dsp.pload_w(state);
}

void tms57002_cload_w(TMS57002* dsp, int state)
{
	dsp->dsp.cload_w(state);
}

void tms57002_data_w(TMS57002* dsp, UINT8 data)
{
	dsp->dsp.data_w(data);
}

UINT8 tms57002_data_r(TMS57002* dsp)
{
	return dsp->dsp.data_r();
}

void tms57002_set_input(TMS57002* dsp, UINT8 channel, INT32 sample)
{
	dsp->dsp.si[channel & 3] = (u32)sample & 0xffffff;
}

INT32 tms57002_get_output(const TMS57002* dsp, UINT8 channel)
{
	return (INT32)(dsp->dsp.so[channel & 3] << 8) >> 8;
}

UINT8 tms57002_sim(const TMS57002* dsp)
{
	return (dsp->dsp.st0 & 0x000008) ? 1 : 0;	// ST0_SIM
}

void tms57002_sync(TMS57002* dsp)
{
	dsp->dsp.sync_w(1);
}

void tms57002_run(TMS57002* dsp, int cycles)
{
	dsp->dsp.run(cycles);
}

void tms57002_prerun(TMS57002* dsp, UINT32 periods, int cycles)
{
	UINT32 k;
	for (k = 0; k < 4; k++)
		dsp->dsp.si[k] = 0;
	for (k = 0; k < periods; k++)
	{
		dsp->dsp.sync_w(1);
		dsp->dsp.run(cycles);
	}
}

UINT32 tms57002_state_save(const TMS57002* dsp, UINT8* buf, UINT32 bufSize)
{
	return dsp->dsp.state_save(buf, bufSize);
}

UINT8 tms57002_state_load(TMS57002* dsp, const UINT8* buf, UINT32 size)
{
	return dsp->dsp.state_load(buf, size) ? 0 : 1;
}

}	// extern "C"
