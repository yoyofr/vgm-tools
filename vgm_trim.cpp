// vgm_trim - trim / loop / split / de-duplicate / ROM-optimise VGM logs; ES5505/ES5506 (+ ES5510 ESP)
// extension aware.
// Output ending in .vgz is gzip-compressed (zlib).
//
// build: c++ -O2 -std=c++17 -o vgm_trim vgm_trim.cpp -lz
//
// usage:
//   vgm_trim in.vgm out.vgz --start 18.2s --end 1:05.5 [--loop 30.0s] [--cmp] [--sro]
//   vgm_trim in.vgm out.vgz --auto --split log --track all --gap 5s --find-loop --cmp --sro
//   vgm_trim in.vgm --info
//   vgm_trim in.vgm --activity
#include <algorithm>
#include <memory>
#include <zlib.h>
#include <array>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <strings.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t i16;
typedef uint32_t u32;
typedef int64_t i64;
typedef std::vector<u8> Bytes;

static const i64 SAMPLE_RATE = 44100;
static const int PAGE_REG = 15;

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------
static u32 le32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static void put_le32(u8* p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void push_le32(Bytes& b, u32 v) { b.push_back(v); b.push_back(v >> 8); b.push_back(v >> 16); b.push_back(v >> 24); }
static void push_be16(Bytes& b, u32 v) { b.push_back((v >> 8) & 0xFF); b.push_back(v & 0xFF); }

[[noreturn]] static void die(const std::string& msg)
{
	fprintf(stderr, "%s\n", msg.c_str());
	exit(1);
}

static std::string fmt_pos(i64 samples)
{
	double s = (double)samples / SAMPLE_RATE;
	char buf[64];
	snprintf(buf, sizeof(buf), "%d:%06.3f (%lld)", (int)floor(s / 60), fmod(s, 60.0), (long long)samples);
	return buf;
}

// samples, "12.5s" or "m:ss.sss" -> samples
static i64 parse_pos(const std::string& txt)
{
	size_t colon = txt.find(':');
	if (colon != std::string::npos)
		return (i64)llround((atoi(txt.substr(0, colon).c_str()) * 60 + atof(txt.substr(colon + 1).c_str())) * SAMPLE_RATE);
	if (!txt.empty() && txt.back() == 's')
		return (i64)llround(atof(txt.substr(0, txt.size() - 1).c_str()) * SAMPLE_RATE);
	return atoll(txt.c_str());
}

static int cmd_length(u8 op)
{
	if (op == 0x30 || op == 0x3F || op == 0x4F || op == 0x50) return 2;
	if (op >= 0x31 && op <= 0x3E) return 2;
	if (op >= 0x40 && op <= 0x4E) return 3;
	if (op >= 0x51 && op <= 0x5F) return 3;
	if (op == 0x61) return 3;
	if (op == 0x62 || op == 0x63 || op == 0x66) return 1;
	if (op == 0x64) return 4;
	if (op == 0x67 || op == 0x68) return 0;
	if (op >= 0x70 && op <= 0x8F) return 1;
	if (op >= 0x90 && op <= 0x95) { static const int l[6] = {5, 5, 6, 11, 2, 5}; return l[op - 0x90]; }
	if (op >= 0xA0 && op <= 0xBF) return 3;
	if (op >= 0xC0 && op <= 0xDF) return 4;
	if (op >= 0xE0) return 5;
	return 1;
}

static i64 wait_of(const u8* d, u8 op)
{
	if (op == 0x61) return d[1] | (d[2] << 8);
	if (op == 0x62) return 735;
	if (op == 0x63) return 882;
	if (op >= 0x70 && op <= 0x7F) return (op & 15) + 1;
	if (op >= 0x80 && op <= 0x8F) return op & 15;
	return 0;
}

static void wait_cmd(Bytes& out, i64 n)
{
	while (n > 0)
	{
		if (n <= 16) { out.push_back(0x70 + n - 1); n = 0; }
		else if (n == 735) { out.push_back(0x62); n = 0; }
		else if (n == 882) { out.push_back(0x63); n = 0; }
		else { i64 c = std::min<i64>(n, 0xFFFF); out.push_back(0x61); out.push_back(c & 0xFF); out.push_back(c >> 8); n -= c; }
	}
}

// ---------------------------------------------------------------------------------------------
// VGM file model
// ---------------------------------------------------------------------------------------------
struct Cmd { size_t p; u8 op; size_t len; i64 t; };

struct VGMFile
{
	Bytes data;
	u32 version = 0, eof = 0, gd3 = 0, total = 0, loop_ofs = 0, loop_samples = 0, data_ofs = 0;
	u32 es5506_clock = 0, zsg2_clock = 0;
	bool is_es5506 = false, is_zsg2 = false;

	void load(const std::string& path)
	{
		FILE* f = fopen(path.c_str(), "rb");
		if (!f) die("cannot open " + path);
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		data.resize(n);
		if (n && fread(data.data(), 1, n, f) != (size_t)n) die("read error");
		fclose(f);
		if (n >= 2 && data[0] == 0x1f && data[1] == 0x8b)   // .vgz: gzip-compressed, inflate it
		{
			Bytes raw;
			gzFile g = gzopen(path.c_str(), "rb");
			if (!g) die("cannot open " + path);
			Bytes buf(1 << 20);
			for (;;)
			{
				int got = gzread(g, buf.data(), (unsigned)buf.size());
				if (got < 0) die("gzip read error: " + path);
				if (got == 0) break;
				raw.insert(raw.end(), buf.begin(), buf.begin() + got);
			}
			gzclose(g);
			data.swap(raw);
			n = (long)data.size();
		}
		if (n < 0x40 || memcmp(data.data(), "Vgm ", 4) != 0) die("not a VGM file");
		version = le32(&data[0x08]);
		eof = le32(&data[0x04]) + 0x04;
		gd3 = le32(&data[0x14]);
		if (gd3) gd3 += 0x14;
		total = le32(&data[0x18]);
		u32 loop = le32(&data[0x1C]);
		loop_ofs = loop ? loop + 0x1C : 0;
		loop_samples = le32(&data[0x20]);
		data_ofs = (version >= 0x150) ? le32(&data[0x34]) + 0x34 : 0x40;
		es5506_clock = (data_ofs > 0xD3) ? le32(&data[0xD0]) : 0;
		is_es5506 = (es5506_clock & 0x80000000) != 0;
		zsg2_clock = (data_ofs > 0x103) ? le32(&data[0x100]) : 0;   // VGM 1.72 extension: ZSG-2 (Taito Zoom) clock at 0x100 (header 0x110)
		is_zsg2 = zsg2_clock != 0;
	}

	// iterate the commands: f(cmd) returns false to stop
	template <typename F> void commands(F f) const
	{
		size_t p = data_ofs, end = gd3 ? gd3 : eof;
		if (end > data.size()) end = data.size();
		i64 t = 0;
		const u8* d = data.data();
		while (p < end)
		{
			u8 op = d[p];
			size_t len;
			if (op == 0x67) len = 7 + (p + 7 <= end ? (le32(&d[p + 3]) & 0x7FFFFFFF) : 0);
			else if (op == 0x68) len = 12;
			else len = cmd_length(op);
			if (!f(Cmd{p, op, len, t})) return;
			t += wait_of(&d[p], op);
			if (op == 0x66) return;
			p += len;
		}
	}
};

// ---------------------------------------------------------------------------------------------
// register model
// ---------------------------------------------------------------------------------------------
enum Group { GL = 0, GH = 1, GT = 2 };
static inline int page_group(int page) { return page < 0x20 ? GL : (page < 0x40 ? GH : GT); }

static const u32 ES5505_MASK_LOW[16] = {0x0FFF, 0xFFFE, 0x1FFF, 0xFFE0, 0x1FFF, 0xFFE0, 0xFFF0, 0xFFF0,
                                        0xFF00, 0xFF00, 0x1FFF, 0xFFFF, 0x0000, 0x001F, 0x0000, 0x007F};
static const u32 ES5505_MASK_HIGH[16] = {0x0FFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0, 0x001F, 0x0000, 0x007F};
// ES5505 test page: only registers 8, 13, 15 are listed, others default to "all bits"
static const u32 ES5505_MASK_TEST[16] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                         0xF807, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x001F, 0xFFFFFFFF, 0x007F};
static const u32 ES5506_MASK_LOW[16] = {0xFFFF, 0x1FFFF, 0xFFFF, 0xFF00, 0xFFFF, 0xFF00, 0x1FF, 0xFFFF,
                                        0xFF01, 0xFFFF, 0xFF01, 0x1F, 0x1F, 0, 0, 0x7F};
static const u32 ES5506_MASK_HIGH[16] = {0xFFFF, 0xFFFFF800, 0xFFFFFF80, 0xFFFFFFFF, 0x3FFFF, 0x3FFFF, 0x3FFFF,
                                         0x3FFFF, 0x3FFFF, 0x3FFFF, 0x7F, 0x7F, 0x7F, 0, 0, 0x7F};
static const u32 ES5506_MASK_TEST[16] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                         0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x7F};
// the emitter's ES5505 masks default to 0xFFFF for registers not listed on the test page
static const u32 ES5505_EMIT_TEST[16] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                                         0xF807, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x001F, 0xFFFF, 0x007F};

// side effects: bit reg of SIDE_EFFECT[chip][group]
static const u16 SIDE_EFFECT_5506[3] = {(1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9),
                                        (1 << 0) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9), 0};
static const u16 SIDE_EFFECT_5505[3] = {(1 << 0) | (1 << 10) | (1 << 11),
                                        (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6), 0};

struct RegWrite
{
	int page, reg;
	u32 value;
	bool changed;
	int lane;       // ES5505 single byte lane (0 = high, 1 = low), -1 = full
	bool has_prev;
	u32 prev;
};

struct Range { i64 first, last; };

struct ES55xxState
{
	bool is_es5506;
	int page = 0;
	// per-page registers (CR of page p and p+0x20 share (p & 0x1F, 0)); globals per group
	u32 regs[0x80][16];
	bool known[0x80][16];
	u32 gregs[3][16];
	bool gknown[3][16];
	int reg_count = 0;
	// ES5506 byte lane latch per (page, reg)
	u8 latch[0x80][16][4];
	u8 latch_mask[0x80][16];
	std::map<int, u32> voice_bank, region_window, region_alias;
	int irq_autoack = -1;
	std::map<int, u32> gains;
	std::vector<std::array<u8, 3>> esp_cmds;
	u32 running = 0;    // bit per voice
	std::vector<Range> rom_used[4];
	// playback position of each voice in host accumulator units, advanced from the host FC at
	// the chip sample rate (clock / (16 * (ACT + 1))), so that the state at any time can be
	// re-created with the sample where it actually is
	i64 now = 0;
	u32 clock = 0;
	double pos[32] = {};
	i64 pos_t[32] = {};
	bool pos_ok[32] = {};

	double chip_rate()
	{
		u32 act = 0;
		if (is_es5506) { if (!gknown[GL][11] || !(act = gregs[GL][11])) act = gknown[GH][11] ? gregs[GH][11] : 0x1F; }
		else { if (!gknown[GL][13] || !(act = gregs[GL][13])) act = gknown[GH][13] ? gregs[GH][13] : 0x1F; }
		return clock ? (double)clock / (16.0 * ((act & 0x1F) + 1)) : 29761.0;
	}
	u32 fc_of(int v) { u32 f = 0; get(v, 1, f); return is_es5506 ? (f & 0x1FFFF) : (f & 0xFFFE); }
	u32 start_of(int v) { u32 h = 0, l = 0; if (is_es5506) { get(0x20 + v, 1, h); return h; } get(v, 2, h); get(v, 3, l); return ((h & 0x1FFF) << 16) | (l & 0xFFFF); }
	u32 end_of(int v) { u32 h = 0, l = 0; if (is_es5506) { get(0x20 + v, 2, h); return h; } get(v, 4, h); get(v, 5, l); return ((h & 0x1FFF) << 16) | (l & 0xFFFF); }
	u32 acc_of(int v) { u32 h = 0, l = 0; if (is_es5506) { get(0x20 + v, 3, h); return h; } get(v, 10, h); get(v, 11, l); return ((h & 0x1FFF) << 16) | (l & 0xFFFF); }
	// move voice v from its last known position to time t (host units: FC/2 per chip sample)
	void advance(int v, i64 t)
	{
		if (!pos_ok[v] || !((running >> v) & 1) || t <= pos_t[v]) { pos_t[v] = t; return; }
		u32 cr = 0; get(v, 0, cr);
		double step = (double)fc_of(v) * 0.5 * ((double)(t - pos_t[v]) * chip_rate() / SAMPLE_RATE);
		double st_ = start_of(v), en = end_of(v), L = en - st_;
		int mode = cr & 0x18;   // LPE 0x08, BLE 0x10
		bool dir = (cr & 0x40) != 0;   // DIR: the chip flips it at each end of a bi-directional loop
		double p = pos[v];
		if (L <= 0) { p += dir ? -step : step; pos[v] = std::max(st_, std::min(en, p)); pos_t[v] = t; return; }
		if (mode == 0x08)   // uni-directional loop
		{
			p += dir ? -step : step;
			if (p > en) p = st_ + fmod(p - en, L);
			else if (p < st_) p = en - fmod(st_ - p, L);
		}
		else if (mode == 0x18)   // bi-directional: bounce between START and END, the first pass may begin below START
		{
			double left = step;
			int guard = 0;
			while (left > 0 && guard++ < 100000)
			{
				double room = dir ? (p - st_) : (en - p);
				if (room < 0) room = 0;
				if (left <= room) { p += dir ? -left : left; left = 0; break; }
				left -= room; p = dir ? st_ : en; dir = !dir;
				if (left > 4 * L) left = fmod(left, 2 * L);   // skip whole round trips
			}
			u32* vp; bool* kn; slot(v, 0, vp, kn);
			*vp = (*vp & ~0x40u) | (dir ? 0x40u : 0u);
		}
		else   // one shot: the chip stops the voice at the end
		{
			p += dir ? -step : step;
			if ((!dir && p > en) || (dir && p < st_)) { p = dir ? st_ : en; running &= ~(1u << v); u32* vp; bool* kn; slot(v, 0, vp, kn); *vp |= 1; }
		}
		pos[v] = p; pos_t[v] = t;
	}
	void restart_positions(i64 t) { for (int v = 0; v < 32; v++) { pos[v] = acc_of(v); pos_t[v] = t; pos_ok[v] = true; } }

	explicit ES55xxState(bool es5506) : is_es5506(es5506)
	{
		memset(known, 0, sizeof(known));
		memset(gknown, 0, sizeof(gknown));
		memset(regs, 0, sizeof(regs));
		memset(gregs, 0, sizeof(gregs));
		memset(latch_mask, 0, sizeof(latch_mask));
	}

	u32 mask(int page, int reg) const
	{
		int g = page_group(page);
		if (is_es5506) return g == GL ? ES5506_MASK_LOW[reg] : (g == GH ? ES5506_MASK_HIGH[reg] : ES5506_MASK_TEST[reg]);
		return g == GL ? ES5505_MASK_LOW[reg] : (g == GH ? ES5505_MASK_HIGH[reg] : ES5505_MASK_TEST[reg]);
	}

	bool is_global(int page, int reg) const
	{
		int g = page_group(page);
		if (reg == PAGE_REG) return true;
		if (is_es5506) return (g != GT && (reg == 11 || reg == 12)) || (g == GH && reg == 10);
		return reg == 13 || (g == GT && reg == 8);
	}

	bool side_effect(int page, int reg) const
	{
		const u16* tbl = is_es5506 ? SIDE_EFFECT_5506 : SIDE_EFFECT_5505;
		return (tbl[page_group(page)] >> reg) & 1;
	}

