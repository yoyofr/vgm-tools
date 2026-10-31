// vgm_v3conv: converts VGM files of the first ES55xx / ZSG-2 encoding (v1, what the MAME logger and the
// libvgm "es55xx" branch wrote until 2026-10) to the v3 encoding of VGM-proposal-v3-allocation.md.
//
// usage: vgm_v3conv input.vg[mz] output.vg[mz] [--force | --to-v1]
//   output gzipped when its name ends in .vgz. Exit code 0 = converted, 2 = nothing to convert (the
//   output is a plain copy), 1 = error (unrepresentable v1 command, broken file...).
//   --force: convert even when the header does not look like a v1 file.
//   --to-v1: the reverse conversion (v3 -> v1), as vgm_trim does internally for v3 input.
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

#include "vgm_v3.h"

using namespace vgmv3;

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

int main(int argc, char** argv)
{
	std::string inPath, outPath;
	bool force = false, toV1 = false;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--force")) force = true;
		else if (!strcmp(argv[i], "--to-v1")) toV1 = true;
		else if (inPath.empty()) inPath = argv[i];
		else if (outPath.empty()) outPath = argv[i];
		else { fprintf(stderr, "too many arguments\n"); return 1; }
	}
	if (outPath.empty()) { fprintf(stderr, "usage: vgm_v3conv input.vg[mz] output.vg[mz] [--force | --to-v1]\n"); return 1; }
	try
	{
		Bytes d = load(inPath);
		if (toV1)
		{
			if (!is_v3_ext(d)) { fprintf(stderr, "%s: not a v3 ES55xx / ZSG-2 file\n", inPath.c_str()); return 1; }
			save(outPath, to_v1(d));
			fprintf(stderr, "%s: v3 -> v1\n", inPath.c_str());
			return 0;
		}
		int kind;
		std::map<std::string, unsigned> stats;
		Bytes o = to_v3(d, kind, &stats, force);
		save(outPath, o);
		if (kind == NONE)
		{
			if (layout(d).dataOfs >= 0x120) fprintf(stderr, "%s: header 0x%zX, already v3 (copied)\n", inPath.c_str(), layout(d).dataOfs);
			else fprintf(stderr, "%s: no ES5505/ES5506 or ZSG-2, copied unchanged\n", inPath.c_str());
			return 2;
		}
		fprintf(stderr, "%s: %s v1 -> v3\n", inPath.c_str(), kind == ES ? "ES55xx" : "ZSG-2");
		for (auto& kv : stats) fprintf(stderr, "  %-44s %u\n", kv.first.c_str(), kv.second);
		return 0;
	}
	catch (const std::exception& e)
	{
		fprintf(stderr, "%s: error: %s\n", inPath.c_str(), e.what());
		return 1;
	}
}
