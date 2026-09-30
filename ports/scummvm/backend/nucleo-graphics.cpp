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

#include "common/scummsys.h"

#ifdef NUCLEO_BACKEND

#include "backends/platform/nucleo/nucleo-graphics.h"
#include "backends/platform/nucleo/nucleo-imports.h"
#include "common/textconsole.h"

static const Graphics::PixelFormat kFormat565(2, 5, 6, 5, 0, 11, 5, 0, 0);

static inline uint16 rgbTo565(byte r, byte g, byte b) {
	return (uint16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

NucleoGraphicsManager::NucleoGraphicsManager() :
	_screenChangeID(0), _shakeX(0), _shakeY(0), _gx(0), _gy(0), _gw(0), _gh(0),
	_mapX(nullptr), _mapY(nullptr), _overlayVisible(false), _dirty(true), _alive(true),
	_cursor(nullptr), _cursorMask(nullptr), _cursorW(0), _cursorH(0), _hotX(0), _hotY(0),
	_keycolor(0), _cursorDontScale(false), _cursorPaletteEnabled(false), _cursorVisible(false),
	_mouseX(0), _mouseY(0) {
	_canvasW = nv_gfx_width();
	_canvasH = nv_gfx_height();
	if (_canvasW <= 0 || _canvasH <= 0) {   // no canvas: the manifest lacks gfx / canvas_w,h
		_canvasW = 320;
		_canvasH = 200;
	}
	_frame = new uint16[_canvasW * _canvasH]();
	_overlay = new uint16[_canvasW * _canvasH]();
	_screenFormat = _pendingFormat = Graphics::PixelFormat::createFormatCLUT8();
	_pendingW = _canvasW;
	_pendingH = _canvasH;
	memset(_palRGB, 0, sizeof(_palRGB));
	memset(_pal565, 0, sizeof(_pal565));
	memset(_cursorPal565, 0, sizeof(_cursorPal565));
	_cursorFormat = Graphics::PixelFormat::createFormatCLUT8();
	endGFXTransaction();
}

NucleoGraphicsManager::~NucleoGraphicsManager() {
	_screen.free();
	delete[] _frame;
	delete[] _overlay;
	delete[] _mapX;
	delete[] _mapY;
	delete[] _cursor;
	delete[] _cursorMask;
}

bool NucleoGraphicsManager::hasFeature(OSystem::Feature f) const {
	return f == OSystem::kFeatureCursorPalette;
}

void NucleoGraphicsManager::setFeatureState(OSystem::Feature f, bool enable) {
	if (f == OSystem::kFeatureCursorPalette) {
		_cursorPaletteEnabled = enable;
		_dirty = true;
	}
}

bool NucleoGraphicsManager::getFeatureState(OSystem::Feature f) const {
	return f == OSystem::kFeatureCursorPalette && _cursorPaletteEnabled;
}

#ifdef USE_RGB_COLOR
Common::List<Graphics::PixelFormat> NucleoGraphicsManager::getSupportedFormats() const {
	Common::List<Graphics::PixelFormat> list;
	list.push_back(kFormat565);
	list.push_back(Graphics::PixelFormat::createFormatCLUT8());
	return list;
}
#endif

void NucleoGraphicsManager::initSize(uint width, uint height, const Graphics::PixelFormat *format) {
	_pendingW = width;
	_pendingH = height;
	_pendingFormat = format ? *format : Graphics::PixelFormat::createFormatCLUT8();
}

OSystem::TransactionError NucleoGraphicsManager::endGFXTransaction() {
	int err = OSystem::kTransactionSuccess;
	if (_pendingFormat.bytesPerPixel != 1 && _pendingFormat != kFormat565) {
		warning("nucleo: pixel format %s not supported, using CLUT8", _pendingFormat.toString().c_str());
		_pendingFormat = Graphics::PixelFormat::createFormatCLUT8();
		err |= OSystem::kTransactionFormatNotSupported;
	}
	if (_screen.w != (int)_pendingW || _screen.h != (int)_pendingH || _screen.format != _pendingFormat ||
	    !_screen.getPixels()) {
		_screen.free();
		_screen.create(_pendingW, _pendingH, _pendingFormat);
		_screenFormat = _pendingFormat;
		_screenChangeID++;
		computeGameRect();
		_dirty = true;
	}
	return (OSystem::TransactionError)err;
}

// Fit the game screen into the canvas keeping its pixel aspect: integer scale when it fits whole
// (320x200 -> 1x), otherwise the largest ratio that fits, centered with black bars.
void NucleoGraphicsManager::computeGameRect() {
	const int sw = _screen.w, sh = _screen.h;
	if (sw <= 0 || sh <= 0) {
		_gw = _gh = 0;
		return;
	}
	if (sw <= _canvasW && sh <= _canvasH) {
		_gw = sw;
		_gh = sh;
	} else if ((int64)sw * _canvasH >= (int64)sh * _canvasW) {
		_gw = _canvasW;
		_gh = (int)((int64)sh * _canvasW / sw);
	} else {
		_gh = _canvasH;
		_gw = (int)((int64)sw * _canvasH / sh);
	}
	_gx = (_canvasW - _gw) / 2;
	_gy = (_canvasH - _gh) / 2;
	delete[] _mapX;
	delete[] _mapY;
	_mapX = new int[_gw];
	_mapY = new int[_gh];
	for (int x = 0; x < _gw; x++)
		_mapX[x] = x * sw / _gw;
	for (int y = 0; y < _gh; y++)
		_mapY[y] = y * sh / _gh;
}

void NucleoGraphicsManager::setPalette(const byte *colors, uint start, uint num) {
	assert(start + num <= 256);
	memcpy(_palRGB + start * 3, colors, num * 3);
	for (uint i = 0; i < num; i++)
		_pal565[start + i] = rgbTo565(colors[i * 3], colors[i * 3 + 1], colors[i * 3 + 2]);
	_dirty = true;
}

void NucleoGraphicsManager::grabPalette(byte *colors, uint start, uint num) const {
	assert(start + num <= 256);
	memcpy(colors, _palRGB + start * 3, num * 3);
}

void NucleoGraphicsManager::setCursorPalette(const byte *colors, uint start, uint num) {
	assert(start + num <= 256);
	for (uint i = 0; i < num; i++)
		_cursorPal565[start + i] = rgbTo565(colors[i * 3], colors[i * 3 + 1], colors[i * 3 + 2]);
	_cursorPaletteEnabled = true;
	_dirty = true;
}

void NucleoGraphicsManager::copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) {
	_screen.copyRectToSurface(buf, pitch, x, y, w, h);
	_dirty = true;
}

void NucleoGraphicsManager::fillScreen(uint32 col) {
	_screen.fillRect(Common::Rect(_screen.w, _screen.h), col);
	_dirty = true;
}

void NucleoGraphicsManager::fillScreen(const Common::Rect &r, uint32 col) {
	Common::Rect c = r;
	c.clip(Common::Rect(_screen.w, _screen.h));
	if (!c.isEmpty())
		_screen.fillRect(c, col);
	_dirty = true;
}

void NucleoGraphicsManager::setShakePos(int shakeXOffset, int shakeYOffset) {
	if (_shakeX != shakeXOffset || _shakeY != shakeYOffset) {
		_shakeX = shakeXOffset;
		_shakeY = shakeYOffset;
		_dirty = true;
	}
}

// Game screen -> RGB565 canvas-sized buffer (bars black), with the shake offset applied.
void NucleoGraphicsManager::composeGame(uint16 *dst) const {
	const int cw = _canvasW, ch = _canvasH;
	if (_gw != cw || _gh != ch || _shakeX || _shakeY)
		memset(dst, 0, cw * ch * sizeof(uint16));
	if (!_gw || !_gh || !_screen.getPixels())
		return;
	const bool direct = _gw == _screen.w && _gh == _screen.h;
	const bool clut = _screen.format.bytesPerPixel == 1;
	for (int y = 0; y < _gh; y++) {
		const int dy = _gy + y + _shakeY;
		if (dy < 0 || dy >= ch)
			continue;
		const int sy = direct ? y : _mapY[y];
		uint16 *d = dst + dy * cw;
		int x0 = 0, x1 = _gw;
		if (_gx + _shakeX < 0)
			x0 = -(_gx + _shakeX);
		if (_gx + _shakeX + x1 > cw)
			x1 = cw - _gx - _shakeX;
		d += _gx + _shakeX;
		if (clut) {
			const byte *s = (const byte *)_screen.getBasePtr(0, sy);
			if (direct)
				for (int x = x0; x < x1; x++) d[x] = _pal565[s[x]];
			else
				for (int x = x0; x < x1; x++) d[x] = _pal565[s[_mapX[x]]];
		} else {
			const uint16 *s = (const uint16 *)_screen.getBasePtr(0, sy);
			if (direct)
				memcpy(d + x0, s + x0, (x1 - x0) * sizeof(uint16));
			else
				for (int x = x0; x < x1; x++) d[x] = s[_mapX[x]];
		}
	}
}

uint16 NucleoGraphicsManager::cursorColor(uint32 v) const {
	if (_cursorFormat.bytesPerPixel == 1)
		return _cursorPaletteEnabled ? _cursorPal565[v & 0xFF] : _pal565[v & 0xFF];
	if (_cursorFormat == kFormat565)
		return (uint16)v;
	byte r, g, b;
	_cursorFormat.colorToRGB(v, r, g, b);
	return rgbTo565(r, g, b);
}

void NucleoGraphicsManager::drawCursor(uint16 *dst) const {
	if (!_cursorVisible || !_cursor || !_cursorW || !_cursorH)
		return;
	// The cursor lives in the space the mouse moves in: overlay (canvas 1:1) or game screen.
	int cx = _mouseX, cy = _mouseY, num = 1, den = 1;
	if (!_overlayVisible && _screen.w > 0) {
		num = _gw;
		den = _screen.w;
		cx = _gx + _mouseX * _gw / _screen.w;
		cy = _gy + _mouseY * _gh / _screen.h;
	}
	const bool scale = !_overlayVisible && !_cursorDontScale && num != den;
	const int dw = scale ? (int)_cursorW * num / den : (int)_cursorW;
	const int dh = scale ? (int)_cursorH * num / den : (int)_cursorH;
	const int hx = scale ? _hotX * num / den : _hotX;
	const int hy = scale ? _hotY * num / den : _hotY;
	const int bpp = _cursorFormat.bytesPerPixel;
	for (int y = 0; y < dh; y++) {
		const int py = cy - hy + y;
		if (py < 0 || py >= _canvasH)
			continue;
		const int sy = scale ? y * den / num : y;
		for (int x = 0; x < dw; x++) {
			const int px = cx - hx + x;
			if (px < 0 || px >= _canvasW)
				continue;
			const int sx = scale ? x * den / num : x;
			const int i = sy * _cursorW + sx;
			uint32 v;
			switch (bpp) {
			case 1: v = _cursor[i]; break;
			case 2: v = ((const uint16 *)_cursor)[i]; break;
			default: v = ((const uint32 *)_cursor)[i]; break;
			}
			if (_cursorMask) {
				if (_cursorMask[i] == kCursorMaskTransparent)
					continue;
				if (_cursorMask[i] == kCursorMaskInvert) {
					dst[py * _canvasW + px] ^= 0xFFFF;
					continue;
				}
			} else if (v == _keycolor) {
				continue;
			}
			dst[py * _canvasW + px] = cursorColor(v);
		}
	}
}

void NucleoGraphicsManager::present() {
	if (_overlayVisible)
		memcpy(_frame, _overlay, _canvasW * _canvasH * sizeof(uint16));
	else
		composeGame(_frame);
	drawCursor(_frame);
	nv_gfx_blit_raw(_frame, _canvasW * _canvasH * 2, 0, 0, _canvasW, _canvasH);
	if (!nv_gfx_present())
		_alive = false;
}

void NucleoGraphicsManager::updateScreen() {
	if (!_dirty)
		return;
	_dirty = false;
	present();
}

void NucleoGraphicsManager::idlePresent() {
	if (_dirty)
		updateScreen();
	else if (!nv_gfx_present())   // nothing drawn: the OS just paces ~16 ms and feeds its watchdog
		_alive = false;
}

void NucleoGraphicsManager::showOverlay(bool inGUI) {
	if (_overlayVisible)
		return;
	_overlayVisible = true;
	// Mouse coordinates switch to the overlay space.
	if (_screen.w > 0) {
		_mouseX = _gx + _mouseX * _gw / _screen.w;
		_mouseY = _gy + _mouseY * _gh / _screen.h;
	}
	_dirty = true;
}

void NucleoGraphicsManager::hideOverlay() {
	if (!_overlayVisible)
		return;
	_overlayVisible = false;
	int sx, sy;
	canvasToScreen(_mouseX, _mouseY, sx, sy);
	_mouseX = sx;
	_mouseY = sy;
	_dirty = true;
}

Graphics::PixelFormat NucleoGraphicsManager::getOverlayFormat() const {
	return kFormat565;
}

// ScummVM convention: clearing the overlay shows the game screen behind the GUI.
void NucleoGraphicsManager::clearOverlay() {
	composeGame(_overlay);
	_dirty = true;
}

void NucleoGraphicsManager::grabOverlay(Graphics::Surface &surface) const {
	assert(surface.format.bytesPerPixel == 2);
	const int w = MIN<int>(surface.w, _canvasW), h = MIN<int>(surface.h, _canvasH);
	for (int y = 0; y < h; y++)
		memcpy(surface.getBasePtr(0, y), _overlay + y * _canvasW, w * 2);
}

void NucleoGraphicsManager::copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) {
	const byte *src = (const byte *)buf;
	if (x < 0) { w += x; src -= x * 2; x = 0; }
	if (y < 0) { h += y; src -= y * pitch; y = 0; }
	if (x + w > _canvasW) w = _canvasW - x;
	if (y + h > _canvasH) h = _canvasH - y;
	if (w <= 0 || h <= 0)
		return;
	for (int r = 0; r < h; r++)
		memcpy(_overlay + (y + r) * _canvasW + x, src + r * pitch, w * 2);
	_dirty = true;
}