	// key resolution: returns pointers to the value / known flag
	void slot(int page, int reg, u32*& val, bool*& kn)
	{
		if (is_global(page, reg)) { int g = page_group(page); val = &gregs[g][reg]; kn = &gknown[g][reg]; return; }
		if (reg == 0 && page < 0x40) page &= 0x1F;
		val = &regs[page][reg]; kn = &known[page][reg];
	}
	bool get(int page, int reg, u32& out)
	{
		if (reg == PAGE_REG) { out = page_; return true; }
		u32* v; bool* k; slot(page, reg, v, k);
		if (!*k) return false;
		out = *v; return true;
	}
	int page_ = 0;   // alias of page kept in sync (see store)

	// voice registers by (voice, reg) on the low page and (0x20+voice, reg) on the high page
	bool lo(int voice, int reg, u32& out) { return get(voice, reg, out); }
	bool hi(int voice, int reg, u32& out) { return get(0x20 + voice, reg, out); }

	bool voice_range(int voice, int& region, i64& first, i64& last)
	{
		u32 control;
		if (!lo(voice, 0, control)) return false;
		i64 start, end, acc;
		if (is_es5506)
		{
			u32 s, e, a;
			if (!hi(voice, 1, s) || !hi(voice, 2, e)) return false;
			start = s >> 11; end = e >> 11;
			acc = hi(voice, 3, a) ? (i64)(a >> 11) : start;
			region = (control >> 14) & 3;
		}
		else
		{
			u32 r2, r3, r4, r5, r10, r11;
			if (!lo(voice, 2, r2) || !lo(voice, 3, r3) || !lo(voice, 4, r4) || !lo(voice, 5, r5)) return false;
			start = (((i64)r2 << 16) | r3) >> 9;
			end = (((i64)r4 << 16) | r5) >> 9;
			acc = (lo(voice, 10, r10) && lo(voice, 11, r11)) ? ((((i64)r10 << 16) | r11) >> 9) : start;
			region = (control >> 2) & 1;
		}
		auto al = region_alias.find(region);
		if (al != region_alias.end()) region = al->second;
		i64 base = 0;
		auto w = region_window.find(region);
		if (w != region_window.end()) base += (i64)w->second * 0x100000;
		auto vb = voice_bank.find(voice);
		if (vb != voice_bank.end()) base += (i64)vb->second * 0x100000;
		first = base + std::min(start, std::min(end, acc));
		last = base + std::max(start, std::max(end, acc)) + 1;
		return true;
	}

	void note_rom_use(int voice)
	{
		int region; i64 first, last;
		if (!voice_range(voice, region, first, last)) return;
		if (region < 0 || region > 3) return;
		std::vector<Range>& lst = rom_used[region];
		if (!lst.empty() && lst.back().first == first && lst.back().last == last) return;
		lst.push_back({first, last});
	}

	RegWrite store(int page, int reg, u32 value)
	{
		value &= mask(page, reg);
		if (reg == PAGE_REG)
		{
			bool changed = page_ != (int)value;
			page_ = value;
			return RegWrite{page, reg, value, changed, -1, false, 0};
		}
		{
			int voice = page & 0x1F;
			bool low = page < 0x20, high = page >= 0x20 && page < 0x40;
			bool is_act = is_es5506 ? (reg == 11 && page < 0x40) : (reg == 13 && page < 0x40);
			if (is_act) { for (int w = 0; w < 32; w++) advance(w, now); }
			else if ((low && (reg == 0 || reg == 1)) || (page < 0x40 && reg == 0)) advance(voice, now);   // CR, FC
		}
		u32* v; bool* k; slot(page, reg, v, k);
		bool had = *k;
		u32 old = *v;
		if (!had) reg_count++;
		*v = value; *k = true;
		if (page < 0x40)
		{
			int voice = page & 0x1F;
			if (reg == 0)
			{
				if (value & 3) running &= ~(1u << voice);
				else { running |= (1u << voice); note_rom_use(voice); }
			}
			else if ((running >> voice) & 1)
				note_rom_use(voice);
			// a host write of the accumulator places the voice
			if ((!is_es5506 && page < 0x20 && (reg == 10 || reg == 11)) || (is_es5506 && page >= 0x20 && reg == 3))
			{
				pos[voice] = acc_of(voice); pos_t[voice] = now; pos_ok[voice] = true;
			}
		}
		return RegWrite{page, reg, value, !had || old != value, -1, had, old};
	}

	// 0xBE aa dd -> logical writes (0, 1); returns false when nothing is to be emitted
	int write8(int aa, u8 dd, RegWrite& w)
	{
		if (aa >= 0x40)
		{
			int voice = aa & 0x1F;
			voice_bank[voice] = dd;
			if ((running >> voice) & 1) note_rom_use(voice);
			return 0;
		}
		if (is_es5506)
		{
			int reg = aa >> 2, lane = aa & 3;
			latch[page_][reg][lane] = dd;
			latch_mask[page_][reg] |= 1 << lane;
			if (lane == 3 && latch_mask[page_][reg] == 0xF)
			{
				const u8* l = latch[page_][reg];
				latch_mask[page_][reg] = 0;
				w = store(page_, reg, ((u32)l[0] << 24) | (l[1] << 16) | (l[2] << 8) | l[3]);
				return 1;
			}
			return 0;
		}
		int reg = aa >> 1, lane = aa & 1;
		u32 old = 0;
		bool kn = get(page_, reg, old);
		u32 value = lane == 0 ? ((old & 0x00FF) | ((u32)dd << 8)) : ((old & 0xFF00) | dd);
		w = store(page_, reg, value);
		if (!kn) w.changed = true;
		w.lane = lane;
		return 1;
	}

	// 0xD6 aa ddee (big endian)
	int write16(int aa, u32 data, RegWrite& w)
	{
		if (aa >= 0x40)
		{
			int voice = aa & 0x1F;
			voice_bank[voice] = data;
			if ((running >> voice) & 1) note_rom_use(voice);
			return 0;
		}
		if (is_es5506)
		{
			int reg = aa >> 2, half = (aa >> 1) & 1;
			latch[page_][reg][half * 2] = data >> 8;
			latch[page_][reg][half * 2 + 1] = data & 0xFF;
			latch_mask[page_][reg] |= 3 << (half * 2);
			if (half == 1 && latch_mask[page_][reg] == 0xF)
			{
				const u8* l = latch[page_][reg];
				latch_mask[page_][reg] = 0;
				w = store(page_, reg, ((u32)l[0] << 24) | (l[1] << 16) | (l[2] << 8) | l[3]);
				return 1;
			}
			return 0;
		}
		w = store(page_, aa >> 1, data);
		return 1;
	}

	bool ext(int sub, u8 aa, u8 dd)
	{
		bool changed;
		if (sub == 0x00)
		{
			int voice = aa & 0x1F;
			auto it = voice_bank.find(voice);
			changed = it == voice_bank.end() || it->second != dd;
			voice_bank[voice] = dd;
			if ((running >> voice) & 1) note_rom_use(voice);
		}
		else if (sub == 0x01)
		{
			auto it = region_window.find(aa & 3);
			changed = it == region_window.end() || it->second != dd;
			region_window[aa & 3] = dd;
			for (int v = 0; v < 32; v++)
				if ((running >> v) & 1) note_rom_use(v);
		}
		else if (sub == 0x02)
		{
			auto it = region_alias.find(aa & 3);
			changed = it == region_alias.end() || it->second != dd;
			region_alias[aa & 3] = dd;
		}
		else if (sub == 0x04)
		{
			changed = irq_autoack != dd;
			irq_autoack = dd;
		}
		else
			changed = true;
		return changed;
	}

	bool gain(u8 cc, u32 g)
	{
		auto it = gains.find(cc);
		bool changed = it == gains.end() || it->second != g;
		gains[cc] = g;
		return changed;
	}

	// register image for the initialisation sequence (globals, then pages; control last per page)
	std::vector<RegWrite> image() const
	{
		std::vector<RegWrite> writes;
		// Python sorts the global keys ('G', group, reg) with group chars 'H' < 'L' < 'T'
		static const int gorder[3] = {GH, GL, GT};
		static const int gpage[3] = {0x00, 0x20, 0x40};   // page of group L, H, T
		for (int gi = 0; gi < 3; gi++)
		{
			int g = gorder[gi];
			for (int r = 0; r < 16; r++)
				if (gknown[g][r] && r != PAGE_REG)
					writes.push_back(RegWrite{gpage[g], r, gregs[g][r], true, -1, false, 0});
		}
		for (int page = 0; page < 0x80; page++)
		{
			for (int r = 1; r < 16; r++)
				if (known[page][r])
					writes.push_back(RegWrite{page, r, regs[page][r], true, -1, false, 0});
			if (known[page][0])
				writes.push_back(RegWrite{page, 0, regs[page][0], true, -1, false, 0});
		}
		return writes;
	}
};

// ---------------------------------------------------------------------------------------------
// command emitter
// ---------------------------------------------------------------------------------------------
struct Emitter
{
	bool is_es5506;
	u8 cs;
	int out_page = -1;
	bool lane_opt = false;

	Emitter(bool es5506, u8 cs_) : is_es5506(es5506), cs(cs_) {}

	void page_cmd(Bytes& out, int page) const
	{
		out.push_back(0xBE);
		out.push_back((is_es5506 ? 0x3F : 0x1F) | cs);
		out.push_back(page);
	}

	static bool lane_trusted(int g, int reg, int lane)
	{
		if (reg == 0 && g != GT) return lane == 0;
		return !((SIDE_EFFECT_5505[g] >> reg) & 1);
	}

	void reg(Bytes& out, const RegWrite& w)
	{
		if (w.reg == PAGE_REG) return;
		if (out_page != w.page) { page_cmd(out, w.page); out_page = w.page; }
		if (is_es5506)
		{
			int aa = w.reg << 2;
			out.push_back(0xD6); out.push_back(aa | cs); push_be16(out, (w.value >> 16) & 0xFFFF);
			out.push_back(0xD6); out.push_back((aa | 2) | cs); push_be16(out, w.value & 0xFFFF);
			return;
		}
		int aa = w.reg << 1;
		int g = page_group(w.page);
		u32 mask = g == GL ? ES5505_MASK_LOW[w.reg] : (g == GH ? ES5505_MASK_HIGH[w.reg] : ES5505_EMIT_TEST[w.reg]);
		if (w.lane >= 0)
		{
			u8 byte = w.lane == 0 ? (w.value >> 8) : (w.value & 0xFF);
			out.push_back(0xBE); out.push_back((aa + w.lane) | cs); out.push_back(byte);
		}
		else if ((mask & 0x00FF) == 0)
		{
			out.push_back(0xBE); out.push_back(aa | cs); out.push_back((w.value >> 8) & 0xFF);
		}
		else if ((mask & 0xFF00) == 0)
		{
			out.push_back(0xBE); out.push_back((aa + 1) | cs); out.push_back(w.value & 0xFF);
		}
		else if (lane_opt && w.has_prev && lane_trusted(g, w.reg, 0) && ((w.prev ^ w.value) & mask & 0xFF00) == 0)
		{
			out.push_back(0xBE); out.push_back((aa + 1) | cs); out.push_back(w.value & 0xFF);
		}
		else if (lane_opt && w.has_prev && lane_trusted(g, w.reg, 1) && ((w.prev ^ w.value) & mask & 0x00FF) == 0)
		{
			out.push_back(0xBE); out.push_back(aa | cs); out.push_back((w.value >> 8) & 0xFF);
		}
		else
		{
			out.push_back(0xD6); out.push_back(aa | cs); push_be16(out, w.value & 0xFFFF);
		}
	}

	void page_sync(Bytes& out, int page)
	{
		if (out_page != page) { out_page = page; page_cmd(out, page); }
	}

	// every voice as it plays at time t: registers, computed playback position, then CR (a stopped
	// voice only gets its CR), plus ACT; makes the chip state equal to `st` whatever it was before
	void voice_image(Bytes& out, ES55xxState& st, i64 t)
	{
		auto w = [&](int page, int reg, u32 value) { reg_write(out, RegWrite{page, reg, value & st.mask(page, reg), true, -1, false, 0}); };
		// ACT is not rewritten: a write changes the chip sample rate and restarts the player's
		// resampler (a 30 ms dropout), and the body logs any real change anyway
		int dbg_run = 0, dbg_stop = 0;
		for (int v = 0; v < 32; v++)
		{
			u32 cr = 0;
			if (!st.get(v, 0, cr)) continue;
			if (!((st.running >> v) & 1)) { w(v, 0, cr | 3); dbg_stop++; continue; }
			st.advance(v, t);
			st.get(v, 0, cr);   // DIR may have been flipped by the bi-directional loop model
			if (!((st.running >> v) & 1)) { w(v, 0, cr | 3); dbg_stop++; continue; }
			dbg_run++;
			u32 acc = (u32)std::llround(std::max(0.0, st.pos[v]));
			u32 x;
			if (is_es5506)
			{
				for (int r = 1; r <= 0x0A; r++) if (st.get(v, r, x)) w(v, r, x);
				if (st.get(0x20 + v, 1, x)) w(0x20 + v, 1, x);
				if (st.get(0x20 + v, 2, x)) w(0x20 + v, 2, x);
				w(0x20 + v, 3, acc);
			}
			else
			{
				for (int r = 1; r <= 9; r++) if (st.get(v, r, x)) w(v, r, x);
				w(v, 10, (acc >> 16) & 0x1FFF);
				w(v, 11, acc & 0xFFFF);
			}
			st.pos[v] = acc; st.pos_t[v] = t; st.pos_ok[v] = true;
			w(v, 0, cr);
		}
		(void)dbg_run; (void)dbg_stop;
	}
	void reg_write(Bytes& out, const RegWrite& w) { reg(out, w); }

	void init(Bytes& out, const ES55xxState& st)
	{
		for (auto& kv : st.region_alias) { out.push_back(0xD7); out.push_back(0x02 | cs); out.push_back(kv.first); out.push_back(kv.second); }
		for (auto& kv : st.region_window) { out.push_back(0xD7); out.push_back(0x01 | cs); out.push_back(kv.first); out.push_back(kv.second); }
		for (auto& kv : st.voice_bank)
		{
			if (kv.second <= 0xFF) { out.push_back(0xD7); out.push_back(0x00 | cs); out.push_back(kv.first); out.push_back(kv.second); }
			else { out.push_back(0xD6); out.push_back(0x40 | kv.first | cs); push_be16(out, kv.second); }
		}
		if (st.irq_autoack >= 0) { out.push_back(0xD7); out.push_back(0x04 | cs); out.push_back(0x00); out.push_back(st.irq_autoack); }
		for (auto& c : st.esp_cmds) { out.push_back(0xD8); out.push_back(c[0] | cs); out.push_back(c[1]); out.push_back(c[2]); }
		for (auto& kv : st.gains) { out.push_back(0xE3); out.push_back(0x00 | cs); out.push_back(kv.first); push_be16(out, kv.second); }
		for (const RegWrite& w : st.image())
			reg(out, w);
		page_sync(out, st.page_);
	}
};

// ---------------------------------------------------------------------------------------------
// sample ROM blocks
// ---------------------------------------------------------------------------------------------
struct RomSet
{
	bool has[4] = {false, false, false, false};
	Bytes region[4];
	u8 blk_type = 0x90;   // 0x90: ES5506 (region in bits 28-29 of the start, bit 31 = 8-bit data), 0x97: ZSG-2 (plain offset)

