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

/*
 * NucleoOS backend: ScummVM as a WASI app (WAMR guest) on the NucleoOS ESP32-P4 tablet.
 *
 * Single-threaded: the guest has no threads, so the timers and the audio mixer are pumped from
 * the calls the engines make regularly (pollEvent, delayMillis; getMillis is not used for that:
 * engines call it in tight loops).
 *
 * Files: the standalone app has "home" + "fs": "/" is the user's workspace /sdcard/home (games go in
 * /ScummVM/<game>), "/appdata" the app's private folder (scummvm.ini, saves, engine data in
 * /appdata/extra). A store game package runs this module as its engine with "fs" only: "/" is the
 * package's own folder, where nucleo-installer.cpp puts the game (see nucleoDataDir()).
 *
 * Every input device works at once, each as soon as it is connected:
 * Touch (canvas pixels, finger 0 is the pointer):
 *   tap               left click where the finger was
 *   drag              left button held while the finger moves
 *   long press        right click (examine / action menu in most games)
 *   two-finger tap    right click
 *   three-finger tap  Escape (skip cutscene / close dialog)
 *   OS back gesture   ScummVM main menu (in game) or Escape (in the launcher)
 * Keyboard (USB / Bluetooth, ABI 14): every key, US layout, modifiers and F-keys.
 * Mouse (USB / Bluetooth, ABI 14): relative motion, three buttons, wheel.
 * Game controller (ABI 11): D-pad / left stick move the pointer, A left click, B right click,
 * X Escape, Y period (skip line), Start main menu, Back F5.
 */

#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define FORBIDDEN_SYMBOL_EXCEPTION_FILE
#define FORBIDDEN_SYMBOL_EXCEPTION_stderr
#define FORBIDDEN_SYMBOL_EXCEPTION_fputs
#define FORBIDDEN_SYMBOL_EXCEPTION_exit
#define FORBIDDEN_SYMBOL_EXCEPTION_time_h
#define FORBIDDEN_SYMBOL_EXCEPTION_unistd_h
#define FORBIDDEN_SYMBOL_EXCEPTION_mkdir

#include "common/scummsys.h"

#ifdef NUCLEO_BACKEND

#include "backends/modular-backend.h"
#include "backends/mutex/null/null-mutex.h"
#include "backends/saves/default/default-saves.h"
#include "backends/timer/default/default-timer.h"
#include "backends/events/default/default-events.h"
#include "backends/mixer/mixer.h"
#include "backends/fs/posix/posix-fs-factory.h"
#include "backends/platform/nucleo/nucleo-graphics.h"
#include "backends/platform/nucleo/nucleo-imports.h"
#include "backends/platform/nucleo/nucleo-installer.h"
#include "backends/platform/nucleo/nucleo-keymap.h"
#include "audio/mixer_intern.h"
#include "base/main.h"
#include "common/config-manager.h"
#include "common/fs.h"
#include "common/queue.h"
#include "common/events.h"
#include "engines/engine.h"

// ---- audio ----------------------------------------------------------------------------------------

/*
 * The mixer renders 16-bit stereo at 22050 Hz into the OS stream (nv_audio_*), topping the queue up
 * to kTargetFrames each pump. With no stream (speaker busy / absent) it still mixes in real time
 * into a scratch buffer: engines wait for sounds to finish, a mixer that never ran would stall them.
 */
class NucleoMixerManager : public MixerManager {
public:
	static const uint kRate = 22050;
	static const uint kChunkFrames = 512;
	static const uint kTargetFrames = 3072;   // ~140 ms queued

	NucleoMixerManager() : _open(false), _lastMs(0), _debt(0), _retryMs(0) {}
	~NucleoMixerManager() override {
		if (_open)
			nv_audio_close();
	}

	void init() override {
		_mixer = new Audio::MixerImpl(kRate, true, kChunkFrames);
		_mixer->setReady(true);
		_open = nv_audio_open(kRate, 2) == 1;
		_lastMs = nv_millis();
	}

