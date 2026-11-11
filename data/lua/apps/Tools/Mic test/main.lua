-- Tools > Mic test — the microphone capture seam on this device: a live
-- level meter, the ADC gain, and a recording to a WAV file on the SD card
-- that plays back through the normal sound path. Sound is muted while the
-- mic is open (the capture engine suspends the player). On a keyboard with
-- a mic/Record key, holding the key records (push-to-talk).
local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")
local sound = require("lib/sound")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6, pad_row = 4, bg_opa = 0,
}
nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

content:Label { text = "Mic test", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function() apps.go_home() end)

local dev  = _device_caps()
local caps = _input_caps()
if not dev.mic or type(_mic_open) ~= "function" then
    content:Label { text = "No microphone on this device.", w = lvgl.PCT(100), h = 20 }
    return root
end

local REC_DIR = "S:/meshpunk/recordings"
local RATE    = 16000

local status = content:Label { text = "Mic closed. Open it to see the level.", w = lvgl.PCT(100), h = 32 }

-- Level meter: a frame with a fill whose width follows the peak.
local meter_w = lvgl.HOR_RES() - 16
local meter = content:Object { w = lvgl.PCT(100), h = 14, bg_opa = 0, border_width = 1,
                               border_color = "#888888", pad_all = 0 }
local fill  = meter:Object { w = 0, h = 12, bg_color = "#44cc44", border_width = 0, pad_all = 0 }
local level_lbl = content:Label { text = "Level: -", w = lvgl.PCT(100), h = 16 }

local open_btn = content:Button { w = lvgl.PCT(48), h = 28 }
local open_lbl = open_btn:Label { text = "Open mic", align = lvgl.ALIGN.CENTER }
local rec_btn  = content:Button { w = lvgl.PCT(48), h = 28 }
local rec_lbl  = rec_btn:Label { text = "Record", align = lvgl.ALIGN.CENTER }

local gain = 4                                  -- 6 dB steps, 0..7
local gain_dn = content:Button { w = lvgl.PCT(30), h = 26 }
gain_dn:Label { text = "Gain -", align = lvgl.ALIGN.CENTER }
local gain_lbl = content:Label { text = "", w = lvgl.PCT(36), h = 26 }
local gain_up = content:Button { w = lvgl.PCT(30), h = 26 }
gain_up:Label { text = "Gain +", align = lvgl.ALIGN.CENTER }

local play_btn = content:Button { w = lvgl.PCT(100), h = 28 }
local play_lbl = play_btn:Label { text = "Play last recording", align = lvgl.ALIGN.CENTER }
local file_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 32 }
if caps.mic_key then
    content:Label { text = "Hold the mic/Record key to record while the mic is open.",
                    w = lvgl.PCT(100), h = 16 }
end

local mic_open  = false
local rec       = nil        -- { f = file, bytes = n, path = s } while recording
local last_path = nil
local peak_hold = 0
local playing   = nil        -- sound object while playing back

local function set_gain_label()
    gain_lbl.text = "Gain: " .. tostring(gain * 6) .. " dB"
end
set_gain_label()

-- 44-byte PCM WAV header: 16 kHz, mono, 16-bit.
local function wav_header(data_bytes)
    return string.pack("<c4I4c4c4I4I2I2I4I4I2I2c4I4",
        "RIFF", 36 + data_bytes, "WAVE",
        "fmt ", 16, 1, 1, RATE, RATE * 2, 2, 16,
        "data", data_bytes)
end

local function next_rec_path()
    _fs_mkdir(REC_DIR)
    for n = 1, 999 do
        local p = string.format("%s/rec_%03d.wav", REC_DIR, n)
        if not _fs_exists(p) then return p end
    end
    return nil
end

-- Move what the ring holds into the file (up to 64 KB per call).
local function drain_to_file()
    if not rec then return end
    for _ = 1, 8 do
        local chunk = _mic_read(8192)
        if not chunk then break end
        rec.f:write(chunk)
        rec.bytes = rec.bytes + #chunk
    end