	void add_block(u32 rom_size, u32 start, const u8* data, size_t len)
	{
		int r = (start >> 28) & 3;
		bool is8 = (start >> 31) & 1;
		size_t ofs = start & 0x0FFFFFFF;
		if (blk_type == 0x97) { r = 0; is8 = false; ofs = start; }
		Bytes conv;
		if (is8)
		{
			rom_size *= 2; ofs *= 2;
			conv.resize(len * 2);
			for (size_t i = 0; i < len; i++) { conv[i * 2] = 0; conv[i * 2 + 1] = data[i]; }
			data = conv.data(); len = conv.size();
		}
		if (!has[r] || region[r].size() != rom_size)
		{
			Bytes nb(rom_size, 0);
			if (has[r]) memcpy(nb.data(), region[r].data(), std::min<size_t>(region[r].size(), rom_size));
			region[r].swap(nb);
			has[r] = true;
		}
		if (ofs < rom_size)
			memcpy(&region[r][ofs], data, std::min<size_t>(len, rom_size - ofs));
	}

	// encode the data blocks; used == nullptr: everything
	size_t blocks(Bytes& out, u8 cs, const std::vector<Range>* used, i64 margin_words, i64 merge_gap_words) const
	{
		size_t total = 0;
		for (int r = 0; r < 4; r++)
		{
			if (!has[r]) continue;
			const Bytes& buf = region[r];
			std::vector<std::pair<i64, i64>> ranges;
			if (!used)
				ranges.push_back({0, (i64)buf.size()});
			else
			{
				i64 words = buf.size() / 2;
				std::vector<std::pair<i64, i64>> spans;
				for (const Range& rg : used[r])
					spans.push_back({std::max<i64>(0, rg.first - margin_words), std::min<i64>(words, rg.last + margin_words)});
				std::sort(spans.begin(), spans.end());
				for (auto& s : spans)
				{
					if (!ranges.empty() && s.first <= ranges.back().second + merge_gap_words)
						ranges.back().second = std::max(ranges.back().second, s.second);
					else
						ranges.push_back(s);
				}
				for (auto& rg : ranges) { rg.first *= 2; rg.second *= 2; }
			}
			for (auto& rg : ranges)
			{
				i64 a = rg.first, b = rg.second;
				if (b <= a) continue;
				u32 plen = 8 + (u32)(b - a);
				out.push_back(0x67); out.push_back(0x66); out.push_back(blk_type);
				push_le32(out, plen | (cs ? 0x80000000 : 0));
				push_le32(out, buf.size());
				push_le32(out, blk_type == 0x97 ? (u32)a : (((u32)r << 28) | (u32)a));
				out.insert(out.end(), buf.begin() + a, buf.begin() + b);
				total += b - a;
			}
		}
		return total;
	}
};

// ---------------------------------------------------------------------------------------------
// ES5510 host interface: dead command elimination
// ---------------------------------------------------------------------------------------------
struct EspCmd { size_t tag; u8 sub, ofs, data; };
static const u16 ESP_G = 0x007, ESP_I = 0x1F8;   // latch byte bits: g0-g2, i0-i5

static u16 esp_defs(u8 ofs, u8 data)
{
	if (ofs <= 0x02) return 1 << ofs;
	if (ofs >= 0x03 && ofs <= 0x08) return 1 << ofs;
	if (ofs == 0x80)
	{
		u16 s = 0;
		if (data < 0xA0) s |= ESP_I;
		if (data < 0xC0 || data >= 0xEA) s |= ESP_G;
		return s;
	}
	return 0;
}
static u16 esp_uses(u8 ofs, u8 data)
{
	if (ofs == 0xA0) return ESP_G;
	if (ofs == 0xC0) return data < 0xA0 ? ESP_I : 0;
	if (ofs == 0xE0) return ESP_G | (data < 0xA0 ? ESP_I : 0);
	return 0;
}

// latch byte liveness, backwards over init + body: kept init list, dead body tags
static void esp_liveness(std::vector<EspCmd>& init, const std::vector<EspCmd>& body, std::set<size_t>& dead_body)
{
	size_t n_init = init.size(), n = n_init + body.size();
	std::vector<char> dead(n, 0);
	u16 live = ESP_G | ESP_I;
	for (size_t i = n; i-- > 0;)
	{
		const EspCmd& c = i < n_init ? init[i] : body[i - n_init];
		if (c.sub != 0x00) continue;
		live |= esp_uses(c.ofs, c.data);
		u16 defs = esp_defs(c.ofs, c.data);
		if (c.ofs <= 0x08 || c.ofs == 0x80)
		{
			if (!(defs & live)) { dead[i] = 1; continue; }
			live &= ~defs;
		}
		else if (c.ofs == 0xC0 && c.data >= 0xA0)
			dead[i] = 1;
	}
	std::vector<EspCmd> kept;
	for (size_t i = 0; i < n_init; i++) if (!dead[i]) kept.push_back(init[i]);
	for (size_t i = n_init; i < n; i++) if (dead[i]) dead_body.insert(body[i - n_init].tag);
	init.swap(kept);
}

static void esp_optimise(std::vector<EspCmd>& init, const std::vector<EspCmd>& body, std::set<size_t>& dead_body)
{
	std::set<size_t> scratch;
	esp_liveness(init, body, scratch);
	// last store wins within the init block
	std::vector<char> keep(init.size(), 1);
	std::set<int> gpr_done, instr_done;
	for (size_t i = init.size(); i-- > 0;)
	{
		const EspCmd& c = init[i];
		if (c.sub != 0x00) continue;
		if (c.ofs == 0x80)
		{
			if (c.data < 0xA0) instr_done.erase(c.data);
			if (c.data < 0xC0) gpr_done.erase(c.data);
		}
		else if (c.ofs == 0xA0 && c.data < 0xC0)
		{
			if (gpr_done.count(c.data)) keep[i] = 0;
			gpr_done.insert(c.data);
		}
		else if (c.ofs == 0xC0 && c.data < 0xA0)
		{
			if (instr_done.count(c.data)) keep[i] = 0;
			instr_done.insert(c.data);
		}
		else if (c.ofs == 0xE0 && c.data < 0xC0)
		{
			if (gpr_done.count(c.data) && (c.data >= 0xA0 || instr_done.count(c.data))) keep[i] = 0;
			gpr_done.insert(c.data);
			if (c.data < 0xA0) instr_done.insert(c.data);
		}
	}
	std::vector<EspCmd> kept;
	for (size_t i = 0; i < init.size(); i++) if (keep[i]) kept.push_back(init[i]);
	init.swap(kept);
	esp_liveness(init, body, dead_body);
}

// ---------------------------------------------------------------------------------------------
// trim
// ---------------------------------------------------------------------------------------------
struct TrimStats { i64 kept = 0, dropped = 0, skipped = 0; size_t rom_bytes = 0; };

// skip_from..skip_to: commands of that range are not emitted (the first repetition of a song whose
// loop body is taken from the second one); the state they leave is re-created at the splice, except
// the key-ons of voices that only play there
// --volume: VGM header volume modifier (offset 0x7C, VGM 1.60+): the player applies 2^(n/32) to the output.
// n is -64..192 (0xC1..0xFF code -63..-1, 0xC0 is not used); INT_MIN leaves the header untouched.
static int g_volume_mod = INT_MIN;

// the ES55xx / ZSG-2 extensions stay at VGM 1.72 while nothing is validated upstream; early ZSG-2
// captures were written as 1.73 and are brought back to 1.72 by every output path
static void fix_version(Bytes& out) { if (out.size() >= 0x0C && le32(&out[0x08]) == 0x173) put_le32(&out[0x08], 0x172); }
static int volume_mod_from_db(double db) { return (int)std::lround(db * 32.0 / (20.0 * std::log10(2.0))); }
static double volume_db_from_mod(int n) { return n * (20.0 * std::log10(2.0)) / 32.0; }
static int volume_mod_from_byte(u8 b) { return b <= 0xC0 ? b : (b == 0xC1 ? -64 : (int)b - 0x100); }
static u8 volume_byte_from_mod(int n) { return n >= 0 ? (u8)n : (n <= -64 ? 0xC1 : (u8)(n + 0x100)); }

