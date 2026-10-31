// vgm_v3conv: converts VGM files of the first ES55xx / ZSG-2 encoding (v1, what the MAME logger and the
// libvgm "es55xx" branch wrote until 2026-10) to the v3 encoding of VGM-proposal-v3-allocation.md.
//
// usage: vgm_v3conv input.vg[mz] output.vg[mz] [--force]
//   output gzipped when its name ends in .vgz. Exit code 0 = converted, 2 = nothing to convert (the
//   output is a plain copy), 1 = error (unrepresentable v1 command, broken file...).
//   --force: convert even when the header does not look like a v1 file.
//
// The conversion is lossless by construction: every v1 command maps to v3 commands that drive the same
// emulation entry points. It must still be checked by rendering (v3pack.py does: vgm2wav-v1 vs vgm2wav,
// byte comparison).
//
// build: c++ -O2 -std=c++17 -o vgm_v3conv vgm_v3conv.cpp -lz

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

typedef std::vector<uint8_t> Bytes;

static uint32_t rd32(const Bytes& d, size_t o) { return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24); }
static void wr32(Bytes& d, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) d[o + i] = (uint8_t)(v >> (8 * i)); }

[[noreturn]] static void fail(const std::string& s) { throw std::runtime_error(s); }

static Bytes load(const std::string& path)
{
	gzFile g = gzopen(path.c_str(), "rb");
	if (!g) fail("cannot open " + path);
	Bytes out;
	uint8_t buf[1 << 16];
	int n;
	while ((n = gzread(g, buf, sizeof(buf))) > 0) out.insert(out.end(), buf, buf + n);
	gzclose(g);
	if (n < 0) fail("read error: " + path);
	return out;
}

static void save(const std::string& path, const Bytes& d)
{
	bool gz = path.size() >= 4 && strcasecmp(path.c_str() + path.size() - 4, ".vgz") == 0;
	if (gz)
	{
		gzFile g = gzopen(path.c_str(), "wb9");
		if (!g || gzwrite(g, d.data(), (unsigned)d.size()) != (int)d.size()) fail("write error: " + path);
		gzclose(g);
	}
	else
	{
		FILE* f = fopen(path.c_str(), "wb");
		if (!f || fwrite(d.data(), 1, d.size(), f) != d.size()) fail("write error: " + path);
		fclose(f);
	}
}

// length of the command at d[p] (0x67 data blocks handled by the caller), 0 = unknown
static size_t cmd_len(uint8_t o)
{
	if (o >= 0x30 && o <= 0x3F) return 2;
	if (o >= 0x40 && o <= 0x4E) return 3;
	if (o == 0x4F || o == 0x50) return 2;
	if (o >= 0x51 && o <= 0x5F) return 3;
	if (o == 0x61) return 3;
	if (o == 0x62 || o == 0x63 || o == 0x66) return 1;
	if (o == 0x68) return 12;
	if (o >= 0x70 && o <= 0x8F) return 1;
	switch (o) { case 0x90: case 0x91: case 0x95: return 5; case 0x92: return 6; case 0x93: return 11; case 0x94: return 2; }
	if (o >= 0xA0 && o <= 0xBF) return 3;
	if (o >= 0xC0 && o <= 0xDF) return 4;
	if (o >= 0xE0) return 5;
	return 0;
}

struct Conv
{
	const Bytes& in;
	Bytes out;          // command stream only
	bool es = false, zsg = false;
	bool espUsed[2] = {false, false};
	bool espCreated[2] = {false, false};
	// v1 ESP routing state, simulated so that presets can be expressed as 0xE2 commands
	uint8_t routeIn[2][6] = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
	bool direct[2][12] = {{true, true, true, true, true, true, true, true, true, true, true, true},
	                      {true, true, true, true, true, true, true, true, true, true, true, true}};
	uint8_t dramLog2 = 0;
	uint8_t window[2][4] = {}, alias[2][4] = {{0, 1, 2, 3}, {0, 1, 2, 3}};
	std::map<std::string, unsigned> stats;

	explicit Conv(const Bytes& d) : in(d) {}

	void put(std::initializer_list<uint8_t> b) { out.insert(out.end(), b); }
	void count(const std::string& k) { stats[k]++; }

