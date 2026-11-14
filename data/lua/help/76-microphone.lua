local caps, dev = ...

-- Only devices with a microphone get this page.
if not dev.mic then return nil end

local body = [[
The device has a microphone. Tools > Recorder records it into WAV files (16 kHz, mono) in meshpunk/recordings on the SD card, or another folder you pick, and plays them back; the files also open on a PC.

While a recording runs, sound is muted: notification melodies and music wait until it stops. A recording keeps going when you leave the app; the home screen shows it with an X that stops and saves it.
]]

if dev.audioplug then
    body = body .. [[

Voice to Text, from the App Library (Tools), turns speech into text: hold ]] ..
        (caps.mic_key and "the mic key" or "its Talk button") ..
        [[, speak, let go, and the text appears in its box a few seconds later. While it runs in the background,]] ..
        (caps.mic_key and [[ the mic key types speech into the text box selected in any app (or onto the clipboard when none is), and]] or "") ..
        [[ Messenger's message box has Insert voice text in its hold menu. The App Library installs it with its 13.8 MB speech model, on the SD card; the app's help page has the details.
]]
end

return {
    title   = "Microphone",
    section = "Guide",
    order   = 76,
    body    = body,
}