static Bytes trim(const VGMFile& vgm, i64 start, i64 loop, bool has_loop, i64 end, bool do_cmp, bool do_sro, bool keep_gd3,
                  i64 sro_margin, i64 sro_gap, TrimStats& stats, i64 skip_from = 0, i64 skip_to = 0)
{
	const bool skip = skip_to > skip_from;
	ES55xxState shadow[2] = {ES55xxState(vgm.is_es5506), ES55xxState(vgm.is_es5506)};
	bool shadow_init = false;
	std::vector<EspCmd> esp_splice[2];
	const u8* d = vgm.data.data();
	ES55xxState states[2] = {ES55xxState(vgm.is_es5506), ES55xxState(vgm.is_es5506)};
	states[0].clock = states[1].clock = vgm.es5506_clock & 0x3FFFFFFF;
	Emitter emitters[2] = {Emitter(vgm.is_es5506, 0x00), Emitter(vgm.is_es5506, 0x80)};
	RomSet roms[2];
	Bytes other_blocks, body;
	i64 loop_body_ofs = -1;
	i64 pending_wait = 0;
	bool init_emitted = false;
	std::set<size_t> esp_dead;
	std::vector<EspCmd> esp_init[2];

	if (do_cmp || skip)
	{
		if (do_cmp) emitters[0].lane_opt = emitters[1].lane_opt = true;
		std::vector<EspCmd> body_cmds[2], after_cmds[2];
		vgm.commands([&](const Cmd& c) {
			if (c.op == 0xD8)
			{
				int ci = (d[c.p + 1] & 0x80) ? 1 : 0;
				EspCmd e{c.p, (u8)(d[c.p + 1] & 0x7F), d[c.p + 2], d[c.p + 3]};
				if (c.t < start) esp_init[ci].push_back(e);
				else if (c.t >= end) return false;
				else if (skip && c.t >= skip_from && c.t < skip_to) esp_splice[ci].push_back(e);
				else { body_cmds[ci].push_back(e); if (skip && c.t >= skip_to) after_cmds[ci].push_back(e); }
			}
			return true;
		});
		for (int ci = 0; ci < 2; ci++)
		{
			if (do_cmp) esp_optimise(esp_init[ci], body_cmds[ci], esp_dead);
			if (skip) { std::set<size_t> scratch; esp_optimise(esp_splice[ci], after_cmds[ci], scratch); }
		}
	}

	// A looping file re-enters the loop body with the state of the END of the body, not the state
	// of the first pass. Writes inside the body may only be dropped when they are redundant in
	// both passes, so the second pass is tracked from the end-of-range state (vgm_cmp does the same).
	std::vector<ES55xxState> st_end;
	std::vector<ES55xxState> st2;      // second-pass state, valid once inside the loop body
	bool track2 = false;
	bool keyed_in_body[2][32] = {};    // voice keyed on inside the loop body (after the skipped part)
	if (has_loop)
	{
		st_end.assign({ES55xxState(vgm.is_es5506), ES55xxState(vgm.is_es5506)});
		vgm.commands([&](const Cmd& c) {
			if (c.op == 0x66 || c.t >= end) return false;
			size_t p = c.p;
			if (c.len < 2) return true;
			int ci = (d[p + 1] & 0x80) ? 1 : 0;
			ES55xxState& st = st_end[ci];
			RegWrite rw;
			int n = 0;
			if (c.op == 0xBE) n = st.write8(d[p + 1] & 0x7F, d[p + 2], rw);
			else if (c.op == 0xD6) n = st.write16(d[p + 1] & 0x7F, (d[p + 2] << 8) | d[p + 3], rw);
			if (n && rw.reg == 0 && rw.page < 0x40 && !(rw.value & 3) && c.t >= loop) keyed_in_body[ci][rw.page & 0x1F] = true;
			else if (c.op == 0xD7) st.ext(d[p + 1] & 0x7F, d[p + 2], d[p + 3]);
			else if (c.op == 0xE3) st.gain(d[p + 2], (d[p + 3] << 8) | d[p + 4]);
			return true;
		});
	}

	auto flush_wait = [&]() { if (pending_wait) { wait_cmd(body, pending_wait); pending_wait = 0; } };
	auto emit_init = [&]() {
		if (init_emitted) return;
		init_emitted = true;
		for (int ci = 0; ci < 2; ci++)
		{
			ES55xxState& st = states[ci];
			for (int r = 0; r < 4; r++) st.rom_used[r].clear();
			for (int v = 0; v < 32; v++)
				if ((st.running >> v) & 1) st.note_rom_use(v);
			if (do_cmp)
			{
				st.esp_cmds.clear();
				for (const EspCmd& e : esp_init[ci]) st.esp_cmds.push_back({e.sub, e.ofs, e.data});
			}
			if (st.reg_count || !st.esp_cmds.empty() || !st.gains.empty())
				emitters[ci].init(body, st);
			st.restart_positions(start);   // the image restarts every voice at its host accumulator
		}
	};
	// voices whose state at the loop point (first pass) differs from the state at the end of the
	// loop body (what the second pass starts with)
	auto loop_report = [&]() {
		for (int ci = 0; ci < 2; ci++)
		{
			ES55xxState& a = states[ci];
			ES55xxState& b = st_end[ci];
			if (!a.reg_count && !b.reg_count) continue;
			int sustained = 0, stopped = 0, changed = 0;
			for (int v = 0; v < 32; v++)
			{
				bool r1 = (a.running >> v) & 1, r2 = (b.running >> v) & 1;
				if (r1 && !r2) { sustained++; continue; }
				if (!r1 && r2) { stopped++; continue; }
				if (!r1) continue;
				bool diff = false;
				for (int pg = 0; pg < 2 && !diff; pg++)
					for (int r = 0; r < 16; r++)
					{
						u32 x, y; bool kx = a.get(pg * 0x20 + v, r, x), ky = b.get(pg * 0x20 + v, r, y);
						if (kx != ky || (kx && x != y)) { diff = true; break; }
					}
				if (diff) { changed++; }
			}
			if (sustained || stopped || changed)
				printf("loop point, chip %d: voice state differs between the loop point and the end of the loop body "
				       "(%d voices still playing at the loop point only, %d at the end only, %d with other parameters); "
				       "the second pass converges as the driver re-keys them\n", ci + 1, sustained, stopped, changed);
		}
	};
	// re-create, before the loop point, the state left by the skipped repetition: registers that
	// differ, board state, ESP host writes; a voice that only plays in the skipped part stays stopped
	auto emit_splice = [&]() {
		if (!shadow_init) return;
		for (int ci = 0; ci < 2; ci++)
		{
			ES55xxState& st = states[ci];
			const ES55xxState& sh = shadow[ci];
			if (!sh.reg_count && sh.esp_cmds.empty()) continue;
			// the voices as they play at the end of the skipped repetition, at their current position
			// (a tail of its last bar is heard over the end of the intro, as at every later loop)
			u32 act;
			if (!vgm.is_es5506 && const_cast<ES55xxState&>(sh).get(0, 13, act)) emitters[ci].reg(body, RegWrite{0, 13, act, true, -1, false, 0});
			if (vgm.is_es5506 && const_cast<ES55xxState&>(sh).get(0, 11, act)) emitters[ci].reg(body, RegWrite{0, 11, act, true, -1, false, 0});
			for (const RegWrite& w0 : sh.image())
			{
				if (w0.page < 0x40 && !st.is_global(w0.page, w0.reg)) continue;   // voices come with the image below
				u32 cur; bool kn = st.get(w0.page, w0.reg, cur);
				if (kn && cur == w0.value) continue;
				emitters[ci].reg(body, w0);
			}
			u8 cs = ci ? 0x80 : 0x00;
			for (auto& kv : sh.region_alias) { auto it = st.region_alias.find(kv.first); if (it == st.region_alias.end() || it->second != kv.second) { body.insert(body.end(), {0xD7, u8(0x02 | cs), u8(kv.first), u8(kv.second)}); } }
			for (auto& kv : sh.region_window) { auto it = st.region_window.find(kv.first); if (it == st.region_window.end() || it->second != kv.second) { body.insert(body.end(), {0xD7, u8(0x01 | cs), u8(kv.first), u8(kv.second)}); } }
			for (auto& kv : sh.voice_bank)
			{
				auto it = st.voice_bank.find(kv.first);
				if (it != st.voice_bank.end() && it->second == kv.second) continue;
				if (kv.second <= 0xFF) body.insert(body.end(), {0xD7, u8(0x00 | cs), u8(kv.first), u8(kv.second)});
				else { body.push_back(0xD6); body.push_back(u8(0x40 | kv.first | cs)); push_be16(body, kv.second); }
			}
			if (sh.irq_autoack >= 0 && sh.irq_autoack != st.irq_autoack) body.insert(body.end(), {0xD7, u8(0x04 | cs), 0x00, u8(sh.irq_autoack)});
			for (auto& kv : sh.gains) { auto it = st.gains.find(kv.first); if (it == st.gains.end() || it->second != kv.second) { body.push_back(0xE3); body.push_back(cs); body.push_back(u8(kv.first)); push_be16(body, kv.second); } }
			for (const EspCmd& e : esp_splice[ci]) body.insert(body.end(), &d[e.tag], &d[e.tag + 4]);
			st = sh;   // the loop image that follows places every voice
		}
	};
	auto mark_loop = [&]() {
		if (has_loop && loop_body_ofs < 0)
		{
			flush_wait();
			if (skip) emit_splice();
			loop_body_ofs = body.size();
			emitters[0].out_page = emitters[1].out_page = -1;
			if (!st_end.empty()) { st2 = st_end; track2 = do_cmp; loop_report(); }
			// first thing in the loop body: the full voice state as it plays here in the first pass,
			// so that the second pass starts from the same sound whatever the body left behind
			for (int ci = 0; ci < 2; ci++)
				if (states[ci].reg_count) emitters[ci].voice_image(body, states[ci], loop);
		}
	};

	vgm.commands([&](const Cmd& c) {
		size_t p = c.p;
		u8 op = c.op;
		if (op == 0x66) return false;
		if (op == 0x67)
		{
			if (d[p + 2] == 0x90 && c.len >= 15)
			{
				u32 size = le32(&d[p + 3]);
				int ci = (size & 0x80000000) ? 1 : 0;
				roms[ci].add_block(le32(&d[p + 7]), le32(&d[p + 11]), &d[p + 15], c.len - 15);
			}
			else
				other_blocks.insert(other_blocks.end(), &d[p], &d[p + c.len]);
			return true;
		}
		i64 w = wait_of(&d[p], op);
		if (w)
		{
			i64 a = c.t, b = c.t + w;
			if (b <= start || a >= end) return true;
			emit_init();
			a = std::max(a, start);
			b = std::min(b, end);
			auto add_span = [&](i64 sa, i64 sb) {
				if (has_loop && loop_body_ofs < 0 && sa <= loop && loop < sb)
				{
					pending_wait += loop - sa;
					mark_loop();
					sa = loop;
				}
				pending_wait += sb - sa;
			};
			if (skip)
			{
				if (b > skip_from && !shadow_init) { shadow[0] = states[0]; shadow[1] = states[1]; shadow_init = true; }
				if (std::min(b, skip_from) > a) add_span(a, std::min(b, skip_from));
				if (b > std::max(a, skip_to)) add_span(std::max(a, skip_to), b);
			}
			else
				add_span(a, b);
			return true;
		}
		int ci = (c.len > 1 && (d[p + 1] & 0x80)) ? 1 : 0;
		const bool in_skip = skip && c.t >= skip_from && c.t < skip_to;
		if (in_skip && !shadow_init) { shadow[0] = states[0]; shadow[1] = states[1]; shadow_init = true; }
		ES55xxState& st = in_skip ? shadow[ci] : states[ci];
		st.now = c.t;
		Emitter& em = emitters[ci];
		RegWrite rw, rw2;
		int nwrites = -1;      // -1: not a register write
		bool raw_changed = true;
		bool in_body = track2 && c.t >= start && c.t < end;
		if (op == 0xBE)
		{
			nwrites = st.write8(d[p + 1] & 0x7F, d[p + 2], rw);
			if (in_body) st2[ci].write8(d[p + 1] & 0x7F, d[p + 2], rw2);
		}
		else if (op == 0xD6)
		{
			nwrites = st.write16(d[p + 1] & 0x7F, (d[p + 2] << 8) | d[p + 3], rw);
			if (in_body) st2[ci].write16(d[p + 1] & 0x7F, (d[p + 2] << 8) | d[p + 3], rw2);
		}
		else if (op == 0xD7)
		{
			raw_changed = st.ext(d[p + 1] & 0x7F, d[p + 2], d[p + 3]) || (d[p + 1] & 0x7F) == 0x03;
			if (in_body) raw_changed |= st2[ci].ext(d[p + 1] & 0x7F, d[p + 2], d[p + 3]);
		}
		else if (op == 0xD8)
		{
			st.esp_cmds.push_back({(u8)(d[p + 1] & 0x7F), d[p + 2], d[p + 3]});
			raw_changed = esp_dead.count(p) == 0;
		}
		else if (op == 0xE3)
		{
			raw_changed = st.gain(d[p + 2], (d[p + 3] << 8) | d[p + 4]);
			if (in_body) raw_changed |= st2[ci].gain(d[p + 2], (d[p + 3] << 8) | d[p + 4]);
		}
		if (in_body && nwrites > 0 && rw.reg != PAGE_REG)
		{
			// keep the write when it changes the register in the second pass; a byte-lane write
			// becomes a full write when the other lane differs in the second pass (if that lane is
			// host-owned); lane_opt may only skip a lane that is equal in both passes
			if (rw2.changed) rw.changed = true;
			if (!rw2.has_prev || rw2.prev != rw.prev) rw.has_prev = false;
			if (rw.lane >= 0)
			{
				u32 om = (rw.lane == 0 ? 0x00FF : 0xFF00) & st.mask(rw.page, rw.reg);
				bool other_differs = !rw2.has_prev || ((rw2.prev ^ rw.value) & om) != 0;
				if (other_differs && Emitter::lane_trusted(page_group(rw.page), rw.reg, 1 - rw.lane)) rw.lane = -1;
			}
		}
		if (c.t < start || in_skip) { stats.skipped++; return true; }
		if (c.t >= end) return true;
		emit_init();
		if (has_loop && loop_body_ofs < 0 && c.t >= loop) mark_loop();
		if (nwrites >= 0)
		{
			if (nwrites == 0)
			{
				if ((d[p + 1] & 0x7F) >= 0x40)
				{
					flush_wait();
					body.insert(body.end(), &d[p], &d[p + c.len]);   // legacy voice bank write
					stats.kept++;
				}
				return true;   // partial ES5506 latch
			}
			if (rw.reg == PAGE_REG) return true;
			if (do_cmp && !rw.changed && !st.side_effect(rw.page, rw.reg)) { stats.dropped++; return true; }
			flush_wait();
			em.reg(body, rw);
			stats.kept++;
			return true;
		}
		if (do_cmp && !raw_changed) { stats.dropped++; return true; }
		flush_wait();
		body.insert(body.end(), &d[p], &d[p + c.len]);
		stats.kept++;
		return true;
	});
	flush_wait();
	if (has_loop && loop_body_ofs < 0) loop_body_ofs = body.size();
	body.push_back(0x66);

	Bytes blocks(other_blocks);
	stats.rom_bytes = 0;
	for (int ci = 0; ci < 2; ci++)
		stats.rom_bytes += roms[ci].blocks(blocks, ci ? 0x80 : 0x00, do_sro ? states[ci].rom_used : nullptr, sro_margin, sro_gap);

	Bytes out(vgm.data.begin(), vgm.data.begin() + vgm.data_ofs);
	// files using the ES55xx extension are VGM 1.72 with a 0x100-byte header; upgrade older captures
	if (vgm.data_ofs >= 0xD4 && le32(&out[0xD0]) != 0) {
		if (le32(&out[0x08]) < 0x172) put_le32(&out[0x08], 0x172);
		if (out.size() < 0x100) { out.resize(0x100, 0); put_le32(&out[0x34], 0x100 - 0x34); }
	}
	fix_version(out);
	if (g_volume_mod != INT_MIN)
	{
		// the volume modifier needs the VGM 1.60 header (0x80 bytes)
		if (le32(&out[0x08]) < 0x160) put_le32(&out[0x08], 0x160);
		if (out.size() < 0x80) { out.resize(0x80, 0); put_le32(&out[0x34], 0x80 - 0x34); }
		out[0x7C] = volume_byte_from_mod(g_volume_mod);
	}
	size_t data_start = out.size();
	put_le32(&out[0x18], (u32)(end - start - (skip ? skip_to - skip_from : 0)));
	if (has_loop)
	{
		put_le32(&out[0x1C], (u32)(data_start + blocks.size() + loop_body_ofs - 0x1C));
		put_le32(&out[0x20], (u32)(end - loop));
	}
	else
	{
		put_le32(&out[0x1C], 0);
		put_le32(&out[0x20], 0);
	}
	out.insert(out.end(), blocks.begin(), blocks.end());
	out.insert(out.end(), body.begin(), body.end());
	Bytes gd3;
	if (keep_gd3 && vgm.gd3 && vgm.gd3 + 12 <= vgm.data.size())
	{
		u32 gd3_len = le32(&d[vgm.gd3 + 8]);
		size_t e = std::min<size_t>(vgm.gd3 + 12 + gd3_len, vgm.data.size());
		gd3.assign(&d[vgm.gd3], &d[e]);
	}
	put_le32(&out[0x14], gd3.empty() ? 0 : (u32)(out.size() - 0x14));
	out.insert(out.end(), gd3.begin(), gd3.end());
	put_le32(&out[0x04], (u32)(out.size() - 4));
	return out;
}

// ---------------------------------------------------------------------------------------------
// info / activity
// ---------------------------------------------------------------------------------------------
static void info(const VGMFile& vgm)
{
	std::map<int, i64> counts;
	std::string blocks;
	std::set<int> other;
	vgm.commands([&](const Cmd& c) {
		counts[c.op]++;
		if (c.op == 0x67)
		{
			char buf[96];
			snprintf(buf, sizeof(buf), "%stype %02X %zu bytes @%s", blocks.empty() ? "" : ", ", vgm.data[c.p + 2], c.len - 7, fmt_pos(c.t).c_str());
			blocks += buf;
		}
		u8 op = c.op;
		if ((op >= 0x50 && op <= 0x5F) || (op >= 0x30 && op <= 0x4E) ||
		    (op >= 0xA0 && op <= 0xDF && op != 0xBE && op != 0xD6 && op != 0xD7 && op != 0xD8) || op == 0xE0 || op == 0xE1)
			other.insert(op);
		return true;
	});
	printf("version %X, total %s, loop %s\n", vgm.version, fmt_pos(vgm.total).c_str(), vgm.loop_ofs ? fmt_pos(vgm.loop_samples).c_str() : "none");
	if (vgm.version >= 0x160 && vgm.data_ofs > 0x7C && vgm.data[0x7C] != 0)
	{
		int n = volume_mod_from_byte(vgm.data[0x7C]);
		printf("volume modifier %d (%+.2f dB, x%.3f)\n", n, volume_db_from_mod(n), std::pow(2.0, n / 32.0));
	}
	if (vgm.is_zsg2) printf("ZSG-2 clock %u Hz (%u Hz output, TMS57002 DSP at %u Hz)\n", vgm.zsg2_clock, vgm.zsg2_clock / 768, vgm.zsg2_clock / 2);
	else printf("ES5506 clock %08X (%s, %d pair(s))\n", vgm.es5506_clock, vgm.is_es5506 ? "ES5506" : "ES5505", vgm.data.size() > 0xD5 ? vgm.data[0xD5] : 0);
	printf("data blocks: %s\n", blocks.c_str());
	std::string cmds;
	for (auto& kv : counts) { char buf[32]; snprintf(buf, sizeof(buf), "%s%02X:%lld", cmds.empty() ? "" : ", ", kv.first, (long long)kv.second); cmds += buf; }
	printf("commands: %s\n", cmds.c_str());
	if (!other.empty())
	{
		std::string s;
		for (int o : other) { char buf[8]; snprintf(buf, sizeof(buf), "%s%02X", s.empty() ? "" : " ", o); s += buf; }
		printf("WARNING: commands of other chips present (%s), their state is not re-created by trimming\n", s.c_str());
	}
}

// gaps where no voice is audible (running with a non-zero volume)
static std::vector<std::pair<i64, i64>> zsg2_activity(const VGMFile& vgm, i64 min_gap, bool quiet);
static std::vector<std::pair<i64, i64>> activity(const VGMFile& vgm, i64 min_gap, bool quiet)
{
	if (vgm.is_zsg2) return zsg2_activity(vgm, min_gap, quiet);
	const u8* d = vgm.data.data();
	ES55xxState states[2] = {ES55xxState(vgm.is_es5506), ES55xxState(vgm.is_es5506)};
	std::map<i64, int> per_sec;
	std::vector<std::pair<i64, i64>> gaps;
	i64 gap_start = -1, last_t = 0;
	vgm.commands([&](const Cmd& c) {
		if (c.op == 0x66) return false;
		int ci = (c.len > 1 && (d[c.p + 1] & 0x80)) ? 1 : 0;
		RegWrite rw;
		if (c.op == 0xBE) states[ci].write8(d[c.p + 1] & 0x7F, d[c.p + 2], rw);
		else if (c.op == 0xD6) states[ci].write16(d[c.p + 1] & 0x7F, (d[c.p + 2] << 8) | d[c.p + 3], rw);
		int n = 0;
		for (int k = 0; k < 2; k++)
		{
			ES55xxState& st = states[k];
			for (int v = 0; v < 32; v++)
			{
				if (!((st.running >> v) & 1)) continue;
				u32 l = st.known[v][8] ? st.regs[v][8] : 0, r = st.known[v][9] ? st.regs[v][9] : 0;
				if ((l | r) & 0xFF00) n++;
			}
		}
		if (!quiet)
		{
			i64 sec = c.t / SAMPLE_RATE;
			auto it = per_sec.find(sec);
			if (it == per_sec.end()) per_sec[sec] = n; else it->second = std::max(it->second, n);
		}
		if (n == 0)
		{
			if (gap_start < 0) gap_start = c.t;
		}
		else
		{
			// the silence before the first note always ends a (leading) gap, whatever its length
			if (gap_start >= 0 && (c.t - gap_start >= min_gap || gaps.empty())) gaps.push_back({gap_start, c.t});
			gap_start = -1;
		}
		last_t = c.t;
		return true;
	});
	if (gap_start >= 0 && last_t - gap_start >= min_gap) gaps.push_back({gap_start, last_t});
	if (quiet) return gaps;
	printf("running voices per second (max):\n");
	int col = 0;
	for (auto& kv : per_sec)
	{
		printf("%s%3lld:%2d", col == 0 ? "  " : " ", (long long)kv.first, kv.second);
		if (++col == 12) { printf("\n"); col = 0; }
	}
	if (col) printf("\n");
	printf("gaps with no running voice (>= %s):\n", fmt_pos(min_gap).c_str());
	for (auto& g : gaps)
		printf("  %s .. %s (%.2f s)\n", fmt_pos(g.first).c_str(), fmt_pos(g.second).c_str(), (double)(g.second - g.first) / SAMPLE_RATE);
	return gaps;
}