	// v3 0xE2 (as the player applies it): input ii <- channel cc, and that channel leaves the direct mix
	void route(uint8_t chip, uint8_t ii, uint8_t cc)
	{
		uint8_t c7 = chip << 7;
		put({0xE2, (uint8_t)(0x35 | c7), ii, (uint8_t)(0x25 | c7), cc});
		if (ii < 6) { routeIn[chip][ii] = cc; if (cc < 12) direct[chip][cc] = false; }
	}

	// v1 preset = reset (all channels direct, no input) + preset routes. Representable when no channel that
	// the preset makes direct again is currently routed away from the direct mix (0xE2 cannot restore it).
	void es_preset(uint8_t chip, uint8_t preset)
	{
		uint8_t tIn[6]; bool tDir[12];
		for (int c = 0; c < 12; c++) tDir[c] = true;
		for (int i = 0; i < 6; i++) tIn[i] = 0xFF;
		if (preset == 0) for (int i = 0; i < 6; i++) { tIn[i] = (uint8_t)(i + 2); tDir[i + 2] = false; }
		else if (preset == 1) for (int i = 0; i < 6; i++) { tIn[i] = (uint8_t)i; tDir[i] = false; }
		for (int c = 0; c < 12; c++)
			if (tDir[c] && !direct[chip][c]) fail("ES5510 routing preset would put channel " + std::to_string(c) + " back in the direct mix: not representable in v3");
		espCreated[chip] = espUsed[chip] = true;
		bool any = false;
		for (uint8_t i = 0; i < 6; i++)
			if (routeIn[chip][i] != tIn[i] || (tIn[i] < 12 && direct[chip][tIn[i]])) { route(chip, i, tIn[i]); any = true; }
		if (!any)   // nothing changes: a no-op route still creates the ESP like the v1 command did
			put({0xE2, (uint8_t)(0x35 | (chip << 7)), 0x00, (uint8_t)(0x25 | (chip << 7)), routeIn[chip][0]});
	}

	void es_d7(uint8_t pp, uint8_t aa, uint8_t dd)
	{
		uint8_t chip = pp >> 7, c7 = pp & 0x80;
		switch (pp & 0x7F)
		{
		case 0x00: put({0x49, (uint8_t)((aa & 0x1F) | c7), dd}); count("D7 00 bank -> 49"); break;
		case 0x01:
			if (dd > 0x3F) fail("ES5506 region window beyond 63M words");
			window[chip][aa & 3] = dd;
			put({0x4A, (uint8_t)((aa & 3) | c7), (uint8_t)((dd << 2) | alias[chip][aa & 3])}); count("D7 01 window -> 4A"); break;
		case 0x02:
			alias[chip][aa & 3] = dd & 3;
			put({0x4A, (uint8_t)((aa & 3) | c7), (uint8_t)((window[chip][aa & 3] << 2) | (dd & 3))}); count("D7 02 alias -> 4A"); break;
		case 0x03: count("D7 03 IRQ ack (dropped)"); break;
		case 0x04:
			if (!dd) fail("ES5506 IRQ auto-ack disabled (D7 04 00): not representable in v3");
			count("D7 04 IRQ auto-ack (dropped)"); break;
		default: fail("unknown ES5506 extension D7 " + std::to_string(pp));
		}
	}

	void es_d8(uint8_t pp, uint8_t aa, uint8_t dd)
	{
		uint8_t chip = pp >> 7, c7 = pp & 0x80;
		uint8_t sub = pp & 0x7F;
		if (sub == 0x06)
		{
			if (espCreated[chip]) fail("ES5510 DRAM size set after the ESP started");
			if (dramLog2 && dramLog2 != dd) fail("two different ES5510 DRAM sizes");
			dramLog2 = dd;
			count("D8 06 DRAM size -> header 0x11C");
			return;
		}
		espCreated[chip] = espUsed[chip] = true;
		switch (sub)
		{
		case 0x00: put({0xCC, c7, aa, dd}); count("D8 00 host -> CC"); break;
		case 0x01: put({0x4B, c7, (uint8_t)(dd ? 1 : 0)}); count("D8 01 halt -> 4B"); break;
		case 0x02:
			if (aa >= 6) { count("D8 02 route of a missing input (dropped)"); break; }
			route(chip, aa, dd); count("D8 02 route -> E2"); break;
		case 0x03:
			if ((aa != 6 && aa != 7) || (dd & 1) != (aa & 1)) fail("ES5510 output routed to the other side: not representable in v3");
			count("D8 03 route out (default, dropped)"); break;
		case 0x04: fail("ES5506 direct-mix flag (D8 04): not representable in v3");
		case 0x05: es_preset(chip, aa); count("D8 05 preset -> E2"); break;
		default: fail("unknown ES5510 command D8 " + std::to_string(pp));
		}
	}