end

local function start_rec()
    if rec or not mic_open then return end
    local path = next_rec_path()
    if not path then status.text = "No free recording name (rec_001..999)."; return end
    local f, err = io.open(path, "w")        -- this io takes r / w / a; bytes pass through as they are
    if not f then status.text = "Cannot create " .. path .. ": " .. tostring(err); return end
    f:write(wav_header(0))
    while _mic_read(16384) do end          -- the ring's contents from before the press
    rec = { f = f, bytes = 0, path = path }
    rec_lbl.text = "Stop"
    status.text = "Recording to " .. path
end

local function stop_rec()
    if not rec then return end
    drain_to_file()
    local f = rec.f
    f:seek("set", 0)
    f:write(wav_header(rec.bytes))
    f:close()
    last_path = rec.path
    file_lbl.text = string.format("%s: %.1f s, %d KB", rec.path, rec.bytes / (RATE * 2), rec.bytes // 1024)
    status.text = "Saved. Close the mic to play it back."
    rec = nil
    rec_lbl.text = "Record"
end

local function open_mic()
    if mic_open then return end
    local ok, err = _mic_open("Mic test")
    if not ok then status.text = "Cannot open the mic: " .. tostring(err); return end
    mic_open = true
    peak_hold = 0
    open_lbl.text = "Close mic"
    status.text = "Mic open. Sound is muted while it is open."
    _mic_set_gain(gain)
end

local function close_mic()
    if not mic_open then return end
    stop_rec()
    _mic_close()
    mic_open = false
    open_lbl.text = "Open mic"
    fill:set { w = 0 }
    status.text = "Mic closed."
end

local function stop_playback()
    if playing then
        playing:stop()
        playing:delete()
        playing = nil
    end
    play_lbl.text = "Play last recording"
end

open_btn:onClicked(function()
    if mic_open then close_mic() else open_mic() end
end)

rec_btn:onClicked(function()
    if not mic_open then status.text = "Open the mic first."; return end
    if rec then stop_rec() else start_rec() end
end)

gain_dn:onClicked(function()
    if gain > 0 then gain = gain - 1 end
    if mic_open then _mic_set_gain(gain) end
    set_gain_label()
end)

gain_up:onClicked(function()
    if gain < 7 then gain = gain + 1 end
    if mic_open then _mic_set_gain(gain) end
    set_gain_label()
end)

play_btn:onClicked(function()
    if playing then stop_playback(); return end
    if mic_open then status.text = "Close the mic first: sound is muted while it is open."; return end
    if not last_path then status.text = "Nothing recorded yet."; return end
    local f, err = io.open(last_path, "r")
    if not f then status.text = "Cannot open " .. last_path .. ": " .. tostring(err); return end
    local obj, lerr = sound.loadFile(f)        -- the sound layer owns the file from here
    if not obj then status.text = "Cannot load " .. last_path .. ": " .. tostring(lerr); return end
    playing = obj
    obj:play()
    play_lbl.text = "Stop playback"
    status.text = "Playing " .. last_path
end)

-- 100 ms: level meter, push-to-talk edges, the recording drain, playback end.
local ptt_prev = false
apps.add_timer { period = 100, cb = function()
    if mic_open then
        local peak, dropped = _mic_level()
        if peak > peak_hold then peak_hold = peak end
        fill:set { w = math.floor(meter_w * peak / 32767) }
        level_lbl.text = string.format("Level: %5d  peak %5d  dropped %d", peak, peak_hold, dropped)
        local ptt = _mic_ptt()
        if ptt and not ptt_prev then start_rec() end
        if not ptt and ptt_prev and rec then stop_rec() end
        ptt_prev = ptt
        drain_to_file()
    end
    if playing and sound.fileEnded() then stop_playback() end
end }

-- Any close path (Home button, home chord): end the session first. The C side
-- also closes it on Lua teardown and standby.
apps.set_on_close(function()
    stop_playback()
    close_mic()
    return true
end)

return root