	void suspendAudio() override { _audioSuspended = true; }
	int resumeAudio() override {
		if (!_audioSuspended)
			return -2;
		_audioSuspended = false;
		_lastMs = nv_millis();
		return 0;
	}

	void update() {
		if (!_mixer || _audioSuspended)
			return;
		const uint32 now = nv_millis();
		if (!_open && now - _retryMs > 2000) {   // the Music app may have released the speaker
			_retryMs = now;
			_open = nv_audio_open(kRate, 2) == 1;
		}
		if (_open) {
			for (int guard = 0; guard < 16; guard++) {
				const int backlog = nv_audio_backlog();
				if (backlog < 0) {   // stream gone: fall back to the silent clock
					_open = false;
					break;
				}
				if ((uint)backlog >= kTargetFrames * 4)
					break;
				_mixer->mixCallback((byte *)_buf, sizeof(_buf));
				if (nv_audio_write(_buf, sizeof(_buf)) < 0) {
					_open = false;
					break;
				}
			}
			_lastMs = now;
			_debt = 0;
			return;
		}
		// Silent: consume exactly as many frames as real time would have played.
		_debt += (now - _lastMs) * kRate;
		_lastMs = now;
		while (_debt >= (uint64)kChunkFrames * 1000) {
			_mixer->mixCallback((byte *)_buf, sizeof(_buf));
			_debt -= (uint64)kChunkFrames * 1000;
		}
	}

private:
	bool _open;
	uint32 _lastMs;
	uint64 _debt;          // frames * 1000 owed to the silent clock
	uint32 _retryMs;
	int16 _buf[kChunkFrames * 2];
};

// ---- system ---------------------------------------------------------------------------------------

class OSystem_Nucleo : public ModularMixerBackend, public ModularGraphicsBackend, Common::EventSource {
public:
	OSystem_Nucleo();
	~OSystem_Nucleo() override;

	void initBackend() override;
	bool pollEvent(Common::Event &event) override;
	Common::MutexInternal *createMutex() override { return new NullMutexInternal(); }
	uint32 getMillis(bool skipRecord = false) override { return (uint32)nv_millis() - _startMs; }
	void delayMillis(uint msecs) override;
	void getTimeAndDate(TimeDate &td, bool skipRecord = false) const override;
	void quit() override;
	void logMessage(LogMessageType::Type type, const char *message) override;
	void addSysArchivesToSearchSet(Common::SearchSet &s, int priority) override;
	Common::Path getDefaultConfigFileName() override { return Common::Path(Common::String(nucleoDataDir()) + "/scummvm.ini"); }

private:
	NucleoGraphicsManager *gfx() { return (NucleoGraphicsManager *)_graphicsManager; }
	void pump();
	void pollTouch();
	void pollPad();
	void pollKeyboard();
	void pollMouse();
	void pushMouse(Common::EventType type);
	void pushKey(Common::KeyCode code, uint16 ascii);
	void pushKeyEvent(Common::EventType type, Common::KeyCode code, uint16 ascii, byte flags);
	void moveTo(int cx, int cy);
	void nudge(int dx16, int dy16);   // move the pointer by 1/16 canvas px

	uint32 _startMs;
	uint32 _lastPump;
	Common::Queue<Common::Event> _events;
	bool _quitSent;

	// Touch gesture state (canvas pixels).
	enum TouchState { kIdle, kPending, kDrag, kLong };
	TouchState _touch;
	int _downX, _downY, _lastX, _lastY, _maxFingers;
	uint32 _downMs;

	// Pad state (first controller, nv_pad_state).
	uint32 _padPrev;
	uint32 _padMs;
	int _padSpeed;
	int _fracX, _fracY;             // pointer sub-pixel remainder, 1/16 canvas px

	// Keyboard (nv_kbd_state) and mouse (nv_mouse_read).
	uint8 _kbdPrev[7];
	bool _capsLock;
	uint32 _mousePrev;
	bool _mouseProbe;
};

