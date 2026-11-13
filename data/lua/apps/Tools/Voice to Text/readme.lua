local caps, dev = ...

if not dev.mic then
    return { body = [[
Turns speech into text. This device has no microphone, so the app only says so.]] }
end

local how = caps.mic_key
    and "Hold the mic key (or the Talk button), speak, then let go."
    or "Hold the Talk button, speak, then let go."

return { body = how .. [[ The text goes into the box a few seconds later: about four times as long as you spoke (a 3 second sentence takes about 12 seconds). English only.

With the Append button (beside Model), each new recording goes after the text in the box; tap it for Overwrite, which puts each new recording in the text's place. Copy puts the box's text on the clipboard; paste it in Messenger or any text field. Clear empties the box. Sound is muted only while you hold the ]] ..
    (caps.mic_key and "key" or "button") .. [[.

Long speech is cut into parts of up to 4 seconds at the quietest moments, so pause a little between sentences. Numbers are written as words ("twenty five"); some come out as "⁇", so check them.

The speech engine needs its model file (13.8 MB, model.bin), which the App Library puts in the app's folder: install the app on the SD card, the model does not fit in the internal storage. Model picks another model file.

Bluetooth (the phone link) is off while the engine is loaded: the engine needs the memory it uses. Quit, or the home screen row's X, turns it back on. Starting a game module while the engine runs leaves Bluetooth off until the next restart.

Home keeps the engine loaded in the background.]] ..
    (caps.mic_key and [[ The mic key then records from any app: hold it, speak, let go, and the text goes into the text box you had selected, where its cursor is, or onto the clipboard when no text box is selected. A popup shows where it went.]] or "") ..
    [[ While it runs, Messenger's message box has Insert voice text in its hold menu: speak, tap End, and the text goes into the message where the cursor is. The home screen row's X, or Quit, unloads the engine.

The speech model is conformer-stt-s3's, a quantized and distilled version of NVIDIA's stt_en_conformer_ctc_small, licensed CC BY 4.0 (creativecommons.org/licenses/by/4.0). The engine code is conformer-stt-s3 (Apache 2.0, lspr98) with Espressif's esp-dsp FFT (Apache 2.0). Neither project is endorsed by the original model's authors.
]] }
