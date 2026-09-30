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

#ifndef BACKENDS_PLATFORM_NUCLEO_GRAPHICS_H
#define BACKENDS_PLATFORM_NUCLEO_GRAPHICS_H

#include "backends/graphics/graphics.h"
#include "graphics/surface.h"

/*
 * NucleoOS graphics: the OS gives the app a fixed RGB565 canvas (manifest canvas_w x canvas_h,
 * 320x200) and scales it to the panel itself (canvas_scale "fit": exactly 3x, 960x600). Every
 * updateScreen composes the game screen (CLUT8 or RGB565), or the GUI overlay, plus the mouse
 * cursor into one canvas-sized frame, blits it and presents it. A game screen of another size is
 * scaled to fit the canvas (nearest neighbour) and centered.
 */
class NucleoGraphicsManager : public GraphicsManager {
public:
	NucleoGraphicsManager();
	~NucleoGraphicsManager() override;

	bool hasFeature(OSystem::Feature f) const override;
	void setFeatureState(OSystem::Feature f, bool enable) override;
	bool getFeatureState(OSystem::Feature f) const override;

#ifdef USE_RGB_COLOR
	Graphics::PixelFormat getScreenFormat() const override { return _screenFormat; }
	Common::List<Graphics::PixelFormat> getSupportedFormats() const override;
#endif
	void initSize(uint width, uint height, const Graphics::PixelFormat *format = nullptr) override;
	int getScreenChangeID() const override { return _screenChangeID; }

	void beginGFXTransaction() override {}
	OSystem::TransactionError endGFXTransaction() override;

	int16 getHeight() const override { return _screen.h; }
	int16 getWidth() const override { return _screen.w; }
	void setPalette(const byte *colors, uint start, uint num) override;
	void grabPalette(byte *colors, uint start, uint num) const override;
	void copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) override;
	Graphics::Surface *lockScreen() override { return &_screen; }
	void unlockScreen() override { _dirty = true; }
	void fillScreen(uint32 col) override;
	void fillScreen(const Common::Rect &r, uint32 col) override;
	void updateScreen() override;
	void setShakePos(int shakeXOffset, int shakeYOffset) override;
	void setFocusRectangle(const Common::Rect &rect) override {}
	void clearFocusRectangle() override {}

	void showOverlay(bool inGUI) override;
	void hideOverlay() override;
	bool isOverlayVisible() const override { return _overlayVisible; }
	Graphics::PixelFormat getOverlayFormat() const override;
	void clearOverlay() override;
	void grabOverlay(Graphics::Surface &surface) const override;
	void copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) override;
	int16 getOverlayHeight() const override { return _canvasH; }
	int16 getOverlayWidth() const override { return _canvasW; }

	bool showMouse(bool visible) override;
	void warpMouse(int x, int y) override;
	void setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY, uint32 keycolor,
	                    bool dontScale = false, const Graphics::PixelFormat *format = nullptr,
	                    const byte *mask = nullptr) override;
	void setCursorPalette(const byte *colors, uint start, uint num) override;

	// Backend side.
	int canvasWidth() const { return _canvasW; }
	int canvasHeight() const { return _canvasH; }
	// Canvas pixel -> the coordinate space events use (overlay or game screen), clamped.
	void canvasToScreen(int cx, int cy, int &sx, int &sy) const;
	// Mouse position in that same space (what warpMouse / the last event set).
	int mouseX() const { return _mouseX; }
	int mouseY() const { return _mouseY; }
	void setMousePos(int x, int y);
	// False once the OS asked the app to close (nv_gfx_present returned 0).
	bool alive() const { return _alive; }
	// Keep the OS wedge watchdog fed while the engine is busy without drawing (~16 ms sleep).
	void idlePresent();

private:
	void composeGame(uint16 *dst) const;
	void drawCursor(uint16 *dst) const;
	void computeGameRect();
	uint16 cursorColor(uint32 v) const;
	void present();

	int _canvasW, _canvasH;
	uint16 *_frame;                      // what goes to the canvas
	uint16 *_overlay;                    // GUI layer, canvas-sized RGB565

	Graphics::Surface _screen;           // game screen
	Graphics::PixelFormat _screenFormat;
	Graphics::PixelFormat _pendingFormat;
	uint _pendingW, _pendingH;
	int _screenChangeID;
	byte _palRGB[256 * 3];
	uint16 _pal565[256];
	int _shakeX, _shakeY;

	// Game rect inside the canvas (scaled to fit, centered) + per-column/row source lookups.
	int _gx, _gy, _gw, _gh;
	int *_mapX, *_mapY;

	bool _overlayVisible;
	bool _dirty;
	bool _alive;

	// Cursor: raw pixels in the cursor's own format, converted while drawing.
	byte *_cursor;
	byte *_cursorMask;
	uint _cursorW, _cursorH;
	int _hotX, _hotY;
	uint32 _keycolor;
	bool _cursorDontScale;
	Graphics::PixelFormat _cursorFormat;
	uint16 _cursorPal565[256];
	bool _cursorPaletteEnabled;
	bool _cursorVisible;
	int _mouseX, _mouseY;
};

#endif