OSystem_Nucleo::OSystem_Nucleo() :
	_startMs(nv_millis()), _lastPump(0), _quitSent(false), _touch(kIdle), _downX(0), _downY(0),
	_lastX(0), _lastY(0), _maxFingers(0), _downMs(0), _padPrev(0), _padMs(0), _padSpeed(0), _fracX(0),
	_fracY(0), _capsLock(false), _mousePrev(0), _mouseProbe(true) {
	memset(_kbdPrev, 0, sizeof(_kbdPrev));
	_fsFactory = new POSIXFilesystemFactory();
}

OSystem_Nucleo::~OSystem_Nucleo() {
}

void OSystem_Nucleo::initBackend() {
	const Common::String data = nucleoDataDir();
	mkdir((data + "/saves").c_str(), 0777);
	mkdir((data + "/extra").c_str(), 0777);
	ConfMan.registerDefault("savepath", data + "/saves");
	ConfMan.registerDefault("extrapath", data + "/extra");
	ConfMan.registerDefault("gui_theme", "builtin");
	if (!data.empty()) {   // the standalone app sees the user's workspace: games go in /ScummVM
		mkdir("/ScummVM", 0777);
		ConfMan.registerDefault("browser_lastpath", "/ScummVM");
	}

	_timerManager = new DefaultTimerManager();
	_eventManager = new DefaultEventManager(this);
	_savefileManager = new DefaultSaveFileManager(Common::Path(data + "/saves"));
	_graphicsManager = new NucleoGraphicsManager();
	_mixerManager = new NucleoMixerManager();
	_mixerManager->init();

	BaseBackend::initBackend();
}

// Timers and audio, at most every 5 ms (engines call pollEvent in tight loops).
void OSystem_Nucleo::pump() {
	const uint32 now = nv_millis();
	if (now - _lastPump < 5)
		return;
	_lastPump = now;
	((DefaultTimerManager *)_timerManager)->checkTimers();
	((NucleoMixerManager *)_mixerManager)->update();
}

void OSystem_Nucleo::delayMillis(uint msecs) {
	const uint32 end = nv_millis() + msecs;
	for (;;) {
		pump();
		const int32 left = (int32)(end - (uint32)nv_millis());
		if (left <= 0)
			break;
		if (left >= 16 && gfx())
			gfx()->idlePresent();   // sleeps ~16 ms in the OS, feeds its wedge watchdog
		else
			usleep(MIN<int32>(left, 5) * 1000);
	}
}

void OSystem_Nucleo::getTimeAndDate(TimeDate &td, bool skipRecord) const {
	time_t curTime = time(nullptr);
	struct tm t = *localtime(&curTime);
	td.tm_sec = t.tm_sec;
	td.tm_min = t.tm_min;
	td.tm_hour = t.tm_hour;
	td.tm_mday = t.tm_mday;
	td.tm_mon = t.tm_mon;
	td.tm_year = t.tm_year;
	td.tm_wday = t.tm_wday;
}

void OSystem_Nucleo::quit() {
	exit(0);
}

void OSystem_Nucleo::logMessage(LogMessageType::Type type, const char *message) {
	fputs(message, stderr);
	if (type == LogMessageType::kError || type == LogMessageType::kWarning)
		nv_log(type == LogMessageType::kError ? NV_LOG_ERROR : NV_LOG_WARN, message);
}

void OSystem_Nucleo::addSysArchivesToSearchSet(Common::SearchSet &s, int priority) {
	Common::FSNode extra(Common::Path(Common::String(nucleoDataDir()) + "/extra"));
	if (extra.exists() && extra.isDirectory())
		s.add("nucleo-extra", new Common::FSDirectory(extra, 4), priority);
}

// ---- input ----------------------------------------------------------------------------------------

void OSystem_Nucleo::pushMouse(Common::EventType type) {
	Common::Event e;
	e.type = type;
	e.mouse = Common::Point(gfx()->mouseX(), gfx()->mouseY());
	_events.push(e);
}