	void es_e3(uint8_t pp, uint8_t cc, uint8_t g0, uint8_t g1)
	{
		uint8_t c7 = pp & 0x80;
		if (cc < 12) put({0xE3, (uint8_t)(0x25 | c7), cc, g0, g1});
		else if (cc == 0x10 || cc == 0x11) put({0xE3, (uint8_t)(0x35 | c7), (uint8_t)(6 + (cc & 1)), g0, g1});
		else if (cc == 0x12 || cc == 0x13) put({0xE3, (uint8_t)(0x25 | c7), (uint8_t)(0xFE + (cc & 1)), g0, g1});
		else { count("E3 unused gain channel (dropped)"); return; }
		count("E3 gain -> E3 tt");
	}

	void zsg_e4(uint8_t ss, uint8_t aa, uint8_t d0, uint8_t d1)
	{
		uint8_t c7 = ss & 0x80;
		uint16_t dddd = (uint16_t)((d0 << 8) | d1);
		switch (ss & 0x0F)
		{
		case 0x0: case 0x1: case 0x2: case 0x3:
			put({0xE4, (uint8_t)(c7 | (ss & 3)), aa, d0, d1}); count("E4 reg -> E4 oooo"); break;
		case 0x4: put({0x4D, c7, d1}); count("E4 4 DSP data -> 4D"); break;
		case 0x5: put({0x4E, c7, (uint8_t)(d1 & 3)}); count("E4 5 DSP lines -> 4E"); break;
		case 0x6:
		{
			uint8_t tt, cc;
			if (aa < 4) { tt = 0x36; cc = 0x80 + aa; }
			else if (aa >= 0x10 && aa < 0x14 && !(aa & 1)) { tt = 0x36; cc = aa & 3; }
			else if (aa >= 0x14 && aa < 0x18 && (aa & 1)) { tt = 0x36; cc = aa & 3; }
			else if (aa >= 0x20 && aa < 0x24 && !(aa & 1)) { tt = 0x34; cc = aa & 3; }
			else if (aa >= 0x24 && aa < 0x28 && (aa & 1)) { tt = 0x34; cc = aa & 3; }
			else if (aa < 0x28 && dddd == 0) { count("E4 6 zero gain to the other side (default, dropped)"); return; }
			else fail("ZSG-2 mixer gain channel " + std::to_string(aa) + " not representable in v3");
			put({0xE3, (uint8_t)(tt | c7), cc, d0, d1}); count("E4 6 gain -> E3"); break;
		}
		case 0x7: put({0x4C, c7, 0x01}); put({0x4E, c7, 0x04}); count("E4 7 reset -> 4C + 4E"); break;
		case 0x8:
		{
			uint32_t n = ((uint32_t)aa << 16) | dddd;
			do
			{
				uint16_t k = (uint16_t)(n > 0xFFFF ? 0xFFFF : n);
				put({0xCD, c7, (uint8_t)(k >> 8), (uint8_t)k});
				n -= k;
			} while (n);
			count("E4 8 pre-run -> CD"); break;
		}
		default: fail("unknown ZSG-2 extension E4 " + std::to_string(ss));
		}
	}

