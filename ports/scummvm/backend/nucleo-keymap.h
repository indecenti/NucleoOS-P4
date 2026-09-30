/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#ifndef BACKENDS_PLATFORM_NUCLEO_KEYMAP_H
#define BACKENDS_PLATFORM_NUCLEO_KEYMAP_H

#include "common/keyboard.h"

// USB HID keyboard usage (boot protocol, US layout) -> ScummVM key code + the character it types.
// False for usages ScummVM has no key for.
static bool nucleoHidToKey(uint8 usage, bool shift, bool caps, Common::KeyCode &kc, uint16 &ascii) {
	static const char kDigits[] = "1234567890";
	static const char kDigitsShift[] = "!@#$%^&*()";
	ascii = 0;
	if (usage >= 0x04 && usage <= 0x1D) {                      // a..z
		const char c = (char)('a' + usage - 0x04);
		kc = (Common::KeyCode)(Common::KEYCODE_a + (usage - 0x04));
		ascii = (shift != caps) ? (uint16)(c - 'a' + 'A') : (uint16)c;
		return true;
	}
	if (usage >= 0x1E && usage <= 0x27) {                      // 1..9, 0
		const int i = usage - 0x1E;
		kc = i == 9 ? Common::KEYCODE_0 : (Common::KeyCode)(Common::KEYCODE_1 + i);
		ascii = (uint16)(shift ? kDigitsShift[i] : kDigits[i]);
		return true;
	}
	if (usage >= 0x3A && usage <= 0x45) {                      // F1..F12
		kc = (Common::KeyCode)(Common::KEYCODE_F1 + (usage - 0x3A));
		ascii = (uint16)(Common::ASCII_F1 + (usage - 0x3A));
		return true;
	}
	if (usage >= 0x59 && usage <= 0x61) {                      // keypad 1..9
		kc = (Common::KeyCode)(Common::KEYCODE_KP1 + (usage - 0x59));
		ascii = (uint16)('1' + (usage - 0x59));
		return true;
	}
	struct Entry { uint8 usage; Common::KeyCode kc; char plain, shifted; };
	static const Entry kTable[] = {
		{ 0x28, Common::KEYCODE_RETURN, 13, 13 },     { 0x29, Common::KEYCODE_ESCAPE, 27, 27 },
		{ 0x2A, Common::KEYCODE_BACKSPACE, 8, 8 },    { 0x2B, Common::KEYCODE_TAB, 9, 9 },
		{ 0x2C, Common::KEYCODE_SPACE, ' ', ' ' },    { 0x2D, Common::KEYCODE_MINUS, '-', '_' },
		{ 0x2E, Common::KEYCODE_EQUALS, '=', '+' },   { 0x2F, Common::KEYCODE_LEFTBRACKET, '[', '{' },
		{ 0x30, Common::KEYCODE_RIGHTBRACKET, ']', '}' }, { 0x31, Common::KEYCODE_BACKSLASH, '\\', '|' },
		{ 0x33, Common::KEYCODE_SEMICOLON, ';', ':' }, { 0x34, Common::KEYCODE_QUOTE, '\'', '"' },
		{ 0x35, Common::KEYCODE_BACKQUOTE, '`', '~' }, { 0x36, Common::KEYCODE_COMMA, ',', '<' },
		{ 0x37, Common::KEYCODE_PERIOD, '.', '>' },   { 0x38, Common::KEYCODE_SLASH, '/', '?' },
		{ 0x39, Common::KEYCODE_CAPSLOCK, 0, 0 },     { 0x46, Common::KEYCODE_PRINT, 0, 0 },
		{ 0x47, Common::KEYCODE_SCROLLOCK, 0, 0 },    { 0x48, Common::KEYCODE_PAUSE, 0, 0 },
		{ 0x49, Common::KEYCODE_INSERT, 0, 0 },       { 0x4A, Common::KEYCODE_HOME, 0, 0 },
		{ 0x4B, Common::KEYCODE_PAGEUP, 0, 0 },       { 0x4C, Common::KEYCODE_DELETE, 127, 127 },
		{ 0x4D, Common::KEYCODE_END, 0, 0 },          { 0x4E, Common::KEYCODE_PAGEDOWN, 0, 0 },
		{ 0x4F, Common::KEYCODE_RIGHT, 0, 0 },        { 0x50, Common::KEYCODE_LEFT, 0, 0 },
		{ 0x51, Common::KEYCODE_DOWN, 0, 0 },         { 0x52, Common::KEYCODE_UP, 0, 0 },
		{ 0x53, Common::KEYCODE_NUMLOCK, 0, 0 },      { 0x54, Common::KEYCODE_KP_DIVIDE, '/', '/' },
		{ 0x55, Common::KEYCODE_KP_MULTIPLY, '*', '*' }, { 0x56, Common::KEYCODE_KP_MINUS, '-', '-' },
		{ 0x57, Common::KEYCODE_KP_PLUS, '+', '+' },  { 0x58, Common::KEYCODE_KP_ENTER, 13, 13 },
		{ 0x62, Common::KEYCODE_KP0, '0', '0' },      { 0x63, Common::KEYCODE_KP_PERIOD, '.', '.' },
		{ 0x65, Common::KEYCODE_MENU, 0, 0 },
	};
	for (uint i = 0; i < ARRAYSIZE(kTable); i++) {
		if (kTable[i].usage == usage) {
			kc = kTable[i].kc;
			ascii = (uint16)(uint8)(shift ? kTable[i].shifted : kTable[i].plain);
			return true;
		}
	}
	return false;
}

// HID modifier byte -> ScummVM KBD_* flags.
static byte nucleoHidMods(uint8 m) {
	byte f = 0;
	if (m & 0x11) f |= Common::KBD_CTRL;
	if (m & 0x22) f |= Common::KBD_SHIFT;
	if (m & 0x44) f |= Common::KBD_ALT;
	if (m & 0x88) f |= Common::KBD_META;
	return f;
}

#endif