void OSystem_Nucleo::pushKey(Common::KeyCode code, uint16 ascii) {
	Common::Event e;
	e.type = Common::EVENT_KEYDOWN;
	e.kbd = Common::KeyState(code, ascii);
	_events.push(e);
	e.type = Common::EVENT_KEYUP;
	_events.push(e);
}

// Pointer to a canvas pixel: updates the mouse and queues a move if it changed.
void OSystem_Nucleo::moveTo(int cx, int cy) {
	int sx, sy;
	gfx()->canvasToScreen(cx, cy, sx, sy);
	if (sx != gfx()->mouseX() || sy != gfx()->mouseY()) {
		gfx()->setMousePos(sx, sy);
		pushMouse(Common::EVENT_MOUSEMOVE);
	}
}

void OSystem_Nucleo::pollTouch() {
	static const int kDragPx = 5;          // canvas px (15 panel px): past this a press is a drag
	static const uint32 kLongMs = 550;
	const int n = nv_gfx_touch_count();
	int x = _lastX, y = _lastY;
	if (n > 0) {
		const int32 v = nv_gfx_touch_point_raw(0);
		if ((v >> 24) & 1) {
			x = v & 0xFFF;
			y = (v >> 12) & 0xFFF;
		}
	}
	const uint32 now = nv_millis();
	switch (_touch) {
	case kIdle:
		if (n > 0) {
			_touch = kPending;
			_downX = _lastX = x;
			_downY = _lastY = y;
			_downMs = now;
			_maxFingers = n;
			moveTo(x, y);
		}
		break;
	case kPending:
		_maxFingers = MAX(_maxFingers, n);
		if (n == 0) {
			_touch = kIdle;
			if (_maxFingers >= 3) {
				pushKey(Common::KEYCODE_ESCAPE, Common::ASCII_ESCAPE);
			} else if (_maxFingers == 2) {
				pushMouse(Common::EVENT_RBUTTONDOWN);
				pushMouse(Common::EVENT_RBUTTONUP);
			} else {
				pushMouse(Common::EVENT_LBUTTONDOWN);
				pushMouse(Common::EVENT_LBUTTONUP);
			}
		} else if (_maxFingers == 1 && (ABS(x - _downX) > kDragPx || ABS(y - _downY) > kDragPx)) {
			_touch = kDrag;
			pushMouse(Common::EVENT_LBUTTONDOWN);   // at the press point, then follow the finger
			moveTo(x, y);
		} else if (_maxFingers == 1 && now - _downMs >= kLongMs) {
			_touch = kLong;
			pushMouse(Common::EVENT_RBUTTONDOWN);
		}
		break;
	case kDrag:
	case kLong:
		if (n == 0) {
			pushMouse(_touch == kDrag ? Common::EVENT_LBUTTONUP : Common::EVENT_RBUTTONUP);
			_touch = kIdle;
		} else {
			moveTo(x, y);
		}
		break;
	}
	_lastX = x;
	_lastY = y;
}

// Pointer by a sub-pixel amount (1/16 canvas px), shared by pad sticks and mouse motion.
void OSystem_Nucleo::nudge(int dx16, int dy16) {
	_fracX += dx16;
	_fracY += dy16;
	const int mx = _fracX / 16, my = _fracY / 16;
	_fracX -= mx * 16;
	_fracY -= my * 16;
	if (!mx && !my)
		return;
	_lastX = CLIP(_lastX + mx, 0, gfx()->canvasWidth() - 1);
	_lastY = CLIP(_lastY + my, 0, gfx()->canvasHeight() - 1);
	moveTo(_lastX, _lastY);
}