	// returns the new offset (in the command stream) of every old command offset, for the loop point
	void run(size_t start, size_t end, std::map<size_t, size_t>& ofsMap)
	{
		size_t p = start;
		while (p < end)
		{
			ofsMap[p] = out.size();
			uint8_t o = in[p];
			if (o == 0x67)
			{
				if (p + 7 > end) fail("truncated data block");
				uint8_t type = in[p + 2];
				uint32_t szf = rd32(in, p + 3), sz = szf & 0x7FFFFFFF;
				if (p + 7 + sz > end) fail("truncated data block");
				if (type == 0x90 && es && sz >= 8)
				{
					uint32_t memsize = rd32(in, p + 7), ofs = rd32(in, p + 11);
					uint8_t region = (ofs >> 28) & 3;
					bool is8 = ofs >> 31;
					ofs &= 0x0FFFFFFF;
					uint8_t nt = region ? (uint8_t)(0x98 + region) : 0x90;
					Bytes data(in.begin() + p + 15, in.begin() + p + 7 + sz);
					if (is8)
					{
						Bytes w(data.size() * 2);
						for (size_t i = 0; i < data.size(); i++) { w[2 * i] = 0; w[2 * i + 1] = data[i]; }
						data.swap(w);
						memsize *= 2; ofs *= 2;
						count("block 90 8-bit -> 16-bit");
					}
					if (region) count("block 90 region -> 99..9B");
					uint32_t nsz = 8 + (uint32_t)data.size();
					put({0x67, 0x66, nt});
					size_t q = out.size(); out.resize(q + 12);
					wr32(out, q, nsz | (szf & 0x80000000)); wr32(out, q + 4, memsize); wr32(out, q + 8, ofs);
					out.insert(out.end(), data.begin(), data.end());
				}
				else if (type == 0x98 && zsg && sz >= 8)
				{
					uint32_t ofs = rd32(in, p + 11);
					uint32_t dlen = sz - 8;
					put({0x67, 0x66, 0xE2});
					size_t q = out.size(); out.resize(q + 8);
					wr32(out, q, (4 + dlen) | (szf & 0x80000000)); wr32(out, q + 4, ofs);
					out.insert(out.end(), in.begin() + p + 15, in.begin() + p + 7 + sz);
					count("block 98 DSP state -> E2");
				}
				else
					out.insert(out.end(), in.begin() + p, in.begin() + p + 7 + sz);
				p += 7 + sz;
				continue;
			}
			size_t l = cmd_len(o);
			if (!l) { char b[64]; snprintf(b, sizeof b, "unknown command 0x%02X at 0x%zX", o, p); fail(b); }
			if (p + l > end) fail("truncated command");
			const uint8_t* c = &in[p];
			if (es && o == 0xBE && (c[1] & 0x7F) >= 0x40) { put({0x49, (uint8_t)((c[1] & 0x9F)), c[2]}); count("BE 40+v bank -> 49"); }
			else if (es && o == 0xD6 && (c[1] & 0x7F) >= 0x40)
			{
				if (c[2]) fail("ES5506 16-bit bank write beyond 255M words");
				put({0x49, (uint8_t)(c[1] & 0x9F), c[3]}); count("D6 40+v bank -> 49");
			}
			else if (es && o == 0xD7) es_d7(c[1], c[2], c[3]);
			else if (es && o == 0xD8) es_d8(c[1], c[2], c[3]);
			else if (es && o == 0xE3) es_e3(c[1], c[2], c[3], c[4]);
			else if (zsg && o == 0xE4) zsg_e4(c[1], c[2], c[3], c[4]);
			else if ((o >= 0x49 && o <= 0x4E) || o == 0xCC || o == 0xCD || o == 0xE2 || (!es && (o == 0xD7 || o == 0xD8 || o == 0xE3)) || (!zsg && o == 0xE4))
			{ char b[64]; snprintf(b, sizeof b, "command 0x%02X at 0x%zX has no v1 meaning", o, p); fail(b); }
			else out.insert(out.end(), c, c + l);
			p += l;
			if (o == 0x66) break;
		}
		ofsMap[p] = out.size();
	}
};

