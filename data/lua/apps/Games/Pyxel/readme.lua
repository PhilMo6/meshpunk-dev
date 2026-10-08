local caps, dev = ...

local body = [[
Plays games made with Pyxel, the Python retro game engine, using a C port of the Pyxel engine; the game's own Python code runs on the device under MicroPython. It is not the official Pyxel. Seven example games from Pyxel come with it.

GAMES
The app comes with seven example games from Pyxel: 30 Seconds of Daylight, Cursed Caverns, Laser Jetman, Mega Wing, Megaball, Space Rescue and Vortexion. Add more as .pyxapp files, or folders holding a main.py, in /pyxel on the SD card, or in a game folder you pick in Settings. A game in /pyxel replaces one with the same file name in the Settings folder, and a game in either replaces the example game with that name. A game made for a screen larger than this one is scaled down to fit.
]]

if caps.trackball then
    body = body .. [[

DEFAULT KEYS
The keys drive a virtual gamepad.
D-pad - W A S D, or the trackball
A - M, or trackball click
B - N
X - K,  Y - J
Start - Enter,  Back - Backspace
Quit - y
]]
else
    body = body .. [[

DEFAULT KEYS]] .. (caps.keyboard and "" or " (USB keyboard)") .. [[

The keys drive a virtual gamepad.
D-pad - W A S D
A - M
B - N
X - K,  Y - J
Start - Enter,  Back - Backspace
Quit - y
]]
end

body = body .. [[

Keys that are not bound type straight into the game, so the letter keys a game names on screen still work. Change any binding in Controls.

MATCH GAME
Many Pyxel games read the keyboard rather than a gamepad. With Match game on (the default), the launcher reads a game's code as it starts and points each control at a key that game uses: the D-pad at the arrow keys (or W A S D), A at Space or Enter, and so on. On-screen pad buttons are relabelled with the keys they send. Keys the game reads itself are left free, so they type straight into it. Turn Match game off in Settings to always send gamepad buttons.
]]

if caps.trackball then
    body = body .. [[

In games that show a mouse pointer, the trackball moves the pointer and its click is the left mouse button.
]]
end

body = body .. [[

SETTINGS
Game folder picks one more folder to look in for games, on the SD card or in internal storage; games can't run from a USB drive. Clear removes the folder. Sound turns game sound on or off, and Match game switches the key matching described above on or off. The launcher remembers these settings.

SAVES
Games that save high scores or progress keep them in /pyxel/save on the SD card. A game that saves into its own .pyxapp file keeps that data only until it closes.
]]

if caps.keyboard then
    body = body .. [[

TYPING
When a game asks you to type, such as a name for the high-score table, press Alt+Enter to switch the keys to typing, type, and press Enter. Press Alt+Enter again to get the game controls back.
]]
else
    -- The touch-mode trigger, worded as in the Touch controls help page.
    local trigger
    if dev.name == "heltec_v4" then
        trigger = "press the IO button on the side of the device"
    elseif dev.name == "wio_l2" then
        trigger = "press the WAKE button"
    else
        trigger = "press the device's input-mode button"
    end
    body = body .. [[

TYPING
When a game asks you to type, such as a name for the high-score table, ]] .. trigger .. [[ until the on-screen keyboard appears, type, and press its Enter key. Keep pressing the same button to get the game pad back.
]]
end

if not caps.keyboard then
    body = body .. [[

On this device you play on the on-screen pad or with a USB gamepad. The Touch button opens the pad's layout editor. Hold the on-screen QUIT button for about a second to exit.]]
else
    body = body .. [[

Hold Alt and Backspace for about 1.5 seconds to quit back to the launcher.]]
end

return { body = body }
