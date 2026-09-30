/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// Plain stdio / POSIX file I/O (the installer runs before ScummVM's own file layer is set up).
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <zlib.h>

#include "common/scummsys.h"

#ifdef NUCLEO_BACKEND

#include "backends/platform/nucleo/nucleo-installer.h"
#include "backends/platform/nucleo/nucleo-games.h"
#include "backends/platform/nucleo/nucleo-imports.h"
#include "graphics/fontman.h"
#include "graphics/font.h"
#include "graphics/surface.h"
#include "common/array.h"
#include "common/textconsole.h"

namespace {

Common::String dataPath(const char *leaf) { return Common::String(nucleoDataDir()) + leaf; }
const uint32 kChunk = 960 * 1024;    // one ranged request (the OS buffers at most 1 MB per handle)

// ---- SHA-256 (FIPS 180-4) -------------------------------------------------------------------------

struct Sha256 {
	uint32 h[8];
	byte buf[64];
	uint64 len;
	uint32 fill;

	Sha256() { reset(); }
	void reset() {
		static const uint32 init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
		memcpy(h, init, sizeof(h));
		len = 0;
		fill = 0;
	}
	static uint32 ror(uint32 x, int n) { return (x >> n) | (x << (32 - n)); }
	void block(const byte *p) {
		static const uint32 k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
		uint32 w[64];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32)p[i * 4] << 24 | (uint32)p[i * 4 + 1] << 16 | (uint32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
		for (int i = 16; i < 64; i++) {
			const uint32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++) {
			const uint32 t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32 t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}
	void update(const byte *p, uint32 n) {
		len += n;
		while (n) {
			const uint32 take = MIN<uint32>(n, 64 - fill);
			memcpy(buf + fill, p, take);
			fill += take;
			p += take;
			n -= take;
			if (fill == 64) {
				block(buf);
				fill = 0;
			}
		}
	}
	Common::String hex() {
		const uint64 bits = len * 8;
		const byte pad = 0x80, zero = 0;
		update(&pad, 1);
		while (fill != 56)
			update(&zero, 1);
		byte l[8];
		for (int i = 0; i < 8; i++)
			l[i] = (byte)(bits >> (56 - i * 8));
		update(l, 8);
		Common::String s;
		for (int i = 0; i < 8; i++)
			s += Common::String::format("%08x", h[i]);
		return s;
	}
};

// ---- UI -------------------------------------------------------------------------------------------

bool g_italian = false;
const char *tr(const char *en, const char *it) { return g_italian ? it : en; }

struct Screen {
	Graphics::Surface s;
	const Graphics::Font *big, *small;
	Screen() {
		s.create(nv_gfx_width() > 0 ? nv_gfx_width() : 320, nv_gfx_height() > 0 ? nv_gfx_height() : 200,
		         Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0));
		big = FontMan.getFontByUsage(Graphics::FontManager::kBigGUIFont);
		small = FontMan.getFontByUsage(Graphics::FontManager::kGUIFont);
	}
	~Screen() { s.free(); }
	static uint16 rgb(int r, int g, int b) { return (uint16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

	// title, line1, line2, progress (0..1000 or -1), hint
	bool draw(const Common::String &title, const Common::String &l1, const Common::String &l2, int permille,
	          const Common::String &hint) {
		const int w = s.w;
		s.fillRect(Common::Rect(w, s.h), rgb(16, 18, 24));
		s.fillRect(Common::Rect(0, 0, w, 36), rgb(200, 100, 0));
		big->drawString(&s, title, 8, 12, w - 16, rgb(255, 255, 255), Graphics::kTextAlignCenter, 0, true);
		small->drawString(&s, l1, 8, 58, w - 16, rgb(230, 230, 230), Graphics::kTextAlignCenter, 0, true);
		small->drawString(&s, l2, 8, 76, w - 16, rgb(160, 170, 180), Graphics::kTextAlignCenter, 0, true);
		if (permille >= 0) {
			const Common::Rect bar(24, 100, w - 24, 112);
			s.frameRect(bar, rgb(120, 130, 140));
			const int fillW = (bar.width() - 4) * MIN(permille, 1000) / 1000;
			if (fillW > 0)
				s.fillRect(Common::Rect(bar.left + 2, bar.top + 2, bar.left + 2 + fillW, bar.bottom - 2), rgb(0, 200, 60));
		}
		small->drawString(&s, hint, 8, s.h - 40, w - 16, rgb(140, 150, 160), Graphics::kTextAlignCenter, 0, true);
		small->drawString(&s, tr("Freeware game, original files from scummvm.org",
		                         "Gioco freeware, file originali da scummvm.org"),
		                  8, s.h - 20, w - 16, rgb(100, 110, 120), Graphics::kTextAlignCenter, 0, true);
		nv_gfx_blit_raw(s.getPixels(), s.w * s.h * 2, 0, 0, s.w, s.h);
		return nv_gfx_present() != 0;
	}
};

bool userLeft() {
	return nv_gfx_back() > 0;
}

// ---- files ----------------------------------------------------------------------------------------

void mkdirs(const Common::String &path) {
	for (uint i = 1; i < path.size(); i++) {
		if (path[i] == '/')
			mkdir(path.substr(0, i).c_str(), 0777);
	}
	mkdir(path.c_str(), 0777);
}

long fileSize(const char *p) {
	struct stat st;
	return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

Common::String readSmall(const char *p) {
	FILE *f = fopen(p, "rb");
	if (!f)
		return Common::String();
	char b[256];
	const size_t n = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[n] = 0;
	Common::String s(b);
	s.trim();
	return s;
}

bool writeSmall(const char *p, const Common::String &v) {
	FILE *f = fopen(p, "wb");
	if (!f)
		return false;
	fwrite(v.c_str(), 1, v.size(), f);
	fclose(f);
	return true;
}

void removeTree(const Common::String &path);

Common::String baseName(const char *url) {
	const char *s = strrchr(url, '/');
	return Common::String(s ? s + 1 : url);
}

// ---- download -------------------------------------------------------------------------------------

struct Progress {
	Screen *scr;
	Common::String title, what;
	uint64 done, total;
	uint32 lastDraw;
	bool alive;
	bool tick(bool force = false) {
		const uint32 now = nv_millis();
		if (!force && now - lastDraw < 250)
			return alive;
		lastDraw = now;
		const int pm = total ? (int)(done * 1000 / total) : 0;
		const Common::String mb = Common::String::format("%.1f / %.1f MB", done / 1048576.0, total / 1048576.0);
		if (!scr->draw(title, what, mb, pm, tr("Back gesture: stop (the download resumes later)",
		                                       "Gesto indietro: ferma (il download riprende dopo)")) || userLeft())
			alive = false;
		return alive;
	}
};

// One ranged GET into buf. Returns bytes, 0 = retry, -1 = fatal, -2 = user left.
int32 fetchRange(const char *url, uint32 from, uint32 len, byte *buf, Progress &pr) {
	const Common::String spec = Common::String::format(
		"{\"url\":\"%s\",\"headers\":{\"Range\":\"bytes=%u-%u\"},\"timeout\":30000,\"max\":%u}",
		url, from, from + len - 1, (unsigned)(len + 1024));
	const int32 h = nv_http_req(spec.c_str(), nullptr, 0);
	if (h < 0)
		return h == -1 ? -1 : 0;
	int32 st;
	while ((st = nv_http_state(h)) == 0) {
		if (!pr.tick()) {
			nv_http_close(h);
			return -2;
		}
		usleep(20000);
	}
	int32 got = 0;
	const int32 status = nv_http_status(h);
	if (st == 1 && (status == 206 || (status == 200 && from == 0))) {
		for (;;) {
			const int32 n = nv_http_read(h, buf + got, len - got);
			if (n <= 0)
				break;
			got += n;
			if ((uint32)got >= len)
				break;
		}
	} else {
		warning("nucleo: %s bytes %u: state %d status %d", url, from, st, status);
	}
	nv_http_close(h);
	return got;
}

// Download (resuming) + SHA-256. True when the file is complete and verified.
bool download(const NucleoFile &f, const Common::String &path, Progress &pr, Common::String &err) {
	static byte *buf = new byte[kChunk];
	Sha256 sha;
	long have = fileSize(path.c_str());
	if (have > (long)f.size) {
		unlink(path.c_str());
		have = -1;
	}
	if (have > 0) {   // re-hash what is already there, then continue after it
		FILE *in = fopen(path.c_str(), "rb");
		size_t n;
		while (in && (n = fread(buf, 1, kChunk, in)) > 0) {
			sha.update(buf, n);
			pr.done += n;
			if (!pr.tick())
				break;
		}
		if (in)
			fclose(in);
		if (!pr.alive)
			return false;
	} else {
		have = 0;
	}
	FILE *out = fopen(path.c_str(), have ? "ab" : "wb");
	if (!out) {
		err = tr("Cannot write to the SD card", "Impossibile scrivere sulla SD");
		return false;
	}
	int retries = 0;
	uint32 pos = (uint32)have;
	while (pos < f.size) {
		const uint32 want = MIN<uint32>(kChunk, f.size - pos);
		const int32 got = fetchRange(f.url, pos, want, buf, pr);
		if (got == -2)
			break;
		if (got == -1) {
			err = tr("No network permission", "Permesso rete mancante");
			break;
		}
		if (got <= 0 || (uint32)got != want) {
			if (++retries > 6) {
				err = tr("Download failed: check Wi-Fi", "Download non riuscito: controlla il Wi-Fi");
				break;
			}
			const uint32 until = nv_millis() + 1000 * retries;
			while (nv_millis() < until && pr.tick())
				usleep(50000);
			if (!pr.alive)
				break;
			continue;
		}
		retries = 0;
		if (fwrite(buf, 1, got, out) != (size_t)got) {
			err = tr("SD card full?", "SD piena?");
			break;
		}
		sha.update(buf, got);
		pos += got;
		pr.done += got;
		if (!pr.tick())
			break;
	}
	fclose(out);
	if (pos < f.size)
		return false;
	if (sha.hex() != f.sha256) {
		unlink(path.c_str());
		err = tr("Corrupted download, try again", "Download corrotto, riprova");
		return false;
	}
	return true;
}

// ---- zip (streaming: members can be far bigger than the app's memory) --------------------------

uint32 le16(const byte *p) { return p[0] | p[1] << 8; }
uint32 le32(const byte *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32)p[3] << 24; }

// Finder metadata some archives carry ("__MACOSX/", "._name", ".DS_Store"): never extracted.
bool macJunk(const Common::String &name) {
	return name.hasPrefix("__MACOSX/") || name.contains("/._") || name.hasPrefix("._") ||
	       name.hasSuffix(".DS_Store");
}

bool unzip(const Common::String &zipPath, const Common::String &dest, Progress &pr, Common::String &err) {
	err = tr("Damaged archive", "Archivio danneggiato");
	FILE *z = fopen(zipPath.c_str(), "rb");
	if (!z)
		return false;
	fseek(z, 0, SEEK_END);
	const long zsize = ftell(z);
	// End of central directory: in the last 64 KB + 22 bytes.
	const long tail = MIN<long>(zsize, 65536 + 22);
	byte *t = new byte[tail];
	fseek(z, zsize - tail, SEEK_SET);
	bool ok = fread(t, 1, tail, z) == (size_t)tail;
	long eocd = -1;
	for (long i = tail - 22; ok && i >= 0; i--) {
		if (le32(t + i) == 0x06054b50) {
			eocd = i;
			break;
		}
	}
	uint32 entries = 0, cdSize = 0, cdOff = 0;
	if (eocd >= 0) {
		entries = le16(t + eocd + 10);
		cdSize = le32(t + eocd + 12);
		cdOff = le32(t + eocd + 16);
	}
	delete[] t;
	if (eocd < 0 || cdOff + cdSize > (uint32)zsize) {
		fclose(z);
		return false;
	}
	byte *cd = new byte[cdSize];
	fseek(z, cdOff, SEEK_SET);
	ok = fread(cd, 1, cdSize, z) == cdSize;

	// Strip a single top-level folder shared by every entry ("bass-cd-1.2/sky.dsk" -> "sky.dsk").
	Common::String prefix;
	bool first = true;
	for (uint32 i = 0, p = 0; ok && i < entries; i++) {
		if (p + 46 > cdSize || le32(cd + p) != 0x02014b50) {
			ok = false;
			break;
		}
		const uint32 nl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
		Common::String name((const char *)cd + p + 46, nl);
		p += 46 + nl + xl + cl;
		if (macJunk(name))
			continue;
		const int slash = name.findFirstOf('/');
		const Common::String top = slash > 0 ? name.substr(0, slash + 1) : Common::String();
		if (first) {
			prefix = top;
			first = false;
		} else if (top != prefix) {
			prefix.clear();
		}
	}

	static byte *in = new byte[65536];
	static byte *outb = new byte[65536];
	for (uint32 i = 0, p = 0; ok && i < entries; i++) {
		const uint32 method = le16(cd + p + 10), crc = le32(cd + p + 16);
		const uint32 csize = le32(cd + p + 20), usize = le32(cd + p + 24);
		const uint32 nl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
		const uint32 loff = le32(cd + p + 42);
		Common::String name((const char *)cd + p + 46, nl);
		p += 46 + nl + xl + cl;
		if (macJunk(name))
			continue;
		if (!prefix.empty() && name.hasPrefix(prefix))
			name = name.substr(prefix.size());
		if (name.empty() || name.lastChar() == '/' || name.contains("..") || name[0] == '/')
			continue;   // folders are created on demand; never write outside dest
		if (method != 0 && method != 8) {
			ok = false;
			break;
		}
		byte lh[30];
		fseek(z, loff, SEEK_SET);
		if (fread(lh, 1, 30, z) != 30 || le32(lh) != 0x04034b50) {
			ok = false;
			break;
		}
		fseek(z, loff + 30 + le16(lh + 26) + le16(lh + 28), SEEK_SET);
		const Common::String outPath = dest + "/" + name;
		const int ls = outPath.findLastOf('/');
		mkdirs(outPath.substr(0, ls));
		FILE *o = fopen(outPath.c_str(), "wb");
		if (!o) {
			err = tr("Cannot write to the SD card", "Impossibile scrivere sulla SD");
			ok = false;
			break;
		}
		pr.what = name;
		uLong c = crc32(0, Z_NULL, 0);
		uint32 left = csize;
		if (method == 0) {
			while (ok && left) {
				const uint32 n = fread(in, 1, MIN<uint32>(left, 65536), z);
				if (!n) { ok = false; break; }
				c = crc32(c, in, n);
				ok = fwrite(in, 1, n, o) == n;
				left -= n;
				pr.done += n;
				if (!pr.tick()) ok = false;
			}
		} else {
			z_stream zs;
			memset(&zs, 0, sizeof(zs));
			ok = inflateInit2(&zs, -MAX_WBITS) == Z_OK;
			int zr = Z_OK;
			while (ok && zr != Z_STREAM_END) {
				if (zs.avail_in == 0 && left) {
					const uint32 n = fread(in, 1, MIN<uint32>(left, 65536), z);
					if (!n) { ok = false; break; }
					left -= n;
					pr.done += n;
					zs.next_in = in;
					zs.avail_in = n;
				}
				zs.next_out = outb;
				zs.avail_out = 65536;
				zr = inflate(&zs, Z_NO_FLUSH);
				if (zr != Z_OK && zr != Z_STREAM_END) { ok = false; break; }
				const uint32 n = 65536 - zs.avail_out;
				c = crc32(c, outb, n);
				if (fwrite(outb, 1, n, o) != n) ok = false;
				if (!pr.tick()) ok = false;
			}
			inflateEnd(&zs);
			if (ok && zs.total_out != usize) ok = false;
		}
		fclose(o);
		if (ok && c != crc)
			ok = false;
		if (!ok)
			unlink(outPath.c_str());
	}
	delete[] cd;
	fclose(z);
	return ok;
}

}   // namespace

// ---- entry ----------------------------------------------------------------------------------------

bool nucleoInstallGame(const char *key, Common::String &gameid, Common::String &lang) {
	const NucleoGame *game = nullptr;
	for (uint i = 0; i < ARRAYSIZE(kNucleoGames); i++) {
		if (!strcmp(kNucleoGames[i].key, key))
			game = &kNucleoGames[i];
	}
	if (!game)
		return false;
	char ui[8] = {0};
	nv_lang(ui, sizeof(ui) - 1);
	g_italian = !strncmp(ui, "it", 2);

	// The variant the store page picked; without one (sideloaded), the device language or the first.
	Common::String want = readSmall(dataPath("/variant").c_str());
	const NucleoVariant *v = nullptr;
	for (int i = 0; i < game->nvariants; i++) {
		if (want == game->variants[i].id)
			v = &game->variants[i];
	}
	for (int i = 0; !v && i < game->nvariants; i++) {
		if (!strncmp(game->variants[i].id, ui, 2))
			v = &game->variants[i];
	}
	if (!v)
		v = &game->variants[0];
	gameid = game->gameid;
	lang = v->lang;

	// Installed = same file set (languages sharing one archive need no second download).
	Common::String need;
	for (int k = 0; k < 6 && v->files[k] >= 0; k++)
		need += Common::String(kNucleoFiles[v->files[k]].sha256, 12) + ",";
	if (readSmall(dataPath("/installed.sha").c_str()) == need)
		return true;

	Screen scr;
	Progress pr;
	pr.scr = &scr;
	pr.title = game->name;
	pr.done = pr.total = 0;
	pr.lastDraw = 0;
	pr.alive = true;
	for (int k = 0; k < 6 && v->files[k] >= 0; k++)
		pr.total += kNucleoFiles[v->files[k]].size;

	const Common::String gameDir = dataPath("/game"), dlDir = dataPath("/dl");
	mkdirs(dlDir);
	removeTree(gameDir);
	mkdirs(gameDir);
	unlink(dataPath("/installed.sha").c_str());
	Common::String err;
	bool ok = true;
	for (int k = 0; ok && k < 6 && v->files[k] >= 0; k++) {
		const NucleoFile &f = kNucleoFiles[v->files[k]];
		const Common::String name = baseName(f.url);
		const bool isZip = name.hasSuffixIgnoreCase(".zip");
		const Common::String dl = (isZip ? dlDir : gameDir) + "/" + name;
		pr.what = Common::String::format("%s %s", tr("Downloading", "Scarico"), name.c_str());
		ok = download(f, dl, pr, err);
		if (ok && isZip) {
			// Unpacking walks the archive again: count it as a second pass of the bar.
			const uint64 d0 = pr.done;
			pr.total += f.size;
			ok = unzip(dl, gameDir, pr, err);
			pr.done = d0 + f.size;
			if (ok)
				unlink(dl.c_str());
		}
	}
	if (ok) {
		writeSmall(dataPath("/installed.sha").c_str(), need);
		return true;
	}
	if (!pr.alive)
		return false;
	// Error screen: tap retries, back leaves.
	while (scr.draw(game->name, err, tr("Tap to try again", "Tocca per riprovare"), -1,
	                tr("Back gesture: exit", "Gesto indietro: esci"))) {
		if (userLeft())
			return false;
		if (nv_gfx_touch_count() > 0) {
			while (nv_gfx_touch_count() > 0 && nv_gfx_present())
				;
			return nucleoInstallGame(key, gameid, lang);
		}
	}
	return false;
}

namespace {
void removeTree(const Common::String &path) {
	// Only ever called on the game folder: remove its files and sub-folders (depth-first).
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return;
	if (!S_ISDIR(st.st_mode)) {
		unlink(path.c_str());
		return;
	}
	DIR *d = opendir(path.c_str());
	if (!d)
		return;
	struct dirent *e;
	Common::Array<Common::String> names;
	while ((e = readdir(d)) != nullptr) {
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			names.push_back(e->d_name);
	}
	closedir(d);
	for (uint i = 0; i < names.size(); i++)
		removeTree(path + "/" + names[i]);
	rmdir(path.c_str());
}
}   // namespace

// "/appdata" when the app has a private folder there (permission "home" puts the workspace on "/"),
// otherwise "/" itself is the private folder (engine packages run with "fs" only).
const char *nucleoDataDir() {
	static int has = -1;
	if (has < 0) {
		struct stat st;
		has = stat("/appdata", &st) == 0 && S_ISDIR(st.st_mode);
	}
	return has ? "/appdata" : "";
}

#endif
