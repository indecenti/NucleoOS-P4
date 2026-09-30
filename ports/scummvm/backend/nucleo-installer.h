/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#ifndef BACKENDS_PLATFORM_NUCLEO_INSTALLER_H
#define BACKENDS_PLATFORM_NUCLEO_INSTALLER_H

#include "common/str.h"

/*
 * Game packages (NucleoOS store, "engine": "scummvm", args "--nucleo-game=<key>"): the package
 * carries no game data. On first start the engine downloads the original archives of the variant
 * the user picked in the store (<data>/variant) from downloads.scummvm.org, checks their SHA-256,
 * unpacks them into <data>/game and remembers what it installed in <data>/installed.sha. Picking another
 * language in the store rewrites <data>/variant: the next start installs that one instead.
 *
 * Runs before scummvm_main with its own minimal UI (it owns the canvas until it returns).
 * Returns true with the ScummVM game id and --language code to start, false when the user left.
 */
// The app's private folder prefix: "/appdata" or "" (see nucleo-installer.cpp).
const char *nucleoDataDir();

bool nucleoInstallGame(const char *key, Common::String &gameid, Common::String &lang);

#endif
