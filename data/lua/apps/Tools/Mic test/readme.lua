local caps, dev = ...

if not dev.mic then
    return { body = [[
Tests the microphone. This device has none, so the app only says so.]] }
end

local body = [[
Tests the microphone: a live level meter with a peak hold, the input gain in 6 dB steps, and a recording to meshpunk/recordings on the SD card as a 16 kHz mono WAV file. Play last recording plays it back on the device once the mic is closed; the file also opens on a PC.

Sound is muted while the microphone is open: notification melodies and music wait until it closes.
]]

if caps.mic_key then
    body = body .. [[

Holding the keyboard's mic or Record key records while the mic is open (push-to-talk). The key's usual role, opening the notifications, pauses for that time.
]]
end

return { body = body }
