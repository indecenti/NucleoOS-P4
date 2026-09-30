# Reversi

Reversi by OBONO: the classic board game against the computer or a friend.

## Controls

Arrows move the cursor, A places a disc, B opens the menu.

- **Touch**: D-pad on the left, **A** and **B** on the right (A lower, B upper, as on the Arduboy). Several fingers at once work.
- **Gamepad** (USB or Bluetooth): D-pad or left stick; A, X and Start = A; B and Y = B.
- **USB keyboard**: arrows or WASD; Space/X/Enter = A; Z/C/Backspace = B.
- **MENU** (top left), the L/R shoulders, Select or the pad's Guide button pause the game.

## The pause menu

**Resume**, **Zoom** 5x, 6x or 7x (7x is the biggest picture and hides the touch controls: best with a gamepad or keyboard), **Colours** (white, amber, green, blue, LCD), **Sound** on or off, **Exit**. Zoom, colours and sound are remembered. The system back gesture opens the menu; a second one exits.

## Saving

The Arduboy EEPROM (1 KB, where games keep high scores and progress) is saved in the app's folder shortly after every write and when the app closes.

## Credits and licenses

**Reversi** is by OBONO (MIT). Source: https://github.com/obono/ArduboyWorks/tree/master/reversi (commit `d4b1f04178`).

Part of OBONO's ArduboyWorks collection (MIT).

Runs on the Arduboy2 library by Scott Allen and contributors (BSD 3-clause license), adapted to NucleoOS; musical note names come from ArduboyTones (MIT). Arduboy is a trademark of Arduboy Inc.; this port is not affiliated.

### Game license

```
The MIT License (MIT)

Copyright (c) 2016-2026 OBONO

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Arduboy2

```
Licensed under the BSD 3-clause license:

Arduboy2 library:
Copyright (c) 2016-2021, Scott Allen
All rights reserved.

The Arduboy2 library was forked from the Arduboy library:
https://github.com/Arduboy/Arduboy
Copyright (c) 2016, Kevin "Arduboy" Bates
Copyright (c) 2016, Chris Martinez
Copyright (c) 2016, Josh Goebel
Copyright (c) 2016, Scott Allen
All rights reserved.

- which is in turn partially based on the Adafruit_SSD1306 library
https://github.com/adafruit/Adafruit_SSD1306
Copyright (c) 2012, Adafruit Industries
All rights reserved.

SetSystemEEPROM example sketch:
Copyright (c) 2018-2020, Scott Allen
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holders nor the
names of its contributors may be used to endorse or promote products
derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ''AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
Licensed under the BSD 2-clause license:

Portions of the Arduboy library, and thus portions of the Arduboy2 library,
based on the Adafruit-GFX library:
https://github.com/adafruit/Adafruit-GFX-Library
Copyright (c) 2012 Adafruit Industries
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
Licensed under the MIT license:

Code from ArduboyExtra:
https://github.com/yyyc514/ArduboyExtra
Copyright (c) 2015 Josh Goebel

Code for drawing compressed bitmaps:
Copyright (c) 2016 TEAM a.r.g.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
