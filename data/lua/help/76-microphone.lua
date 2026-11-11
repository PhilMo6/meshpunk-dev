local caps, dev = ...

-- Only devices with a microphone get this page.
if not dev.mic then return nil end

local body = [[
The device has a microphone. Tools > Mic test shows its live level, sets the gain and records a short WAV file to meshpunk/recordings on the SD card, which plays back on the device or opens on a PC.

While the microphone is open, sound is muted: notification melodies and music wait until it closes.
]]

if caps.mic_key then
    body = body .. [[

Holding the keyboard's mic or Record key records while the Mic test has the microphone open (push-to-talk). The key's usual role, opening the notifications, pauses for that time.
]]
end

return {
    title   = "Microphone",
    section = "Guide",
    order   = 76,
    body    = body,
}