// ---------------------------------------------------------------------------------------------
// loop detection from the note-on sequence
// ---------------------------------------------------------------------------------------------
struct NoteKey
{
	i64 a, b, c, f;
	bool operator==(const NoteKey& o) const { return a == o.a && b == o.b && c == o.c && f == o.f; }
	bool operator<(const NoteKey& o) const
	{
		if (a != o.a) return a < o.a;
		if (b != o.b) return b < o.b;
		if (c != o.c) return c < o.c;
		return f < o.f;
	}
};
struct NoteEvent { i64 t; NoteKey k; };

struct Wav;
static double audio_corr(const Wav& w, i64 a, i64 b, i64 n, i64 max_lag = 2000);

// ---------------------------------------------------------------------------------------------
// ZOOM ZSG-2 + TMS57002 (Taito Zoom board): command 0xE4 ss aa dddd, ROM data block 0x97
// ---------------------------------------------------------------------------------------------
// TMS57002 control lines (bit 0 = PLOAD, bit 1 = CLOAD, active low). The program, status registers and
// coefficients are not modelled: the coefficient updates the sound CPU sends through CLOAD land where the
// running program reads them, so the DSP state at a cut is re-created by replaying every host write from
// the start of the capture, with the time between them replaced by DSP pre-runs (command 0xE4 sub 8).
struct TmsHost
{
	u8 ctrl = 3;
	bool ctrl_known = false, loaded = false;
	bool control(u8 bits)
	{
		bits &= 3;
		bool changed = !ctrl_known || bits != ctrl;
		if (!(bits & 1)) loaded = true;
		ctrl = bits; ctrl_known = true;
		return changed;
	}
};
struct DspCmd { i64 t; u8 sub, aa; u16 data; };

struct ZSG2Voice
{
	u16 v[16] = {};
	bool known[16] = {};
	bool active = false;
	// playback model: the ZSG-2 has no readable position, it is computed from the key-on time
	i64 t_on = 0;               // VGM time of the key-on
	double acc = 0;             // step_ptr accumulator (1/65536 block units, starts at 0x10000 at key-on)
	double chip_frac = 0;       // fractional chip sample carried between events
	i64 last_t = 0;             // VGM time the model was advanced to
	u32 start = 0;              // start position latched at key-on (cur_pos starts at start - 1)
	u16 vol = 0, vol_target = 0, cutoff = 0, cutoff_target = 0, cutoff_initial = 0;
	i16 vol_delta = 0, cutoff_delta = 0;
	bool keyon_delta = false;   // vol_delta = 0x400 from the key-on, not overwritten by register 0xF yet
	double ramp_frac = 0;       // ramp steps happen every other chip sample
};

static i16 zsg2_get_ramp(u8 val)
{
	i16 frac = (i16)(val << 12);
	frac = ((frac >> 12) ^ 8) << (val >> 4);
	return (frac >> 4);
}

struct ZSG2State
{
	ZSG2Voice voice[48];
	u16 creg[32] = {};
	bool creg_known[32] = {};
	std::map<int, u16> gains;
	TmsHost tms;
	std::vector<DspCmd> dsp_hist;   // host writes, control lines, resets and pre-runs of the DSP since the start
	i64 now = 0;
	double chip_rate = 25000000.0 / 768;   // samples per second
	std::vector<Range> rom_used;           // words (2 bytes), from the key-ons
	bool reg_count = false;                // anything known

	void set_clock(u32 clock) { if (clock) chip_rate = (double)(clock / 768); }
	double chip_samples(i64 dt) const { return dt * chip_rate / SAMPLE_RATE; }

	static u16 ramp(u16 current, u16 target, i16 delta, double steps)
	{
		if (steps < 1) return current;
		if (delta >= 0)
		{
			if (current > target) return target;
			double r = current + (double)delta * std::floor(steps);
			return r > target ? target : (u16)r;
		}
		if (current < target) return target;
		double r = current + (double)delta * std::floor(steps);
		return r < target ? target : (u16)r;
	}
	// advance the playback model of a voice to time t
	void advance(int vi, i64 t)
	{
		ZSG2Voice& w = voice[vi];
		if (t <= w.last_t) return;
		double n = chip_samples(t - w.last_t) + w.chip_frac;
		double whole = std::floor(n);
		w.chip_frac = n - whole;
		w.last_t = t;
		if (!w.active) return;
		u32 step = (u32)w.v[4] + 1;
		w.acc += whole * step;
		double steps = whole / 2 + w.ramp_frac;
		w.ramp_frac = steps - std::floor(steps);
		w.vol = ramp(w.vol, w.vol_target, w.vol_delta, steps);
		w.cutoff = ramp(w.cutoff, w.cutoff_target, w.cutoff_delta, steps);
	}
	// current block position of an active voice; false when the sample has ended
	bool position(int vi, u32& pos) const
	{
		const ZSG2Voice& w = voice[vi];
		if (!w.active) return false;
		u32 end = w.v[6], loop = (w.v[5] & 0xFF) | ((w.v[7] & 0xFF) << 8);
		i64 inc = (i64)(w.acc / 65536.0);
		i64 p = (i64)w.start - 1 + inc;
		if (p >= end)
		{
			if ((i64)loop + 1 >= end) return false;
			p = loop + (p - end) % ((i64)end - loop);
		}
		pos = (u32)p;
		return true;
	}
	bool playing(int vi, i64 t)
	{
		advance(vi, t);
		u32 p;
		if (!position(vi, p)) { voice[vi].active = false; return false; }
		return voice[vi].vol_target > 0 || voice[vi].vol > 0;
	}
	void note_rom_use(int vi)
	{
		const ZSG2Voice& w = voice[vi];
		if (!w.known[0] || !w.known[1] || !w.known[6]) return;
		u32 page = (u32)(w.v[1] & 0xFF00) << 8;
		u32 s = page | ((w.v[0] >> 8) | ((w.v[1] & 0xFF) << 8)), e = page | w.v[6];
		if (e <= s) e = s + 1;
		rom_used.push_back({(i64)s * 2, (i64)e * 2 + 2});
	}
	// a register / control write; returns true when the chip state changes (false = redundant)
	bool write(u16 ofs, u16 data)
	{
		reg_count = true;
		if (ofs < 0x300)
		{
			int vi = ofs >> 4, reg = ofs & 15;
			ZSG2Voice& w = voice[vi];
			advance(vi, now);
			bool same = w.known[reg] && w.v[reg] == data;
			w.v[reg] = data; w.known[reg] = true;
			switch (reg)
			{
			case 0x8: w.cutoff_initial = data; break;
			case 0x9: w.cutoff = data; return true;        // current value: side effect
			case 0xB: w.vol = data; return true;
			case 0xC: w.cutoff_target = data; break;
			case 0xD: w.cutoff_delta = zsg2_get_ramp(data & 0xFF); break;
			case 0xE: w.vol_target = data; break;
			case 0xF: { bool kd = w.keyon_delta; w.vol_delta = zsg2_get_ramp(data & 0xFF); w.keyon_delta = false; if (kd) return true; break; }
			default: break;
			}
			return !same;
		}
		int reg = ofs - 0x300;
		if (reg <= 2 || (reg >= 4 && reg <= 6))
		{
			int base = (reg & 3) << 4;
			for (int i = 0; i < 16; i++)
			{
				if (!(data & (1 << i))) continue;
				ZSG2Voice& w = voice[base | i];
				advance(base | i, now);
				if (reg <= 2)
				{
					w.active = true; w.t_on = now; w.acc = 65536.0; w.chip_frac = 0; w.ramp_frac = 0; w.last_t = now;
					w.start = (w.v[0] >> 8) | ((w.v[1] & 0xFF) << 8);
					w.vol = 0; w.vol_delta = 0x400; w.keyon_delta = true;
					w.cutoff = w.cutoff_initial;
					note_rom_use(base | i);
				}
				else { w.active = false; w.vol = 0; }
			}
			return data != 0;
		}
		if (reg < 32)
		{
			bool same = creg_known[reg] && creg[reg] == data;
			creg[reg] = data; creg_known[reg] = true;
			return !same;
		}
		return true;
	}
	bool ext(u8 sub, u8 aa, u16 data)
	{
		reg_count = true;
		switch (sub)
		{
		case 4: dsp_hist.push_back({now, sub, aa, data}); return true;
		case 5: dsp_hist.push_back({now, sub, aa, data}); return tms.control(data & 0xFF);
		case 8: dsp_hist.push_back({now, sub, aa, data}); return true;
		case 6: { auto it = gains.find(aa); bool same = it != gains.end() && it->second == data; gains[aa] = data; return !same; }
		case 7: dsp_hist.push_back({now, sub, aa, data}); for (ZSG2Voice& w : voice) { w.active = false; w.vol = 0; } return true;
		default: return true;
		}
	}
	// re-create the state at time `at`: gains, DSP (program upload unless `dsp_diff` gives the state the
	// player already has, then only the coefficients that differ are updated), control registers,
	// every known voice register, and the voices still playing re-keyed at their current position
	void image(Bytes& out, i64 at, const ZSG2State* dsp_diff, int& rekeyed, int& lost)
	{
		auto raw = [&](u8 sub, u8 aa, u16 d) { out.insert(out.end(), {0xE4, sub, aa, u8(d >> 8), u8(d & 0xFF)}); };
		auto regw = [&](u16 ofs, u16 d) { raw(u8((ofs >> 8) & 3), u8(ofs & 0xFF), d); };
		for (auto& kv : gains) raw(6, u8(kv.first), kv.second);
		if (dsp_diff == nullptr && !dsp_hist.empty())
		{
			// replay the DSP history; the time between the writes becomes pre-runs of the program (silent input),
			// which re-creates the coefficient memory and the program-internal state (LFO phases...)
			i64 base_t = dsp_hist[0].t, emitted = 0;
			auto prerun_to = [&](i64 t) {
				i64 n = (i64)chip_samples(t - base_t) - emitted;
				while (n > 0) { i64 k = std::min<i64>(n, 0xFFFFFF); raw(8, u8(k >> 16), u16(k & 0xFFFF)); emitted += k; n -= k; }
			};
			for (const DspCmd& c : dsp_hist) { prerun_to(c.t); raw(c.sub, c.aa, c.data); }
			prerun_to(at);
		}
		for (int r = 8; r < 32; r++) if (creg_known[r]) regw(0x300 + r, creg[r]);
		for (int vi = 0; vi < 48; vi++)
			for (int r = 0; r < 16; r++)
				if (voice[vi].known[r]) regw(vi * 16 + r, voice[vi].v[r]);
		rekeyed = lost = 0;
		for (int vi = 0; vi < 48; vi++)
		{
			ZSG2Voice& w = voice[vi];
			if (!w.active) continue;
			advance(vi, at);
			u32 pos;
			if (!position(vi, pos)) { w.active = false; continue; }
			if (!w.known[0] || !w.known[1]) { lost++; continue; }
			// key on at the current position, then put the real start address back
			regw(vi * 16 + 0, u16(((pos & 0xFF) << 8) | (w.v[0] & 0xFF)));
			regw(vi * 16 + 1, u16((w.v[1] & 0xFF00) | ((pos >> 8) & 0xFF)));
			regw(0x300 + (vi >> 4), u16(1 << (vi & 15)));
			regw(vi * 16 + 0, w.v[0]);
			regw(vi * 16 + 1, w.v[1]);
			regw(vi * 16 + 0xB, w.vol);
			regw(vi * 16 + 0x9, w.cutoff);
			if (!w.keyon_delta && w.known[0xF]) regw(vi * 16 + 0xF, w.v[0xF]);
			rekeyed++;
		}
	}
};

// note-on events of a ZSG-2 log (for the loop finder): key-on with the sample of the voice
static void zsg2_notes(const VGMFile& vgm, bool bounded, i64 search_to, std::vector<NoteEvent>& events)
{
	const u8* d = vgm.data.data();
	ZSG2State st;
	st.set_clock(vgm.zsg2_clock);
	vgm.commands([&](const Cmd& c) {
		if (c.op == 0x66 || (bounded && c.t >= search_to)) return false;
		if (c.op != 0xE4 || (d[c.p + 1] & 0x0F) > 3) return true;
		u16 ofs = ((d[c.p + 1] & 3) << 8) | d[c.p + 2], data = (d[c.p + 3] << 8) | d[c.p + 4];
		st.now = c.t;
		if (ofs >= 0x300 && ofs <= 0x302 && data)
		{
			int base = (ofs - 0x300) << 4;
			for (int i = 0; i < 16; i++)
			{
				if (!(data & (1 << i))) continue;
				const ZSG2Voice& w = st.voice[base | i];
				i64 start = ((u32)(w.v[1] & 0xFF00) << 8) | (w.v[0] >> 8) | ((w.v[1] & 0xFF) << 8);
				i64 loop = (w.v[5] & 0xFF) | ((w.v[7] & 0xFF) << 8);
				events.push_back({c.t, NoteKey{start, w.v[6], loop, (i64)w.v[4]}});
			}
		}
		st.write(ofs, data);
		return true;
	});
}

// gaps where no ZSG-2 voice is audible (keyed on, not ended, with a non-zero volume)
static std::vector<std::pair<i64, i64>> zsg2_activity(const VGMFile& vgm, i64 min_gap, bool quiet)
{
	const u8* d = vgm.data.data();
	ZSG2State st;
	st.set_clock(vgm.zsg2_clock);
	std::map<i64, int> per_sec;
	std::vector<std::pair<i64, i64>> gaps;
	i64 gap_start = -1, last_t = 0;
	auto sample = [&](i64 t) {
		int n = 0;
		for (int vi = 0; vi < 48; vi++) if (st.playing(vi, t)) n++;
		if (!quiet)
		{
			i64 sec = t / SAMPLE_RATE;
			auto it = per_sec.find(sec);
			if (it == per_sec.end()) per_sec[sec] = n; else it->second = std::max(it->second, n);
		}
		if (n == 0) { if (gap_start < 0) gap_start = t; }
		else if (gap_start >= 0)
		{
			if (t - gap_start >= min_gap || gaps.empty()) gaps.push_back({gap_start, t});
			gap_start = -1;
		}
		last_t = t;
	};
	i64 next_check = 0;
	vgm.commands([&](const Cmd& c) {
		if (c.op == 0x66) return false;
		// sample the state at least every 100 ms (voices end on their own)
		while (next_check < c.t) { sample(next_check); next_check += SAMPLE_RATE / 10; }
		if (c.op == 0xE4)
		{
			u8 sub = d[c.p + 1] & 0x0F;
			u16 data = (d[c.p + 3] << 8) | d[c.p + 4];
			st.now = c.t;
			if (sub <= 3) st.write(((sub & 3) << 8) | d[c.p + 2], data);
			else st.ext(sub, d[c.p + 2], data);
			sample(c.t);
		}
		return true;
	});
	if (gap_start >= 0 && (i64)vgm.total - gap_start >= min_gap) gaps.push_back({gap_start, (i64)vgm.total});
	if (!quiet)
	{
		for (auto& kv : per_sec) printf("%s: %d voice(s)\n", fmt_pos(kv.first * SAMPLE_RATE).c_str(), kv.second);
		for (auto& g : gaps) printf("gap %s .. %s (%.2f s)\n", fmt_pos(g.first).c_str(), fmt_pos(g.second).c_str(), (double)(g.second - g.first) / SAMPLE_RATE);
	}
	(void)last_t;
	return gaps;
}

