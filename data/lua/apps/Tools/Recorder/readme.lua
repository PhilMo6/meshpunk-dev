local caps, dev = ...

if not dev.mic then
    return { body = [[
Records from the microphone. This device has none, so the app only says so.]] }
end

return { body = [[
Records from the microphone into WAV files (16 kHz, mono) that play back on the device and open on a PC.

Record starts and Stop ends a recording. The time, a live level bar (red when the sound is too loud) and the gain in 6 dB steps sit above it; the gain is remembered. Sound is muted only while recording: notification melodies and music wait until the recording stops.

Recordings go to meshpunk/recordings on the SD card. Change picks another folder, on the SD card, a USB drive or the internal storage; the line under it shows how much recording time is left there. A recording stops on its own, saved, when the storage is nearly full.

Files are named by date and time, like 2026-11-12_104105.wav, once the device clock has been set (GPS, the mesh or WiFi); before that they are rec_001.wav and up.

Recordings lists the files in the folder, newest first. Selecting one opens the player: play and pause, 10 s back and forward, Rename and Delete.

Leaving the app keeps a recording going: the home screen shows a row with its time, and that row's X stops and saves it. Opening the Recorder again shows it running. Starting a game module ends a recording; the file keeps everything up to the last 5 seconds before that.
]] }
