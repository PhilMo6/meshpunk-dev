--[[
  lib/dictation — speech to text for every app, through the speech-to-text
  audio plugin (stt.audioplug.elf, FW_API 16) and its model file. One
  engine at a time (one audio plugin at a time): M.active is the open one.
  Tools > Voice to Text opens it and keeps it open while it runs, in the
  background too; lib/voice_popup records with it from other apps.

    local dictation = require("lib/dictation")
    local d, err = dictation.open { plugin = path, model = path, owner = "Voice to Text" }
    dictation.tick(d)             -- every ~100 ms: results, the mic key, the state
    dictation.record(d, sink)     -- a recording starts -> ok, err
    dictation.stop(d)             -- the speech has ended: the rest is written down
    dictation.cancel(d)           -- drop the recording
    dictation.close(d)            -- unload the engine

  A recording's text goes to its sink only:
    sink.piece(text)      optional: the text so far, as each clip's text comes
    sink.done(text, err)  once, at the end: text = what was heard ("" when
                          nothing was), or text nil and err ("cancelled", or
                          why the recording ended without its text)

  d.state    "loading" | "ready" | "listening" | "working" | "failed" | "closed"
  d.err      the engine's last error message, or nil
  d.status   the engine's status line while it works ("listening 2.3 s")
  d.mic_key  the keyboard has a mic / Record key: holding it is push-to-talk
             (read by tick), from any app while tick runs
  d.mic_sink function(d) -> sink or nil, set by the engine's owner: tick calls
             it when the mic key goes down and records into the sink it
             returns until the key comes up (nil: the key does nothing)

  UI-free: the caller's timer, or a background contract's tick, calls tick.
  The microphone session is open only from a recording's start until the
  engine says the capture ended (plugin result "C"), so sounds play the rest
  of the time. open turns BLE off first, without writing the setting
  (_ble_set_enabled(false, false)), and close turns it back on: on the Wio
  L2 the engine and the microphone do not fit in internal RAM beside BLE and
  WiFi. A firmware teardown with the engine loaded (a game module launch)
  leaves BLE off until the next boot, as the Snes launcher's borrow does.
  open then arms push-to-talk (_mic_ptt_arm): that makes the capture task
  now and keeps it for every recording, and on keyboards makes the mic key
  push-to-talk; the engine loads last. Standby unloads the engine (firmware):
  tick then turns the state to "failed" and ends a recording in progress.
]]

local M = { active = nil }

-- The recording in progress ends: its sink gets the text or the reason.
local function finish(d, text, err)
    local rec = d.rec
    if not rec then return end
    d.rec = nil
    d.key_rec = false
    rec.sink.done(text, err)
end

-- Every result the plugin has published, in order.
local function drain(d)
    while true do
        local r = _audio_plug_read()
        if not r then return end
        local kind = r:sub(1, 2)
        if r == "R" then
            if d.state == "loading" then d.state = "ready" end
        elseif kind == "T:" then
            local rec = d.rec
            if rec and not rec.cancelled then
                local piece = r:sub(3)
                if rec.text == "" then piece = piece:gsub("^%s+", "") end
                if piece ~= "" then
                    rec.text = rec.text .. piece
                    if rec.sink.piece then rec.sink.piece(rec.text) end
                end
            end
        elseif r == "C" then
            if d.mic_open then _mic_close(); d.mic_open = false end
        elseif r == "D" then
            if d.mic_open then _mic_close(); d.mic_open = false end
            if d.state == "listening" or d.state == "working" then d.state = "ready" end
            local rec = d.rec
            if rec then
                if rec.cancelled then
                    finish(d, nil, "cancelled")
                elseif rec.text == "" and rec.err then
                    finish(d, nil, rec.err)
                else
                    finish(d, rec.text)
                end
            end
        elseif kind == "E:" then
            d.err = r:sub(3)
            if d.state == "loading" then d.state = "failed" end
            if d.rec then d.rec.err = d.err end
        elseif kind == "S:" then
            d.stats = r:sub(3)
        end
    end