// trim a ZSG-2 log: the state at `start` is re-created (gains, DSP program and coefficients, registers,
// playing voices re-keyed at their computed position), the loop body gets a voice image at the loop point
// and the coefficient updates the second pass needs; --cmp drops writes that are redundant in both passes
static Bytes trim_zsg2(const VGMFile& vgm, i64 start, i64 loop, bool has_loop, i64 end, bool do_cmp, bool do_sro, bool keep_gd3,
                       i64 sro_margin, i64 sro_gap, TrimStats& stats)
{
	const u8* d = vgm.data.data();
	ZSG2State st;
	st.set_clock(vgm.zsg2_clock);
	RomSet rom;
	rom.blk_type = 0x97;
	Bytes other_blocks, body;
	i64 loop_body_ofs = -1, pending_wait = 0;
	bool init_emitted = false;

	// state at the end of the loop body (what the second pass starts with)
	std::unique_ptr<ZSG2State> st_end, st2;
	bool track2 = false;
	if (has_loop)
	{
		st_end.reset(new ZSG2State());
		st_end->set_clock(vgm.zsg2_clock);
		vgm.commands([&](const Cmd& c) {
			if (c.op == 0x66 || c.t >= end) return false;
			if (c.op != 0xE4) return true;
			u8 sub = d[c.p + 1] & 0x0F;
			u16 data = (d[c.p + 3] << 8) | d[c.p + 4];
			st_end->now = c.t;
			if (sub <= 3) st_end->write(((sub & 3) << 8) | d[c.p + 2], data);
			else st_end->ext(sub, d[c.p + 2], data);
			return true;
		});
	}

	auto flush_wait = [&]() { if (pending_wait) { wait_cmd(body, pending_wait); pending_wait = 0; } };
	auto emit_init = [&]() {
		if (init_emitted) return;
		init_emitted = true;
		st.rom_used.clear();
		for (int vi = 0; vi < 48; vi++) if (st.voice[vi].active) st.note_rom_use(vi);
		if (!st.reg_count) return;
		int rekeyed, lost;
		st.image(body, start, nullptr, rekeyed, lost);
		if (rekeyed || lost)
			printf("start %s: %d voice(s) re-keyed at their computed position%s\n", fmt_pos(start).c_str(), rekeyed,
			       lost ? (", " + std::to_string(lost) + " lost (sample address unknown)").c_str() : "");
	};
	auto mark_loop = [&]() {
		if (has_loop && loop_body_ofs < 0)
		{
			flush_wait();
			loop_body_ofs = body.size();
			if (st_end) { st2.reset(new ZSG2State(*st_end)); track2 = do_cmp; }
			int rekeyed, lost;
			st.image(body, loop, st_end.get(), rekeyed, lost);
			if (rekeyed || lost)
				printf("loop point %s: %d voice(s) re-keyed at their computed position%s\n", fmt_pos(loop).c_str(), rekeyed,
				       lost ? (", " + std::to_string(lost) + " lost (sample address unknown)").c_str() : "");
		}
	};

	vgm.commands([&](const Cmd& c) {
		size_t p = c.p;
		u8 op = c.op;
		if (op == 0x66) return false;
		if (op == 0x67)
		{
			if (d[p + 2] == 0x97 && c.len >= 15)
				rom.add_block(le32(&d[p + 7]), le32(&d[p + 11]), &d[p + 15], c.len - 15);
			else
				other_blocks.insert(other_blocks.end(), &d[p], &d[p + c.len]);
			return true;
		}
		i64 w = wait_of(&d[p], op);
		if (w)
		{
			i64 a = c.t, b = c.t + w;
			if (b <= start || a >= end) return true;
			emit_init();
			a = std::max(a, start);
			b = std::min(b, end);
			if (has_loop && loop_body_ofs < 0 && a <= loop && loop < b)
			{
				pending_wait += loop - a;
				mark_loop();
				a = loop;
			}
			pending_wait += b - a;
			return true;
		}
		bool changed = true;
		if (op == 0xE4)
		{
			u8 sub = d[p + 1] & 0x0F;
			u16 data = (d[p + 3] << 8) | d[p + 4];
			bool in_body = track2 && c.t >= start && c.t < end;
			st.now = c.t;
			if (sub <= 3)
			{
				u16 ofs = ((sub & 3) << 8) | d[p + 2];
				changed = st.write(ofs, data);
				if (in_body) { st2->now = c.t; changed |= st2->write(ofs, data); }
			}
			else
			{
				changed = st.ext(sub, d[p + 2], data);
				if (in_body) { st2->now = c.t; changed |= st2->ext(sub, d[p + 2], data); }
			}
		}
		if (c.t < start) { stats.skipped++; return true; }
		if (c.t >= end) return true;
		emit_init();
		if (has_loop && loop_body_ofs < 0 && c.t >= loop) mark_loop();
		if (do_cmp && !changed) { stats.dropped++; return true; }
		flush_wait();
		body.insert(body.end(), &d[p], &d[p + c.len]);
		stats.kept++;
		return true;
	});
	flush_wait();
	if (has_loop && loop_body_ofs < 0) loop_body_ofs = body.size();
	body.push_back(0x66);

	Bytes blocks(other_blocks);
	stats.rom_bytes = rom.blocks(blocks, 0x00, do_sro ? &st.rom_used : nullptr, sro_margin, sro_gap);

	Bytes out(vgm.data.begin(), vgm.data.begin() + vgm.data_ofs);
	fix_version(out);
	if (g_volume_mod != INT_MIN) out[0x7C] = volume_byte_from_mod(g_volume_mod);
	size_t data_start = out.size();
	put_le32(&out[0x18], (u32)(end - start));
	if (has_loop)
	{
		put_le32(&out[0x1C], (u32)(data_start + blocks.size() + loop_body_ofs - 0x1C));
		put_le32(&out[0x20], (u32)(end - loop));
	}
	else { put_le32(&out[0x1C], 0); put_le32(&out[0x20], 0); }
	out.insert(out.end(), blocks.begin(), blocks.end());
	out.insert(out.end(), body.begin(), body.end());
	Bytes gd3;
	if (keep_gd3 && vgm.gd3 && vgm.gd3 + 12 <= vgm.data.size())
	{
		u32 gd3_len = le32(&d[vgm.gd3 + 8]);
		size_t e = std::min<size_t>(vgm.gd3 + 12 + gd3_len, vgm.data.size());
		gd3.assign(&d[vgm.gd3], &d[e]);
	}
	put_le32(&out[0x14], gd3.empty() ? 0 : (u32)(out.size() - 0x14));
	out.insert(out.end(), gd3.begin(), gd3.end());
	put_le32(&out[0x04], (u32)(out.size() - 4));
	return out;
}


static int g_loop_pass = 0;   // --loop-pass: 0 = auto, 1 = loop body is the first repetition (file = intro + 1 repetition), 2 = the second one (seamless, file = intro + 2 repetitions)
struct LoopResult { i64 start, loop1, end1, loop2, end2; bool has2; };
static bool find_loop(const VGMFile& vgm, i64 search_from, i64 search_to, bool bounded, LoopResult& lr,
                      const Wav* wav = nullptr, int window = 12, i64 pre = 2205)
{
	const u8* d = vgm.data.data();
	ES55xxState st(vgm.is_es5506);
	std::vector<NoteEvent> events;
	if (vgm.is_zsg2) zsg2_notes(vgm, bounded, search_to, events);
	else vgm.commands([&](const Cmd& c) {
		if (c.op == 0x66 || (bounded && c.t >= search_to)) return false;
		RegWrite w;
		int n;
		if (c.op == 0xBE) n = st.write8(d[c.p + 1] & 0x7F, d[c.p + 2], w);
		else if (c.op == 0xD6) n = st.write16(d[c.p + 1] & 0x7F, (d[c.p + 2] << 8) | d[c.p + 3], w);
		else return true;
		if (n && w.reg == 0 && w.page < 0x40 && !(w.value & 3) && w.changed)
		{
			int v = w.page & 0x1F;
			int region; i64 first, last;
			if (st.voice_range(v, region, first, last))
			{
				u32 fc = 0;
				st.get(v, 1, fc);
				events.push_back({c.t, NoteKey{region, first, last, fc}});
			}
		}
		return true;
	});
	size_t n = events.size();
	if (n < (size_t)window * 2) return false;

	auto window_hash = [&](size_t i) {
		uint64_t h = 1469598103934665603ULL;
		for (int k = 0; k < window; k++)
		{
			const NoteKey& key = events[i + k].k;
			i64 parts[4] = {key.a, key.b, key.c, key.f};
			for (i64 v : parts) { h ^= (uint64_t)v; h *= 1099511628211ULL; }
		}
		return h;
	};
	auto window_equal = [&](size_t i, size_t j) {
		for (int k = 0; k < window; k++) if (!(events[i + k].k == events[j + k].k)) return false;
		return true;
	};
	std::unordered_map<uint64_t, std::vector<size_t>> index;
	for (size_t i = 0; i + window < n; i++)
		if (events[i].t >= search_from)
			index[window_hash(i)].push_back(i);
	std::map<i64, i64> periods;         // bucket (0.5 s) -> count
	std::vector<i64> period_order;      // first-seen order of the buckets
	struct Pair { size_t i, j; i64 dt; };
	std::vector<Pair> pairs;
	for (size_t j = 0; j + window < n; j++)
	{
		auto it = index.find(window_hash(j));
		if (it == index.end()) continue;
		for (size_t i : it->second)
		{
			if (i < j && window_equal(i, j))
			{
				i64 dt = events[j].t - events[i].t;
				if (dt > 5 * SAMPLE_RATE)
				{
					i64 b = dt / (SAMPLE_RATE / 2);
					if (!periods.count(b)) period_order.push_back(b);
					periods[b]++;
					pairs.push_back({i, j, dt});
				}
			}
		}
	}
	if (pairs.empty()) return false;
	std::map<NoteKey, std::vector<i64>> by_key;
	for (const NoteEvent& e : events) by_key[e.k].push_back(e.t);
	const i64 tol = (i64)(0.03 * SAMPLE_RATE);
	auto recurrence_at = [&](size_t i, i64 per, i64& out) {
		const NoteEvent& e = events[i];
		const std::vector<i64>& ts = by_key[e.k];
		auto it = std::lower_bound(ts.begin(), ts.end(), e.t + per - tol);
		if (it != ts.end() && *it <= e.t + per + tol) { out = *it; return true; }
		return false;
	};
	// every well-supported 0.5 s bucket is a candidate (musical phrases repeat too, and a capture
	// stopped shortly after the loop gives the real period only a few windows); the decisive
	// criterion is the share of ALL note-ons of the first pass that recur at the exact offset
	std::vector<i64> by_count(period_order);
	std::stable_sort(by_count.begin(), by_count.end(), [&](i64 x, i64 y) { return periods[x] > periods[y]; });
	struct Cand { i64 per; double cover; size_t windows; };
	std::vector<Cand> cands;
	for (size_t bi = 0; bi < by_count.size() && bi < 12; bi++)
	{
		i64 bucket = by_count[bi];
		std::vector<Pair> sel_b;
		for (const Pair& pr : pairs)
			if (std::llabs(pr.dt / (SAMPLE_RATE / 2) - bucket) <= 1) sel_b.push_back(pr);
		std::map<i64, i64> fine;
		std::vector<i64> fine_order;
		size_t first_i = SIZE_MAX;
		for (const Pair& pr : sel_b)
		{
			i64 b = pr.dt / (SAMPLE_RATE / 100);
			if (!fine.count(b)) fine_order.push_back(b);
			fine[b]++;
			first_i = std::min(first_i, pr.i);
		}
		std::stable_sort(fine_order.begin(), fine_order.end(), [&](i64 x, i64 y) { return fine[x] > fine[y]; });
		double best_cover = -1;
		i64 best_per = 0;
		for (size_t k = 0; k < fine_order.size() && k < 6; k++)
		{
			std::vector<i64> near;
			for (const Pair& pr : sel_b)
				if (std::llabs(pr.dt / (SAMPLE_RATE / 100) - fine_order[k]) <= 1) near.push_back(pr.dt);
			std::sort(near.begin(), near.end());
			i64 per = near[near.size() / 2];
			i64 span = 0, hits = 0, dummy;
			for (size_t m = first_i; m < n; m++)
			{
				if (events[m].t + per + tol < events[n - 1].t)
				{
					span++;
					if (recurrence_at(m, per, dummy)) hits++;
				}
			}
			double cover = (double)hits / std::max<i64>(1, span);
			if (best_cover < 0 || cover > best_cover) { best_cover = cover; best_per = per; }
		}
		cands.push_back({best_per, best_cover, sel_b.size()});
	}
	std::string cand;
	for (const Cand& c : cands)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "%s%.1f s:%zu:%.0f%%", cand.empty() ? "" : ", ", (double)c.per / SAMPLE_RATE, c.windows, 100 * c.cover);
		cand += buf;
	}
	printf("repetition candidates (period, matching windows, note-on recurrence): %s\n", cand.c_str());
	double best_cover = -1;
	for (const Cand& c : cands) best_cover = std::max(best_cover, c.cover);
	if (best_cover < 0.6)
	{
		printf("no repetition covers the song (best %.0f%%): repeated phrases only, no loop\n", 100 * best_cover);
		return false;
	}
	// preference from the note-ons: the best covered period, then more matching windows, then
	// shorter (a multiple of the loop covers as well with fewer windows, a period shifted by one
	// phrase covers a little less); the rest by coverage
	std::vector<const Cand*> order;
	for (const Cand& c : cands) order.push_back(&c);
	// a multiple of the loop (2x, 3x, 4x) covers as well as the loop itself but matches fewer
	// windows: it goes behind its base period
	auto is_multiple_of_shorter = [&](const Cand* c) {
		for (const Cand& o : cands)
		{
			if (o.per >= c->per || o.cover < best_cover - 0.05 || o.windows <= c->windows) continue;
			double k = (double)c->per / o.per;
			int ki = (int)std::llround(k);
			if (ki >= 2 && ki <= 4 && fabs(k - ki) < 0.01 * ki) return true;
		}
		return false;
	};
	std::stable_sort(order.begin(), order.end(), [&](const Cand* a, const Cand* b) {
		bool ga = a->cover >= best_cover - 0.05, gb = b->cover >= best_cover - 0.05;
		if (ga != gb) return ga;
		if (ga)
		{
			bool ma = is_multiple_of_shorter(a), mb = is_multiple_of_shorter(b);
			if (ma != mb) return !ma;
			return a->windows != b->windows ? a->windows > b->windows : a->per < b->per;
		}
		return a->cover > b->cover;
	});
	// first repeated note-on and its recurrence for a period
	auto first_of = [&](i64 t) -> size_t { for (size_t m = 0; m < n; m++) if (events[m].t == t) return m; return 0; };
	auto locate = [&](i64 period, i64& t1, i64& t2) -> bool {
		size_t first = SIZE_MAX;
		i64 dummy;
		for (size_t i = 0; i < n; i++)
		{
			if (events[i].t < search_from) continue;
			int hits = 0;
			for (size_t m = i; m < std::min(i + 40, n); m++)
				if (recurrence_at(m, period, dummy)) hits++;
			if (hits >= 32)
			{
				for (size_t m = i; m < n; m++)
					if (recurrence_at(m, period, dummy)) { first = m; break; }
				break;
			}
		}
		if (first == SIZE_MAX) return false;
		t1 = events[first].t;
		recurrence_at(first, period, t2);
		return true;
	};
	const Cand* chosen = nullptr;
	i64 t1 = 0, t2 = 0;
	if (wav)
	{
		// the rendered audio vetoes: the loop body must sound alike the second time (note-ons alone
		// miss melodies played by pitch or volume changes on sustained voices, and a short capture
		// may repeat only a riff). The waveform correlation is too sensitive to the effect DSP's
		// modulation to rank candidates, so the note-on preference order is kept among the survivors
		struct Scored { const Cand* c; i64 t1, t2; double corr; };
		std::vector<Scored> scored;
		std::string report;
		for (const Cand* c : order)
		{
			i64 a, b;
			if (!locate(c->per, a, b)) continue;
			i64 len = std::min<i64>(std::min<i64>(b - a, search_to - b), 10 * SAMPLE_RATE);
			if (len < SAMPLE_RATE) continue;   // the capture does not reach far enough into the second pass
			double corr = audio_corr(*wav, a, b, len);
			scored.push_back({c, a, b, corr});
			char buf[48];
			snprintf(buf, sizeof(buf), "%s%.1f s:%.2f", report.empty() ? "" : ", ", (double)c->per / SAMPLE_RATE, corr);
			report += buf;
		}
		printf("audio correlation of the second pass per candidate: %s\n", report.c_str());
		for (const Scored& sc : scored)
			if (sc.corr >= 0.2) { chosen = sc.c; t1 = sc.t1; t2 = sc.t2; break; }
		if (!chosen)
		{
			printf("no candidate sounds the same on its second pass (correlation < 0.2): no loop (capture too short for a full repetition?)\n");
			return false;
		}
	}
	else
	{
		for (const Cand* c : order)
			if (locate(c->per, t1, t2)) { chosen = c; break; }
	}
	if (!chosen) return false;
	printf("period %s (%.4f s): %.0f%% of the note-ons recur at that offset\n", fmt_pos(chosen->per).c_str(), (double)chosen->per / SAMPLE_RATE, 100 * chosen->cover);
	i64 t0 = t1;   // first note-on of the range: the intro before the loop body is kept
	for (size_t i = 0; i < n; i++) if (events[i].t >= search_from) { t0 = events[i].t; break; }
	printf("loop found: first note-on at %s, first repeated note-on at %s, recurs at %s, period %s (%.4f s), %zu matching windows\n",
	       fmt_pos(t0).c_str(), fmt_pos(t1).c_str(), fmt_pos(t2).c_str(), fmt_pos(t2 - t1).c_str(), (double)(t2 - t1) / SAMPLE_RATE, chosen->windows);
	lr.start = std::max<i64>(0, t0 - pre);
	// the loop length is the median period of all recurring note-ons (sample exact when the
	// driver is frame locked), not the recurrence of one note, which may carry its own jitter
	i64 period = chosen->per;
	if (std::llabs((t2 - t1) - period) > SAMPLE_RATE / 1000)
		printf("note: the first repeated note recurs %.1f ms off the median period, loop length taken from the median\n", (double)((t2 - t1) - period) * 1000 / SAMPLE_RATE);
	// two constructions: loop body = first repetition (file = intro + 1 repetition), or = second
	// repetition when the capture holds it (the first one is left out of the stream)
	lr.loop1 = std::max<i64>(0, t1 - pre);
	lr.end1 = t1 + period - pre;
	lr.has2 = t1 + 2 * period + pre <= search_to;
	lr.loop2 = std::max<i64>(0, t1 + period - pre);
	lr.end2 = t1 + 2 * period - pre;
	return true;
}