int main(int argc, char** argv)
{
	std::string inPath, outPath;
	bool force = false;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--force")) force = true;
		else if (inPath.empty()) inPath = argv[i];
		else if (outPath.empty()) outPath = argv[i];
		else { fprintf(stderr, "too many arguments\n"); return 1; }
	}
	if (outPath.empty()) { fprintf(stderr, "usage: vgm_v3conv input.vg[mz] output.vg[mz] [--force]\n"); return 1; }
	try
	{
		Bytes d = load(inPath);
		if (d.size() < 0x40 || memcmp(d.data(), "Vgm ", 4)) fail("not a VGM file");
		uint32_t ver = rd32(d, 0x08);
		size_t dataOfs = ver >= 0x150 && rd32(d, 0x34) ? 0x34 + rd32(d, 0x34) : 0x40;
		size_t eof = 0x04 + (size_t)rd32(d, 0x04);
		if (eof > d.size()) eof = d.size();
		size_t gd3 = rd32(d, 0x14) ? 0x14 + rd32(d, 0x14) : 0;
		size_t loop = rd32(d, 0x1C) ? 0x1C + rd32(d, 0x1C) : 0;
		if (dataOfs > 0xBC + 4 && rd32(d, 0xBC)) fail("extra header present: not handled");
		uint32_t esClk = dataOfs >= 0xD4 ? rd32(d, 0xD0) : 0;
		uint32_t zsgClk = dataOfs >= 0x104 ? rd32(d, 0x100) : 0;

		Conv cv(d);
		cv.es = esClk != 0;
		cv.zsg = zsgClk != 0 && dataOfs == 0x110;   // v1 ZSG-2 header (v3 / PR #151 headers are >= 0x120)
		if (zsgClk && !cv.zsg && !force) { fprintf(stderr, "%s: clock at 0x100 with a 0x%zX header: not a v1 ZSG-2 file\n", inPath.c_str(), dataOfs); return 1; }
		if (dataOfs >= 0x120 && !force)
		{
			save(outPath, d);
			fprintf(stderr, "%s: header 0x%zX, already v3 (copied)\n", inPath.c_str(), dataOfs);
			return 2;
		}
		if (!cv.es && !cv.zsg)
		{
			save(outPath, d);
			fprintf(stderr, "%s: no ES5505/ES5506 or ZSG-2, copied unchanged\n", inPath.c_str());
			return 2;
		}

		size_t dataEnd = gd3 && gd3 > dataOfs && gd3 <= eof ? gd3 : eof;
		std::map<size_t, size_t> ofsMap;
		if (cv.zsg)   // board wiring of the Taito Zoom board, explicit (the player uses it by default)
			for (uint8_t i = 0; i < 4; i++) cv.put({0xE2, 0x36, i, 0x34, i});
		cv.run(dataOfs, dataEnd, ofsMap);

		const size_t HDR = 0x120;
		Bytes o(HDR, 0);
		memcpy(o.data(), d.data(), dataOfs < HDR ? dataOfs : HDR);
		for (size_t i = dataOfs; i < HDR; i++) o[i] = 0;
		if (rd32(o, 0x08) > 0x172) wr32(o, 0x08, 0x172);
		wr32(o, 0x34, HDR - 0x34);
		if (cv.zsg)
		{
			wr32(o, 0x100, 0);
			wr32(o, 0x110, zsgClk);
			wr32(o, 0x118, ((zsgClk & 0x3FFFFFFF) / 2) | (zsgClk & 0x40000000));
		}
		if (cv.espUsed[0] || cv.espUsed[1])
			wr32(o, 0x114, 10000000 | (cv.espUsed[1] ? 0x40000000 : 0));
		o[0x11C] = cv.dramLog2;
		if (loop)
		{
			auto it = ofsMap.find(loop);
			if (it == ofsMap.end()) fail("loop offset is not on a command boundary");
			wr32(o, 0x1C, (uint32_t)(HDR + it->second - 0x1C));
		}
		o.insert(o.end(), cv.out.begin(), cv.out.end());
		if (gd3 && gd3 + 12 <= d.size() && !memcmp(&d[gd3], "Gd3 ", 4))
		{
			size_t glen = 12 + rd32(d, gd3 + 8);
			wr32(o, 0x14, (uint32_t)(o.size() - 0x14));
			o.insert(o.end(), d.begin() + gd3, d.begin() + std::min(d.size(), gd3 + glen));
		}
		else
			wr32(o, 0x14, 0);
		wr32(o, 0x04, (uint32_t)(o.size() - 0x04));
		save(outPath, o);

		fprintf(stderr, "%s: %s v1 -> v3\n", inPath.c_str(), cv.es ? "ES55xx" : "ZSG-2");
		for (auto& kv : cv.stats) fprintf(stderr, "  %-44s %u\n", kv.first.c_str(), kv.second);
		return 0;
	}
	catch (const std::exception& e)
	{
		fprintf(stderr, "%s: error: %s\n", inPath.c_str(), e.what());
		return 1;
	}
}
