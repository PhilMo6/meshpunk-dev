--[[
  lib/wavrec — record the microphone into a WAV file: 16 kHz, mono, 16-bit
  PCM, the capture engine's one format (FW_API 16 _mic_* bindings).

    local wavrec = require("lib/wavrec")
    local s, err = wavrec.start{ path  = "S:/meshpunk/recordings/a.wav",
                                 owner = "Recorder", gain = 4 }
    wavrec.tick(s)               -- while recording, at least once a second:
                                 -- moves what the microphone ring holds
                                 -- (2 s of audio) into the file; false once
                                 -- the recording has ended (s.ended = why)
    local info = wavrec.stop(s)  -- { path, bytes, seconds, ended }

  start() creates the file and opens the microphone session under `owner`
  (playback is muted until the session closes); stop() finishes the file
  and closes the session if it is still ours. The file holds the session
  from its first sample. Every 5 s of audio the header's sizes are
  rewritten and the file is flushed, so a recording cut off by a Lua
  teardown, a reset or a power loss still opens, up to that point.

  A recording ends on its own (tick returns false, s.ended set) when the
  session is closed by someone else (standby closes it), when a write comes
  up short (the storage is full or gone) or at MAX_BYTES of audio (about 17 h; this
  Lua's integers are 32-bit). A caller may end it the same way by setting
  s.ended. The audio written so far stays in the file; stop() is still
  needed to finish the header and close the file.
]]

local M = {}

M.RATE        = 16000
M.BYTES_PER_S = 32000                 -- 16 kHz x 2 bytes

local PATCH_EVERY = 5 * 32000         -- header rewrite + flush per 5 s of audio
local MAX_BYTES   = 2000000000

local function header(data_bytes)
    return string.pack("<c4I4c4c4I4I2I2I4I4I2I2c4I4",
        "RIFF", 36 + data_bytes, "WAVE",
        "fmt ", 16, 1, 1, M.RATE, M.BYTES_PER_S, 2, 16,
        "data", data_bytes)
end

-- Whole samples written so far.
local function data_bytes(s)
    return s.bytes - s.bytes % 2
end

-- Flush the audio, rewrite the RIFF and data sizes, go back to the end, flush
-- again: the header never claims audio that is not on the card yet. (Before
-- FW_API 16, file:seek on a file open for writing clamped to the size at the
-- last flush; flushing first also keeps these seeks right on such firmware.)
local function patch(s)
    local n = data_bytes(s)
    local f = s.f
    f:flush()
    f:seek("set", 4)
    f:write(string.pack("<I4", 36 + n))
    f:seek("set", 40)
    f:write(string.pack("<I4", n))
    f:seek("end")
    f:flush()
    s.patched = s.bytes
end

-- o = { path = file to create, owner = session owner name, gain = 0..7 or nil }
-- -> session table, or nil + reason (nothing is left behind on failure).
function M.start(o)
    if type(_mic_open) ~= "function" then
        return nil, "this firmware has no microphone support"
    end
    local dir = o.path:match("^(.*)/[^/]+$")
    if dir and dir ~= "" and not dir:match(":$") then _fs_mkdir(dir) end
    local f, err = io.open(o.path, "w")
    if not f then return nil, "cannot create the file: " .. tostring(err) end
    if f:write(header(0)) ~= 44 then
        f:close()
        _fs_remove(o.path)
        return nil, "cannot write the file (storage full?)"
    end
    local ok, merr = _mic_open(o.owner)
    if not ok then
        f:close()
        _fs_remove(o.path)
        return nil, "cannot open the microphone: " .. tostring(merr)
    end
    if o.gain then _mic_set_gain(o.gain) end
    return { f = f, path = o.path, owner = o.owner,
             bytes = 0, patched = 0, ended = nil }
end

-- Move the ring into the file. True while recording, false once ended.
function M.tick(s)
    if s.ended or not s.f then return false end
    if _mic_owner() ~= s.owner then
        s.ended = "the microphone was closed"
        return false
    end
    for _ = 1, 16 do                          -- up to 128 KB (4 s) per call
        local chunk = _mic_read(8192)
        if not chunk then break end
        local n = s.f:write(chunk) or 0
        s.bytes = s.bytes + n
        if n ~= #chunk then
            s.ended = "a write failed (the storage is full or was removed)"
            return false
        end
        if s.bytes >= MAX_BYTES then
            s.ended = "the recording reached its size limit (about 17 h)"
            return false
        end
    end
    if s.bytes - s.patched >= PATCH_EVERY then patch(s) end
    return true
end

-- Seconds of audio in the file so far.
function M.seconds(s)
    return data_bytes(s) / M.BYTES_PER_S
end

-- Finish the header, close the file, close the session if it is still ours.
-- Safe to call more than once. -> { path, bytes, seconds, ended }
function M.stop(s)
    if s.f then
        if not s.ended then M.tick(s) end
        patch(s)
        s.f:close()
        s.f = nil
    end
    if _mic_owner() == s.owner then _mic_close() end
    local n = data_bytes(s)
    return { path = s.path, bytes = n, seconds = n / M.BYTES_PER_S, ended = s.ended }
end

-- Seconds of audio in a WAV file of `size` bytes in this format.
function M.size_seconds(size)
    local n = (tonumber(size) or 0) - 44
    if n < 0 then n = 0 end
    return n / M.BYTES_PER_S
end

-- Seconds of audio a WAV file's header announces (what a player plays: a
-- recording cut off between header updates holds more), from its data size
-- and byte rate. nil unless the file starts with a 44-byte RIFF/WAVE header.
function M.header_seconds(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local hdr = f:read(44)
    f:close()
    if not hdr or #hdr < 44 then return nil end
    local riff, _, wave, _, _, _, _, _, brate, _, _, data, dsize =
        string.unpack("<c4I4c4c4I4I2I2I4I4I2I2c4I4", hdr)
    if riff ~= "RIFF" or wave ~= "WAVE" or data ~= "data" or brate == 0 then return nil end
    return dsize / brate
end

return M