// Voices that would be cut when the loop re-enters: audible at the end of the loop body, silent
// at the loop point in the first pass, and muted or stopped by the body's own writes within the
// first 100 ms of the second pass (a driver refreshing every volume replays the silence that
// preceded the first repetition). Returns their count.
static int loop_cut_voices(const VGMFile& vgm, i64 loop, i64 end)
{
	const u8* d = vgm.data.data();
	ES55xxState st[2] = {ES55xxState(vgm.is_es5506), ES55xxState(vgm.is_es5506)};
	std::vector<ES55xxState> at_loop;
	auto apply = [&](ES55xxState* states, const Cmd& c) {
		int ci = (c.len > 1 && (d[c.p + 1] & 0x80)) ? 1 : 0;
		RegWrite rw;
		if (c.op == 0xBE) states[ci].write8(d[c.p + 1] & 0x7F, d[c.p + 2], rw);
		else if (c.op == 0xD6) states[ci].write16(d[c.p + 1] & 0x7F, (d[c.p + 2] << 8) | d[c.p + 3], rw);
	};
	auto volume = [&](const ES55xxState& s, int v) -> int {
		if (!((s.running >> v) & 1)) return 0;
		if (vgm.is_es5506) return std::max(s.known[v][2] ? (int)(s.regs[v][2] >> 8) : 0, s.known[v][4] ? (int)(s.regs[v][4] >> 8) : 0);
		return std::max(s.known[v][8] ? (int)(s.regs[v][8] >> 8) : 0, s.known[v][9] ? (int)(s.regs[v][9] >> 8) : 0);
	};
	vgm.commands([&](const Cmd& c) {
		if (c.op == 0x66 || c.t >= end) return false;
		if (c.t >= loop && at_loop.empty()) at_loop.assign(st, st + 2);
		apply(st, c);
		return true;
	});
	if (at_loop.empty()) return 0;
	// second pass: start from the end state, replay the first 100 ms of the body
	ES55xxState pass2[2] = {st[0], st[1]};
	int audible_end[2][32];
	for (int ci = 0; ci < 2; ci++) for (int v = 0; v < 32; v++) audible_end[ci][v] = volume(st[ci], v);
	int cut = 0;
	bool counted[2][32] = {};
	vgm.commands([&](const Cmd& c) {
		if (c.op == 0x66 || c.t >= loop + SAMPLE_RATE / 10) return false;
		if (c.t < loop) return true;
		apply(pass2, c);
		for (int ci = 0; ci < 2; ci++)
			for (int v = 0; v < 32; v++)
				if (!counted[ci][v] && audible_end[ci][v] >= 0x10 && volume(at_loop[ci], v) < 0x10 && volume(pass2[ci], v) < 0x10)
				{
					counted[ci][v] = true;
					cut++;
				}
		return true;
	});
	return cut;
}

// ---------------------------------------------------------------------------------------------
// WAV based track splitting
// ---------------------------------------------------------------------------------------------
struct Wav { int sr = 0, nch = 0; std::vector<int16_t> smp; size_t frames = 0; };

static bool read_wav(const std::string& path, Wav& w)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	u8 hdr[12];
	if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return false; }
	int bits = 16;
	for (;;)
	{
		u8 ch[8];
		if (fread(ch, 1, 8, f) != 8) { fclose(f); return false; }
		u32 size = le32(ch + 4);
		if (!memcmp(ch, "fmt ", 4))
		{
			Bytes fmt(size);
			if (fread(fmt.data(), 1, size, f) != size) { fclose(f); return false; }
			w.nch = fmt[2] | (fmt[3] << 8);
			w.sr = le32(&fmt[4]);
			bits = fmt[14] | (fmt[15] << 8);
		}
		else if (!memcmp(ch, "data", 4))
		{
			if (bits != 16 || w.nch == 0) { fclose(f); return false; }
			w.smp.resize(size / 2);
			size_t got = fread(w.smp.data(), 1, size, f);
			w.smp.resize(got / 2);
			w.frames = w.smp.size() / w.nch;
			break;
		}
		else
			fseek(f, size + (size & 1), SEEK_CUR);
	}
	fclose(f);
	return true;
}

// peak of every 10 ms window after removing the window's DC offset (boards with an effects DSP
// in the output path, like Taito F3, output a small DC level and noise while "silent")
static std::vector<int> window_peaks(const Wav& w, size_t win)
{
	std::vector<int> peaks;
	for (size_t i = 0; i < w.frames; i += win)
	{
		size_t e = std::min(w.frames, i + win);
		int peak = 0;
		for (int ch = 0; ch < w.nch; ch++)
		{
			double mean = 0;
			for (size_t f = i; f < e; f++) mean += w.smp[f * w.nch + ch];
			mean /= (double)(e - i);
			for (size_t f = i; f < e; f++) peak = std::max(peak, abs((int)(w.smp[f * w.nch + ch] - mean)));
		}
		peaks.push_back(peak);
	}
	return peaks;
}

// silence threshold from the noise floor: 3x the 5th percentile of the window peaks, 4..64
static int auto_silence(const std::vector<int>& peaks)
{
	if (peaks.empty()) return 4;
	std::vector<int> sorted(peaks);
	std::sort(sorted.begin(), sorted.end());
	int noise = sorted[sorted.size() / 20];
	return std::max(4, std::min(64, noise * 3 + 4));
}

static std::vector<std::pair<i64, i64>> find_segments(const Wav& w, int silence_peak, i64 min_silence, i64 min_length)
{
	size_t win = w.sr / 100;
	std::vector<int> peaks = window_peaks(w, win);
	if (silence_peak < 0) { silence_peak = auto_silence(peaks); printf("silence threshold (auto, from the noise floor): peak <= %d\n", silence_peak); }
	// the chip initialisation at t = 0 leaves a DC step in the first window(s): a burst shorter than 50 ms
	// at the very start of the capture, followed by at least 0.5 s of silence, is not sound
	{
		size_t first = 0, last = 0;
		while (first < peaks.size() && peaks[first] <= silence_peak) first++;
		for (last = first; last < peaks.size() && peaks[last] > silence_peak; last++);
		size_t quiet = 0;
		while (last + quiet < peaks.size() && peaks[last + quiet] <= silence_peak) quiet++;
		if (first < 5 && last < first + 5 && last < peaks.size() && quiet >= 50)
		{
			printf("start-up click at %s ignored (%zu ms, then %.2f s of silence)\n", fmt_pos((i64)(first * win * SAMPLE_RATE / w.sr)).c_str(), (last - first) * 10, quiet * 0.01);
			for (size_t k = first; k < last; k++) peaks[k] = 0;
		}
	}
	std::vector<std::pair<i64, i64>> segs;
	bool in_seg = false;
	size_t seg_start = 0, seg_last = 0, silent_run = 0, idx = 0;
	for (size_t i = 0; i < w.frames; i += win, idx++)
	{
		int peak = peaks[idx];
		bool loud = peak > silence_peak;
		if (loud)
		{
			if (!in_seg) { in_seg = true; seg_start = idx; }
			silent_run = 0;
			seg_last = idx;
		}
		else if (in_seg)
		{
			silent_run++;
			if ((i64)(silent_run * win) >= min_silence)
			{
				segs.push_back({(i64)(seg_start * win), (i64)((seg_last + 1) * win)});
				in_seg = false;
			}
		}
	}
	if (in_seg) segs.push_back({(i64)(seg_start * win), (i64)((seg_last + 1) * win)});
	double scale = (double)SAMPLE_RATE / w.sr;
	std::vector<std::pair<i64, i64>> out;
	for (auto& s : segs)
		if (s.second - s.first >= min_length)
			out.push_back({(i64)(s.first * scale), (i64)(s.second * scale)});
	return out;
}

// normalized correlation (left channel) between n samples at a and at b, b searched within
// +-max_lag (coarse step 40, then fine); positions in 44.1 kHz samples
static double audio_corr(const Wav& w, i64 a, i64 b, i64 n, i64 max_lag)
{
	double scale = (double)w.sr / SAMPLE_RATE;
	i64 A = (i64)(a * scale), B = (i64)(b * scale), N = (i64)(n * scale), L = (i64)(max_lag * scale);
	auto corr_at = [&](i64 lag) {
		double xy = 0, xx = 0, yy = 0;
		for (i64 i = 0; i < N; i++)
		{
			i64 ia = A + i, ib = B + lag + i;
			if (ia < 0 || ib < 0 || ia >= (i64)w.frames || ib >= (i64)w.frames) continue;
			double x = w.smp[ia * w.nch], y = w.smp[ib * w.nch];
			xy += x * y; xx += x * x; yy += y * y;
		}
		return (xx > 0 && yy > 0) ? xy / sqrt(xx * yy) : 0.0;
	};
	i64 best = 0; double bestc = -2;
	for (i64 lag = -L; lag <= L; lag += 40) { double c = corr_at(lag); if (c > bestc) { bestc = c; best = lag; } }
	i64 center = best;
	for (i64 lag = center - 40; lag <= center + 40; lag++) { double c = corr_at(lag); if (c > bestc) { bestc = c; best = lag; } }
	return bestc;
}

static void print_loudness(const Wav& w, double step = 2.0)
{
	size_t win = (size_t)(w.sr * step);
	printf("RMS level (left channel) every %.0f s:\n", step);
	int col = 0;
	for (size_t i = 0; i + win <= w.frames; i += win)
	{
		double acc = 0;
		for (size_t s = i; s < i + win; s++) { double v = w.smp[s * w.nch]; acc += v * v; }
		printf("%s%4zu:%5.0f", col == 0 ? "  " : " ", i / w.sr, sqrt(acc / win));
		if (++col == 10) { printf("\n"); col = 0; }
	}
	if (col) printf("\n");
}

// ---------------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------------
static bool ends_with_nocase(const std::string& s, const char* suffix)
{
	size_t n = strlen(suffix);
	if (s.size() < n) return false;
	for (size_t i = 0; i < n; i++) if (tolower((unsigned char)s[s.size() - n + i]) != suffix[i]) return false;
	return true;
}

// .vgz output is gzip-compressed (level 9, no name/mtime in the gzip header)
// --append: concatenate captures. The data blocks (0x67) of every file come first (exact duplicates
// dropped), then the command streams in order: the initial register image of an appended file re-creates
// its state at the junction, the ESP keeps the tail of the previous part. Header and GD3 of the first
// file, total = sum, loop of the last file that has one (offsets shifted, the loop runs to the end).
static Bytes merge(const std::vector<VGMFile>& in)
{
	const VGMFile& a = in[0];
	Bytes blocks, body;
	std::set<Bytes> seen;
	i64 total = 0, loop_t = -1;
	size_t loop_body_ofs = 0, loop_part = 0;
	for (size_t k = 0; k < in.size(); k++) if (in[k].loop_ofs) loop_part = k + 1;
	if (loop_part && loop_part < in.size())
		fprintf(stderr, "warning: part %zu has a loop but is not the last part: the loop will run to the end of the merged file\n", loop_part);
	for (size_t k = 0; k < in.size(); k++)
	{
		const VGMFile& v = in[k];
		if (v.es5506_clock != a.es5506_clock || v.is_es5506 != a.is_es5506)
			fprintf(stderr, "warning: part %zu: ES5506 clock %08X differs from the first part (%08X)\n", k + 1, v.es5506_clock, a.es5506_clock);
		if (k && v.data_ofs > 0x7C && a.data_ofs > 0x7C && v.data[0x7C] != a.data[0x7C])
			fprintf(stderr, "warning: part %zu: volume modifier %02X differs from the first part (%02X), the first one is kept\n", k + 1, v.data[0x7C], a.data[0x7C]);
		if (v.loop_ofs && k + 1 != loop_part) fprintf(stderr, "warning: part %zu has a loop too, only the one of part %zu is kept\n", k + 1, loop_part);
		if (k + 1 == loop_part) loop_t = -1;
		const u8* d = v.data.data();
		bool found_loop = false;
		v.commands([&](const Cmd& c) {
			if (c.op == 0x66) return false;
			if (k + 1 == loop_part && c.p == v.loop_ofs) { loop_body_ofs = body.size(); loop_t = total + c.t; found_loop = true; }
			if (c.op == 0x67)
			{
				Bytes b(d + c.p, d + c.p + c.len);
				if (seen.insert(b).second) blocks.insert(blocks.end(), b.begin(), b.end());
				return true;
			}
			body.insert(body.end(), d + c.p, d + c.p + c.len);
			return true;
		});
		if (k + 1 == loop_part && !found_loop) fprintf(stderr, "warning: part %zu: loop offset does not fall on a command, loop dropped\n", k + 1);
		total += v.total;
		printf("part %zu: %s%s\n", k + 1, fmt_pos(v.total).c_str(), v.loop_ofs ? (", loop " + fmt_pos(v.loop_samples)).c_str() : "");
	}
	body.push_back(0x66);
	Bytes out(a.data.begin(), a.data.begin() + a.data_ofs);
	size_t data_start = out.size();
	put_le32(&out[0x18], (u32)total);
	if (loop_t >= 0)
	{
		put_le32(&out[0x1C], (u32)(data_start + blocks.size() + loop_body_ofs - 0x1C));
		put_le32(&out[0x20], (u32)(total - loop_t));
	}
	else { put_le32(&out[0x1C], 0); put_le32(&out[0x20], 0); }
	out.insert(out.end(), blocks.begin(), blocks.end());
	out.insert(out.end(), body.begin(), body.end());
	if (a.gd3 && a.gd3 + 12 <= a.data.size())
	{
		u32 gd3_len = le32(&a.data[a.gd3 + 8]);
		size_t e = std::min<size_t>(a.gd3 + 12 + gd3_len, a.data.size());
		put_le32(&out[0x14], (u32)(out.size() - 0x14));
		out.insert(out.end(), a.data.begin() + a.gd3, a.data.begin() + e);
	}
	else put_le32(&out[0x14], 0);
	fix_version(out);
	put_le32(&out[0x04], (u32)(out.size() - 4));
	printf("merged: %s%s, %zu data block(s)\n", fmt_pos(total).c_str(), loop_t >= 0 ? (", loop " + fmt_pos(total - loop_t)).c_str() : "", seen.size());
	return out;
}