end

function M.open(opts)
    if M.active then return nil, "the speech engine is already open" end
    if type(_audio_plug_load) ~= "function" then
        return nil, "this firmware has no audio plugins (it needs FW_API 16)"
    end
    local dev = _device_caps()
    if not dev.mic then return nil, "this device has no microphone" end
    if not _fs_stat(opts.model) then return nil, "no model file at " .. opts.model end
    local ble_borrowed = _ble_get_enabled()
    if ble_borrowed then _ble_set_enabled(false, false) end
    local function give_back()
        if ble_borrowed then _ble_set_enabled(true, false) end
    end
    local aok, aerr = _mic_ptt_arm(true)
    if not aok then
        give_back()
        return nil, "push-to-talk: " .. tostring(aerr)
    end
    local ok, err = _audio_plug_load(opts.plugin)
    if not ok then
        _mic_ptt_arm(false)
        give_back()
        return nil, tostring(err)
    end
    local ic = _input_caps()
    local d = {
        state = "loading", err = nil, status = "",
        owner = opts.owner or "speech to text",
        mic_key = ic.mic_key == true,
        ble_borrowed = ble_borrowed,   -- BLE was on: close turns it back on
        mic_open = false, key_down = false, key_rec = false,
        rec = nil, mic_sink = nil,
    }
    _audio_plug_write("model " .. opts.model)
    M.active = d
    return d
end

-- A recording starts: the microphone opens, the engine takes its audio.
function M.record(d, sink)
    if d.state ~= "ready" then
        if d.state == "listening" or d.state == "working" then
            return false, "a recording is in progress"
        end
        if d.state == "loading" then return false, "the speech engine is still loading" end
        return false, "the speech engine is not running (" .. tostring(d.err) .. ")"
    end
    local ok, err = _mic_open(d.owner)
    if not ok then return false, "the microphone is " .. tostring(err) end
    d.mic_open = true
    d.err = nil
    d.rec = { sink = sink, text = "" }
    _audio_plug_write("start")
    d.state = "listening"
    return true
end

-- The speech has ended: the engine writes down what it heard.
function M.stop(d)
    if d.state ~= "listening" then return end
    _audio_plug_write("stop")
    d.state = "working"
end

-- The recording is dropped: its sink gets err "cancelled" once the engine
-- has ended the pass running.
function M.cancel(d)
    if d.state ~= "listening" and d.state ~= "working" then return end
    if d.rec then d.rec.cancelled = true end
    _audio_plug_write("cancel")
    d.state = "working"
end

function M.tick(d)
    if d.state == "closed" or d.state == "failed" then return end
    if not _audio_plug_loaded() then
        -- Standby (or a Lua-side unload) took the engine away.
        if d.mic_open then _mic_close(); d.mic_open = false end
        d.state = "failed"
        d.err = "the speech engine was unloaded (standby); open Voice to Text again"
        finish(d, nil, d.err)
        return
    end
    drain(d)
    if d.mic_key then
        local held = _mic_ptt()
        if held and not d.key_down then
            d.key_down = true
            local sink = d.state == "ready" and d.mic_sink and d.mic_sink(d)
            if sink then
                local ok, err = M.record(d, sink)
                if ok then d.key_rec = true else sink.done(nil, err) end
            end
        elseif not held and d.key_down then
            d.key_down = false
            if d.key_rec then
                d.key_rec = false
                M.stop(d)
            end
        end
    end
    if d.state == "listening" or d.state == "working" then
        d.status = tostring(_audio_plug_status() or "")
    else
        d.status = ""
    end
end

function M.close(d)
    if d.state == "closed" then return end
    if d.mic_open then _mic_close(); d.mic_open = false end
    _audio_plug_unload()
    _mic_ptt_arm(false)
    if d.ble_borrowed then _ble_set_enabled(true, false) end
    d.state = "closed"
    if M.active == d then M.active = nil end
    finish(d, nil, "the speech engine was closed")
end

return M