bool NucleoGraphicsManager::showMouse(bool visible) {
	const bool last = _cursorVisible;
	if (visible != _cursorVisible) {
		_cursorVisible = visible;
		_dirty = true;
	}
	return last;
}

void NucleoGraphicsManager::warpMouse(int x, int y) {
	setMousePos(x, y);
}

void NucleoGraphicsManager::setMousePos(int x, int y) {
	if (x != _mouseX || y != _mouseY) {
		_mouseX = x;
		_mouseY = y;
		if (_cursorVisible)
			_dirty = true;
	}
}

void NucleoGraphicsManager::setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY,
                                           uint32 keycolor, bool dontScale,
                                           const Graphics::PixelFormat *format, const byte *mask) {
	_cursorFormat = format ? *format : Graphics::PixelFormat::createFormatCLUT8();
	const uint bpp = _cursorFormat.bytesPerPixel;
	delete[] _cursor;
	delete[] _cursorMask;
	_cursor = nullptr;
	_cursorMask = nullptr;
	_cursorW = w;
	_cursorH = h;
	if (buf && w && h) {
		_cursor = new byte[w * h * bpp];
		memcpy(_cursor, buf, w * h * bpp);
		if (mask) {
			_cursorMask = new byte[w * h];
			memcpy(_cursorMask, mask, w * h);
		}
	}
	_hotX = hotspotX;
	_hotY = hotspotY;
	_keycolor = keycolor;
	_cursorDontScale = dontScale;
	_dirty = true;
}

void NucleoGraphicsManager::canvasToScreen(int cx, int cy, int &sx, int &sy) const {
	if (_overlayVisible || _gw <= 0 || _gh <= 0) {
		sx = CLIP(cx, 0, _canvasW - 1);
		sy = CLIP(cy, 0, _canvasH - 1);
		return;
	}
	sx = CLIP((cx - _gx) * _screen.w / _gw, 0, _screen.w - 1);
	sy = CLIP((cy - _gy) * _screen.h / _gh, 0, _screen.h - 1);
}

#endif