static void write_output(const std::string& path, const Bytes& out)
{
	if (ends_with_nocase(path, ".vgz"))
	{
		gzFile g = gzopen(path.c_str(), "wb9");
		if (!g) die("cannot write " + path);
		size_t pos = 0;
		while (pos < out.size())
		{
			unsigned chunk = (unsigned)std::min<size_t>(out.size() - pos, 1u << 28);
			if (gzwrite(g, out.data() + pos, chunk) != (int)chunk) die("gzip write failed: " + path);
			pos += chunk;
		}
		if (gzclose(g) != Z_OK) die("gzip close failed: " + path);
		return;
	}
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) die("cannot write " + path);
	fwrite(out.data(), 1, out.size(), f);
	fclose(f);
}

static std::string track_name(const std::string& output, int n)
{
	char num[8];
	snprintf(num, sizeof(num), "%02d", n);
	size_t ph = output.find("{n}");
	if (ph != std::string::npos) return output.substr(0, ph) + num + output.substr(ph + 3);
	size_t dot = output.rfind('.');
	size_t slash = output.find_last_of("/\\");
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		return output.substr(0, dot) + "_" + num + output.substr(dot);
	return output + "_" + num;
}

static void usage()
{
	fprintf(stderr,
	        "usage: vgm_trim input [output] [options]\n"
	        "  output ending in .vgz is written gzip-compressed; with --track all, tracks are output_NN.vgm/.vgz\n"
	        "  --info                 print file information and exit\n"
	        "  --activity             print the running-voice timeline and the silent gaps (from the log) and exit\n"
	        "  --start P --end P      trim range (samples, \"12.5s\" or \"m:ss.sss\")\n"
	        "  --loop P               loop point\n"
	        "  --find-loop            locate the loop from the note-on sequence (within --start/--end)\n"
	        "  --loop-pass N          --find-loop: 1 = loop body is the 1st repetition, 2 = the 2nd one (seamless loop, the 1st is\n"
	        "                         left out, file = intro + 1 repetition); default: 2 only when the loop re-entry would cut voices\n"
	        "  --cmp                  drop register writes that do not change the chip state\n"
	        "  --sro                  keep only the sample ROM ranges that are played\n"
	        "  --sro-margin N         --sro: margin in words around each used range (default 256)\n"
	        "  --sro-gap N            --sro: merge used ranges closer than this many words (default 0)\n"
	        "  --gd3 keep|drop        keep the GD3 tag (default keep)\n"
	        "  --append FILE          append FILE (repeatable) to the input and write the result: header, GD3 and volume of the\n"
	        "                         input, loop of the last part that has one; no other processing\n"
	        "  (no option)            rewrite the input as is (.vgm <-> .vgz, loop kept); files written as VGM 1.73 become 1.72\n"
	        "  --volume D             set the header volume modifier (VGM 1.60+) from a gain in dB (e.g. +6dB, -3): the player\n"
	        "                         applies 2^(n/32), n = round(D / 6.02 * 32), range -64..192 (-12 dB..+36 dB); 0 removes it\n"
	        "  --auto                 split the capture into tracks separated by silence\n"
	        "  --split wav|log        --auto: from the rendered audio (needs vgm2wav) or from the log (default wav)\n"
	        "  --track N|all|0        --auto: track to extract (default 1), all = every track to output_NN, 0 = list only\n"
	        "  --vgm2wav PATH         vgm2wav tool (default: vgm2wav)\n"
	        "  --silence N            --auto: peak level treated as silence (default: automatic from the noise floor, DC removed)\n"
	        "  --gap P                --auto / --activity: minimum silence between two tracks (default 1.0s)\n"
	        "  --pre P --post P       --auto: silence kept before / after each track (default 0.05s / 0.3s)\n"
	        "  --min-length P         --auto: ignore segments shorter than this (default 0.5s)\n");
	exit(1);
}

int main(int argc, char** argv)
{
	std::string input, output, start_s, end_s, loop_s, gd3 = "keep", split = "wav", track = "1", vgm2wav = "vgm2wav";
	std::string gap = "1.0s", pre = "0.05s", post = "0.3s", min_length = "0.5s";
	bool f_info = false, f_activity = false, f_find_loop = false, f_cmp = false, f_sro = false, f_auto = false;
	i64 sro_margin = 256, sro_gap = 0;
	int silence = -1;   // -1: automatic (noise floor)
	std::vector<std::string> positional, appends;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto value = [&](std::string& dst) {
			size_t eq = a.find('=');
			if (eq != std::string::npos) dst = a.substr(eq + 1);
			else if (i + 1 < argc) dst = argv[++i];
			else usage();
		};
		std::string name = a.substr(0, a.find('='));
		if (name == "--info") f_info = true;
		else if (name == "--activity") f_activity = true;
		else if (name == "--find-loop") f_find_loop = true;
		else if (name == "--cmp") f_cmp = true;
		else if (name == "--sro") f_sro = true;
		else if (name == "--auto") f_auto = true;
		else if (name == "--loop-pass") { std::string v; value(v); g_loop_pass = atoi(v.c_str()); }
		else if (name == "--start") value(start_s);
		else if (name == "--end") value(end_s);
		else if (name == "--loop") value(loop_s);
		else if (name == "--gd3") value(gd3);
		else if (name == "--append") { std::string v; value(v); appends.push_back(v); }
		else if (name == "--volume")
		{
			std::string v; value(v);
			char* e = nullptr;
			double db = strtod(v.c_str(), &e);
			if (e == v.c_str() || (*e && strcasecmp(e, "dB") != 0)) { fprintf(stderr, "--volume: expected a gain in dB, got %s\n", v.c_str()); usage(); }
			g_volume_mod = volume_mod_from_db(db);
			if (g_volume_mod < -64 || g_volume_mod > 192) { fprintf(stderr, "--volume: %s is out of range (-12 dB..+36 dB)\n", v.c_str()); usage(); }
			fprintf(stderr, "volume modifier %d (%+.2f dB requested, %+.2f dB applied, x%.3f)\n", g_volume_mod, db, volume_db_from_mod(g_volume_mod), std::pow(2.0, g_volume_mod / 32.0));
		}
		else if (name == "--split") value(split);
		else if (name == "--track") value(track);
		else if (name == "--vgm2wav") value(vgm2wav);
		else if (name == "--gap") value(gap);
		else if (name == "--pre") value(pre);
		else if (name == "--post") value(post);
		else if (name == "--min-length") value(min_length);
		else if (name == "--sro-margin") { std::string v; value(v); sro_margin = atoll(v.c_str()); }
		else if (name == "--sro-gap") { std::string v; value(v); sro_gap = atoll(v.c_str()); }
		else if (name == "--silence") { std::string v; value(v); silence = atoi(v.c_str()); }
		else if (name.rfind("--", 0) == 0) { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); }
		else positional.push_back(a);
	}
	if (positional.empty()) usage();
	input = positional[0];
	if (positional.size() > 1) output = positional[1];

	VGMFile vgm;
	vgm.load(input);
	if (f_activity) { activity(vgm, parse_pos(gap), false); return 0; }
	if (f_info || output.empty()) { info(vgm); return 0; }
	if (!appends.empty())
	{
		std::vector<VGMFile> parts(1 + appends.size());
		parts[0] = std::move(vgm);
		for (size_t k = 0; k < appends.size(); k++) parts[k + 1].load(appends[k]);
		Bytes out = merge(parts);
		write_output(output, out);
		printf("%s -> %s: %zu bytes\n", input.c_str(), output.c_str(), out.size());
		return 0;
	}

	if (!f_auto && !f_find_loop && !f_cmp && !f_sro && start_s.empty() && end_s.empty() && loop_s.empty() && g_volume_mod == INT_MIN && gd3 == "keep")
	{
		// nothing to trim: rewrite the file as is (loop kept), only the version word is normalised
		Bytes out = vgm.data;
		fix_version(out);
		write_output(output, out);
		printf("%s -> %s: rewritten unchanged (version %X), %zu bytes\n", input.c_str(), output.c_str(), le32(&out[0x08]), out.size());
		return 0;
	}

	struct Job { i64 start, loop, end; bool has_loop; std::string output; bool explicit_start; i64 skip_from = 0, skip_to = 0; };
	std::vector<Job> jobs;
	std::unique_ptr<Wav> render;   // rendered capture (--split wav), also used to verify the loop candidates
	if (f_auto)
	{
		std::vector<std::pair<i64, i64>> segs;
		if (split == "log")
		{
			auto gaps = activity(vgm, parse_pos(gap), true);
			i64 pos = 0, minlen = parse_pos(min_length);
			for (auto& g : gaps)
			{
				if (g.first - pos >= minlen) segs.push_back({pos, g.first});
				pos = g.second;
			}
			if ((i64)vgm.total - pos >= minlen) segs.push_back({pos, (i64)vgm.total});
		}
		else
		{
			char tmpl[] = "/tmp/es55xx_XXXXXX";
			int fd = mkstemp(tmpl);
			if (fd < 0) die("cannot create a temporary file");
			close(fd);
			std::string wav = std::string(tmpl) + ".wav";
			remove(tmpl);
			std::string cmd = "\"" + vgm2wav + "\" --fade 0 \"" + input + "\" \"" + wav + "\" >/dev/null 2>&1";
			int rc = system(cmd.c_str());
			render.reset(new Wav());
			Wav& w = *render;
			if (rc != 0 || !read_wav(wav, w)) { remove(wav.c_str()); die("vgm2wav failed (" + vgm2wav + ")"); }
			remove(wav.c_str());
			segs = find_segments(w, silence, parse_pos(gap), parse_pos(min_length));
			if (track == "0") print_loudness(w);
		}
		if (segs.empty()) die("no non-silent segment found");
		for (size_t i = 0; i < segs.size(); i++)
			printf("track %zu: %s .. %s (%.1f s)\n", i + 1, fmt_pos(segs[i].first).c_str(), fmt_pos(segs[i].second).c_str(),
			       (double)(segs[i].second - segs[i].first) / SAMPLE_RATE);
		std::vector<size_t> wanted;
		if (track == "all")
			for (size_t i = 0; i < segs.size(); i++) wanted.push_back(i);
		else
		{
			int n = atoi(track.c_str());
			if (n < 1 || n > (int)segs.size()) return 0;
			wanted.push_back(n - 1);
		}
		for (size_t i : wanted)
			jobs.push_back({std::max<i64>(0, segs[i].first - parse_pos(pre)), 0, std::min<i64>(vgm.total, segs[i].second + parse_pos(post)), false,
			                track == "all" ? track_name(output, (int)i + 1) : output, true});
	}
	else
		jobs.push_back({start_s.empty() ? 0 : parse_pos(start_s), loop_s.empty() ? 0 : parse_pos(loop_s),
		                end_s.empty() ? (i64)vgm.total : parse_pos(end_s), !loop_s.empty(), output, !start_s.empty()});

	for (Job& job : jobs)
	{
		if (job.end > (i64)vgm.total) job.end = vgm.total;
		if (f_find_loop)
		{
			LoopResult lr;
			i64 s = 0, l = 0, e = 0;
			bool found = find_loop(vgm, job.start, job.end, true, lr, render.get());
			if (found)
			{
				int pass = g_loop_pass;
				s = lr.start;
				if (vgm.is_zsg2 && pass != 1)
				{
					if (pass == 2) printf("warning: --loop-pass 2 is not available for ZSG-2 logs, looping on the first repetition\n");
					pass = 1;
				}
				if (pass == 0)
				{
					int cut = loop_cut_voices(vgm, lr.loop1, lr.end1);
					if (cut == 0)
						pass = 1;
					else if (lr.has2)
					{
						pass = 2;
						printf("loop re-entry would cut %d voice(s) still sounding at the end of the first repetition: "
						       "taking the loop body from the second repetition\n", cut);
					}
					else
					{
						pass = 1;
						printf("warning: loop re-entry cuts %d voice(s) still sounding at the end of the loop body; the capture is too short to loop "
						       "on the second repetition instead (needs %s). Capture intro + 2 repetitions + a few seconds for a seamless loop.\n",
						       cut, fmt_pos(lr.end2 + 2205).c_str());
					}
				}
				else if (pass == 2 && !lr.has2)
				{
					pass = 1;
					printf("warning: capture too short to loop on the second repetition (needs %s): looping on the first one\n", fmt_pos(lr.end2 + 2205).c_str());
				}
				if (pass == 2)
				{
					l = lr.loop2; e = lr.end2; job.skip_from = lr.loop1; job.skip_to = lr.loop2;
					printf("loop body = second repetition %s .. %s, the first one (%s .. %s) is left out: the file plays intro + one repetition\n",
					       fmt_pos(l).c_str(), fmt_pos(e).c_str(), fmt_pos(lr.loop1).c_str(), fmt_pos(lr.loop2).c_str());
				}
				else { l = lr.loop1; e = lr.end1; }
			}
			if (!found)
			{
				printf("%s: no loop found, written without loop\n", job.output.c_str());
			}
			else
			{
				// an explicit start (--start or a detected segment) is kept; otherwise start at the first note-on
				if (!job.explicit_start) job.start = s;
				job.loop = std::max(l, job.start); job.end = std::min(e, job.end); job.has_loop = true;
			}
		}
		if (job.has_loop && !(job.start <= job.loop && job.loop < job.end)) die("loop point must be within [start, end)");
		TrimStats stats;
		Bytes out = vgm.is_zsg2 ? trim_zsg2(vgm, job.start, job.loop, job.has_loop, job.end, f_cmp, f_sro, gd3 == "keep", sro_margin, sro_gap, stats)
		                        : trim(vgm, job.start, job.loop, job.has_loop, job.end, f_cmp, f_sro, gd3 == "keep", sro_margin, sro_gap, stats, job.skip_from, job.skip_to);
		write_output(job.output, out);
		printf("%s -> %s: %s .. %s%s, %lld writes kept, %lld dropped (--cmp), %lld commands before start, ROM data %zu bytes; %zu -> %zu bytes\n",
		       input.c_str(), job.output.c_str(), fmt_pos(job.start).c_str(), fmt_pos(job.end).c_str(),
		       job.has_loop ? (", loop at " + fmt_pos(job.loop)).c_str() : "",
		       (long long)stats.kept, (long long)stats.dropped, (long long)stats.skipped, stats.rom_bytes, vgm.data.size(), out.size());
	}
	return 0;
}
