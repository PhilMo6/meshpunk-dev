local caps = ...

local body = [[
Plays games made with Pyxel, the Python retro game engine. It runs the game's own Python code on the device; games are not included.

GAMES
.pyxapp files, or a folder holding a main.py, in /pyxel on the SD card.
]]

if caps.trackball then
    body = body .. [[

DEFAULT KEYS
The keys drive a virtual gamepad, which every Pyxel game reads.
D-pad - W A S D, or the trackball
A - Space, or trackball click
B - X
X - C,  Y - V
Start - P
Quit - y
]]
else
    body = body .. [[

DEFAULT KEYS]] .. (caps.keyboard and "" or " (USB keyboard)") .. [[

The keys drive a virtual gamepad, which every Pyxel game reads.
D-pad - W A S D
A - Space
B - X
X - C,  Y - V
Start - P
Quit - y
]]
end

body = body .. [[

Keys that are not bound type straight into the game, so Enter and the letter keys a game names on screen still work. Change any binding in Controls.
]]

if caps.trackball then
    body = body .. [[

In games that show a mouse pointer, the trackball moves the pointer and its click is the left mouse button.
]]
end

body = body .. [[

SOUND
The Sound button turns game sound on or off.

SAVES
Games that save high scores or progress keep them in /pyxel/save on the SD card. A game that saves into its own .pyxapp file keeps that data only until it closes.
]]

if not caps.keyboard then
    body = body .. [[

On this device you play on the on-screen pad or with a USB gamepad. The Touch button opens the pad's layout editor. Hold the on-screen QUIT button for about a second to exit.]]
else
    body = body .. [[

Hold Alt and Backspace for about 1.5 seconds to quit back to the launcher.]]
end

return { body = body }