// First game controller (ABI 11, Xbox layout whatever the model): D-pad / left stick move the
// pointer, A left click, B right click, X Escape, Y period (skip a line), Start the ScummVM menu,
// Back F5 (the game's own menu). Keyboards are read separately (pollKeyboard), so nv_gfx_pad —
// which folds the keyboard into pad bits — is not used.
void OSystem_Nucleo::pollPad() {
	const uint32 now = nv_millis();
	const int dt = MIN<int>(now - _padMs, 50);
	_padMs = now;
	nv_pad_state_t st;
	uint32 pad = 0;
	int lx = 0, ly = 0;
	if (nv_pad_count() > 0 && nv_pad_state(0, &st, sizeof(st)) > 0) {
		pad = st.buttons;
		static const int kDead = 6000;
		lx = ABS((int)st.lx) < kDead ? 0 : st.lx;
		ly = ABS((int)st.ly) < kDead ? 0 : st.ly;
	}
	const uint32 down = pad & ~_padPrev, up = _padPrev & ~pad;
	_padPrev = pad;
	// Velocity in 1/16 canvas px per second: the D-pad accelerates from 40 to 320 px/s over
	// 0.7 s, the stick is quadratic up to 360 px/s.
	int vx = 0, vy = 0;
	const int dx = ((pad & NV_PADB_RIGHT) ? 1 : 0) - ((pad & NV_PADB_LEFT) ? 1 : 0);
	const int dy = ((pad & NV_PADB_DOWN) ? 1 : 0) - ((pad & NV_PADB_UP) ? 1 : 0);
	if (dx || dy) {
		_padSpeed = MIN(_padSpeed + dt, 700);
		const int v = (40 + _padSpeed * 280 / 700) * 16;
		vx = dx * v;
		vy = dy * v;
	} else {
		_padSpeed = 0;
	}
	vx += (int)((int64)lx * ABS(lx) * 360 * 16 / ((int64)32768 * 32768));
	vy += (int)((int64)ly * ABS(ly) * 360 * 16 / ((int64)32768 * 32768));
	if (vx || vy)
		nudge(vx * dt / 1000, vy * dt / 1000);
	if (down & NV_PADB_A) pushMouse(Common::EVENT_LBUTTONDOWN);
	if (up & NV_PADB_A)   pushMouse(Common::EVENT_LBUTTONUP);
	if (down & NV_PADB_B) pushMouse(Common::EVENT_RBUTTONDOWN);
	if (up & NV_PADB_B)   pushMouse(Common::EVENT_RBUTTONUP);
	if (down & NV_PADB_X) pushKey(Common::KEYCODE_ESCAPE, Common::ASCII_ESCAPE);
	if (down & NV_PADB_Y) pushKey(Common::KEYCODE_PERIOD, '.');
	if (down & NV_PADB_BACK) pushKey(Common::KEYCODE_F5, Common::ASCII_F5);
	if (down & NV_PADB_START) {
		Common::Event e;
		e.type = Common::EVENT_MAINMENU;
		_events.push(e);
	}
}

void OSystem_Nucleo::pushKeyEvent(Common::EventType type, Common::KeyCode code, uint16 ascii, byte flags) {
	Common::Event e;
	e.type = type;
	e.kbd = Common::KeyState(code, ascii, flags);
	_events.push(e);
}

// USB / Bluetooth keyboard (ABI 14): diff the held usages against the last poll. Key repeat comes
// from ScummVM's event manager.
void OSystem_Nucleo::pollKeyboard() {
	uint8 cur[7];
	memset(cur, 0, sizeof(cur));
	const int n = nv_kbd_state(cur, sizeof(cur));
	if (n < 0)
		memset(cur, 0, sizeof(cur));   // unplugged: release whatever was held
	const bool shift = cur[0] & 0x22;
	const byte flags = nucleoHidMods(cur[0]) | (_capsLock ? Common::KBD_CAPS : 0);
	for (int i = 1; i < 7; i++) {   // released
		const uint8 u = _kbdPrev[i];
		if (u < 4 || memchr(cur + 1, u, 6))
			continue;
		Common::KeyCode kc;
		uint16 ascii;
		if (nucleoHidToKey(u, shift, _capsLock, kc, ascii))
			pushKeyEvent(Common::EVENT_KEYUP, kc, ascii, flags);
	}
	for (int i = 1; i < 7; i++) {   // pressed
		const uint8 u = cur[i];
		if (u < 4 || memchr(_kbdPrev + 1, u, 6))
			continue;
		if (u == 0x39)
			_capsLock = !_capsLock;
		Common::KeyCode kc;
		uint16 ascii;
		if (nucleoHidToKey(u, shift, _capsLock, kc, ascii))
			pushKeyEvent(Common::EVENT_KEYDOWN, kc, ascii, flags);
	}
	memcpy(_kbdPrev, cur, sizeof(cur));
}

