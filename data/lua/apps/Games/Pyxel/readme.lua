local caps = ...

local body = [[
Plays games made with Pyxel, the Python retro game engine. It runs the game's own Python code on the device; games are not included.

GAMES
.pyxapp files, or a folder holding a main.py, in /pyxel on the SD card.

DEFAULT KEYS
The keys drive a virtual gamepad, which every Pyxel game reads.
D-pad - W A S D, or the trackball
A - Space, or trackball click
B - X
X - C,  Y - V
Start - P
Quit - y

Keys that are not bound type straight into the game, so Enter and the letter keys a game names on screen still work. Change any binding in Controls.

In games that show a mouse pointer, the trackball moves the pointer and its click is the left mouse button.

SOUND
The Sound button turns game sound on or off. Music written in MML is not supported yet.
]]

if not caps.keyboard then
    body = body .. [[

On this device you play on the on-screen pad or with a USB gamepad. The Touch button opens the pad's layout editor. Hold the on-screen QUIT button for about a second to exit.]]
else
    body = body .. [[

Hold Alt and Backspace for about 1.5 seconds to quit back to the launcher.]]
end

return { body = body }