// USB / Bluetooth mouse (ABI 14): relative motion, buttons, wheel. The first call captures the
// mouse for this app (the OS pointer hides); it is probed every poll since it can be plugged later.
void OSystem_Nucleo::pollMouse() {
	nv_mouse_t m;
	memset(&m, 0, sizeof(m));
	if (nv_mouse_read(&m, sizeof(m)) != 1) {
		m.buttons = 0;
		m.dx = m.dy = m.wheel = 0;
	}
	// Raw counts: ~3 counts per canvas pixel feels like the desktop pointer on the 3x panel.
	if (m.dx || m.dy)
		nudge(m.dx * 16 / 3, m.dy * 16 / 3);
	const uint32 down = m.buttons & ~_mousePrev, up = _mousePrev & ~m.buttons;
	_mousePrev = m.buttons;
	if (down & NV_MOUSE_LEFT)   pushMouse(Common::EVENT_LBUTTONDOWN);
	if (up & NV_MOUSE_LEFT)     pushMouse(Common::EVENT_LBUTTONUP);
	if (down & NV_MOUSE_RIGHT)  pushMouse(Common::EVENT_RBUTTONDOWN);
	if (up & NV_MOUSE_RIGHT)    pushMouse(Common::EVENT_RBUTTONUP);
	if (down & NV_MOUSE_MIDDLE) pushMouse(Common::EVENT_MBUTTONDOWN);
	if (up & NV_MOUSE_MIDDLE)   pushMouse(Common::EVENT_MBUTTONUP);
	for (int w = m.wheel; w > 0; w--) pushMouse(Common::EVENT_WHEELUP);
	for (int w = m.wheel; w < 0; w++) pushMouse(Common::EVENT_WHEELDOWN);
}

bool OSystem_Nucleo::pollEvent(Common::Event &event) {
	pump();
	if (_events.empty()) {
		if (gfx() && !gfx()->alive() && !_quitSent) {
			_quitSent = true;
			event.type = Common::EVENT_QUIT;
			return true;
		}
		if (nv_gfx_back() > 0) {
			if (g_engine) {
				Common::Event e;
				e.type = Common::EVENT_MAINMENU;
				_events.push(e);
			} else {
				pushKey(Common::KEYCODE_ESCAPE, Common::ASCII_ESCAPE);
			}
		}
		pollTouch();
		pollPad();
		pollKeyboard();
		pollMouse();
	}
	if (_events.empty())
		return false;
	event = _events.pop();
	return true;
}

// ---- entry ----------------------------------------------------------------------------------------

// A store game package runs this module with "--nucleo-game=<key>" (manifest "args"): install the
// game data on first start, then start that game directly (ScummVM detects it in <data>/game).
int main(int argc, char *argv[]) {
	g_system = new OSystem_Nucleo();
	assert(g_system);
	const char *key = nullptr;
	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--nucleo-game=", 14))
			key = argv[i] + 14;
	}
	int res;
	if (key) {
		Common::String gameid, lang;
		if (!nucleoInstallGame(key, gameid, lang)) {
			g_system->destroy();
			return 0;
		}
		const Common::String path = Common::String("--path=") + nucleoDataDir() + "/game";
		// --language picks the language inside multi-language data (BASS subtitles, Nippon Safes,
		// Drascula); single-language archives leave it empty (a filter could reject e.g. EN_ANY).
		const Common::String language = "--language=" + lang;
		const char *args[] = { argv[0], path.c_str(), language.c_str(), gameid.c_str() };
		if (lang.empty())
			args[2] = args[3];
		res = scummvm_main(lang.empty() ? 3 : 4, args);
	} else {
		res = scummvm_main(argc, argv);
	}
	g_system->destroy();
	return res;
}

#endif
