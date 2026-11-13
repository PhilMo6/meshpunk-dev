--[[
  Tools > Recorder — a voice recorder: records the microphone to WAV files,
  lists them, plays them back, renames and deletes them.

  Recording goes through lib/wavrec. Its timer lives in the background
  contract's state table, not in the app's timer list (the Screenshot app's
  pattern), so leaving the app by any path, its Home button or the home
  chord, keeps recording; the launcher then shows a row with the elapsed
  time whose X stops and saves. An ELF launch ends a recording (the whole
  Lua state goes), with the file intact up to wavrec's last 5 s update. The
  microphone is open only while recording, so notifications and music are
  held only then.

  Files: <folder>/YYYY-MM-DD_HHMMSS.wav from the device clock once a time
  source has set it, rec_001.wav and up before that (the file system keeps
  no dates). The folder defaults to S:/meshpunk/recordings and is chosen
  with lib/filepick; it and the gain step live in L:/recorder.cfg.
]]

local lvgl     = require("lvgl")
local apps     = require("lib/apps")
local nav      = require("lib/nav")
local theme    = require("lib/theme")
local sound    = require("lib/sound")
local utils    = require("lib/utils")
local filepick = require("lib/filepick")
local wavrec   = require("lib/wavrec")

local W, H        = lvgl.HOR_RES(), lvgl.VER_RES()
local KEY         = "recorder"              -- background contract key
local OWNER       = "Recorder"              -- microphone session owner
local CFG         = "L:/recorder.cfg"
local DEFAULT_DIR = "S:/meshpunk/recordings"
local MIN_FREE    = 2 * 1024 * 1024         -- no start, and a stop, within 2 MB of full
local MAX_SHOW    = 150                     -- rows built per list (as Tools/Files)
local CLOCK_SET   = 1600000000              -- _rtc_time() above this: a time source set it

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)
theme.show_background()

local dev = _device_caps()
if not dev.mic or type(_mic_open) ~= "function" then
    local box = root:Object {
        w = W, h = H, bg_opa = 0, border_width = 0, pad_all = 8,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    nav.replace(box, { flags = nav.ROLLOVER })
    box:Label { text = "This device has no microphone.", w = lvgl.PCT(100) }
    local b = box:Button { w = lvgl.PCT(50), h = 28 }
    b:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    b:onClicked(function() apps.go_home() end)
    return root
end

-- ── Settings ──────────────────────────────────────────────────────────────────

local cfg = { gain = 4, dir = DEFAULT_DIR }

-- Folder paths are kept without a trailing slash ("S:" for a drive root).
local function norm_dir(p)
    p = tostring(p or ""):gsub("/+$", "")
    if p == "" then return DEFAULT_DIR end
    return p
end

local function load_cfg()
    local f = io.open(CFG, "r")
    if not f then return end
    local txt = f:read("*a") or ""
    f:close()
    local g = tonumber(txt:match("gain=(%d+)"))
    if g and g >= 0 and g <= 7 then cfg.gain = g end
    local d = txt:match("dir=([^\r\n]+)")
    if d then cfg.dir = norm_dir(d) end
end

local function save_cfg()
    local f = io.open(CFG, "w")
    if not f then return end
    f:write(string.format("gain=%d\ndir=%s\n", cfg.gain, cfg.dir))
    f:close()
end

load_cfg()

-- ── Formatting ────────────────────────────────────────────────────────────────

local function mmss(sec)
    sec = math.floor(tonumber(sec) or 0)
    if sec < 0 then sec = 0 end
    if sec >= 3600 then
        return string.format("%d:%02d:%02d", sec // 3600, (sec % 3600) // 60, sec % 60)
    end
    return string.format("%d:%02d", sec // 60, sec % 60)
end

local function size_text(bytes)
    bytes = tonumber(bytes) or 0
    if bytes >= 1048576 then return string.format("%.1f MB", bytes / 1048576) end
    return string.format("%d KB", math.floor(bytes / 1024 + 0.5))
end

local function elide(s, n)
    s = tostring(s or "")
    if #s > n then return s:sub(1, n - 1) .. "~" end
    return s
end

local function base_name(path)
    return (path:match("([^/]+)$") or path)
end

local function strip_wav(name)
    return (name:gsub("%.[wW][aA][vV]$", ""))
end

local function toast(msg)
    pcall(function()
        utils.createNotification(lvgl.disp.get_scr_act(), msg, 2500)
    end)
end

-- ── Storage ───────────────────────────────────────────────────────────────────

-- Free bytes on the drive holding `path` ("S:/x" -> drive "S"), nil when the
-- drive is not there.
local function free_bytes(path)
    local drive = path:match("^(%a):") or "L"
    local total, used = _fs_df(drive)
    if not total then return nil end
    return total - used
end

-- "about 3 h 10 min" of recording in `bytes`.
local function time_left_text(bytes)
    local sec = math.floor(bytes / wavrec.BYTES_PER_S)
    local h, m = sec // 3600, (sec % 3600) // 60
    if h > 0 then return string.format("about %d h %d min", h, m) end
    return string.format("about %d min", m)
end

local function no_drive_text(dir)
    if dir:match("^[Ss]:") then return "No SD card in the device." end
    if dir:match("^[Uu]:") then return "No USB drive attached." end
    return "That storage is not available."
end

-- A free file name in `dir`: the date and time when the clock is set,
-- rec_NNN before that.
local function next_path(dir)
    local ok, ts = pcall(_rtc_time)
    if ok and type(ts) == "number" and ts > CLOCK_SET then
        local y, mo, d, hh, mi, ss = utils.localTime(ts)
        local base = string.format("%s/%04d-%02d-%02d_%02d%02d%02d", dir, y, mo, d, hh, mi, ss)
        if not _fs_exists(base .. ".wav") then return base .. ".wav" end
        for n = 2, 99 do
            local p = string.format("%s_%d.wav", base, n)
            if not _fs_exists(p) then return p end
        end
        return nil
    end
    for n = 1, 999 do
        local p = string.format("%s/rec_%03d.wav", dir, n)
        if not _fs_exists(p) then return p end
    end
    return nil
end

-- ── Recording (UI-free: also runs with the app closed) ─────────────────────

-- State that outlives the UI: the background contract carries this table and
-- a relaunched app rebinds to it.
local bg = apps.background_of(KEY)
local st = (bg and bg.state) or {
    session = nil,      -- lib/wavrec session while recording
    timer   = nil,      -- drives the session; held here so it outlives the UI
    free0   = 0,        -- free bytes on the drive when the recording started
    last    = nil,      -- lib/wavrec info of the last finished recording
}

local function finish(reason)
    local s = st.session
    if not s then return end
    if st.timer then st.timer:delete(); st.timer = nil end
    if reason and not s.ended then s.ended = reason end
    st.last    = wavrec.stop(s)
    st.session = nil
end

-- The space left is the free space at the start minus what has been written:
-- the timer never asks the drive (an SD card's first free-space query can
-- take seconds, longer than the 2 s microphone ring).
local function rec_tick()
    local s = st.session
    if not s then return end
    local alive = wavrec.tick(s)
    if alive and st.free0 - s.bytes < MIN_FREE then
        s.ended, alive = "the storage is nearly full", false
    end
    if not alive then finish() end
end

-- The free-space query runs here, before the microphone opens.
local function start_recording()
    if st.session then return true end
    local free = free_bytes(cfg.dir)
    if not free then return nil, no_drive_text(cfg.dir) end
    if free < MIN_FREE then return nil, "Less than 2 MB is left there." end
    local path = next_path(cfg.dir)                -- wavrec.start creates the folder
    if not path then return nil, "No free file name in that folder." end
    local s, err = wavrec.start { path = path, owner = OWNER, gain = cfg.gain }
    if not s then return nil, err end
    st.session = s
    st.free0   = free
    st.last    = nil
    st.timer   = lvgl.Timer { period = 250, cb = function() rec_tick() end }
    return true
end

-- Registered on every launch (fresh closures; stops the manager's tick while
-- the app is foreground). The recording runs on st.timer either way; the
-- manager tick only keeps the launcher row's status current after the Home
-- button (go_background), and the row's X is on_close.
apps.register_background {
    key      = KEY,
    app_name = "Recorder",
    state    = st,
    period   = 1000,
    tick     = function() end,
    sounds   = function() return {} end,
    on_close = function() finish() end,
    status   = function()
        if st.session then return "Rec " .. mmss(wavrec.seconds(st.session)) end
        return "Recorder"
    end,
}

-- ── Playback ──────────────────────────────────────────────────────────────────

local player = { obj = nil, path = nil, paused = false }

local function stop_playback()
    if player.obj then
        pcall(function() player.obj:stop() end)
        pcall(function() player.obj:delete() end)
    end
    player.obj, player.path, player.paused = nil, nil, false
end

local function start_playback(path)
    stop_playback()
    local f, err = io.open(path, "r")
    if not f then return nil, tostring(err) end
    local obj, lerr = sound.loadFile(f)            -- the sound layer owns the file from here
    if not obj then return nil, tostring(lerr) end
    player.obj, player.path, player.paused = obj, path, false
    obj:play()
    return true
end

-- ── Views ─────────────────────────────────────────────────────────────────────

local vw = nil        -- the current view (one child of root at a time)
local ui = {}         -- the current view's widgets that the UI timer updates

local function swap_view(builder)
    ui = {}
    local old = vw
    vw = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_opa = 0, border_width = 0, pad_all = 0, radius = 0,
    }
    vw:clear_flag(lvgl.FLAG.SCROLLABLE)
    builder(vw)
    if old then apps.delete_view(old) end
end

local function content_of(v)
    local c = v:Object {
        w = W, h = H, x = 0, y = 0, bg_opa = 0, border_width = 0,
        pad_all = 6, pad_row = 4,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    nav.replace(c, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })
    return c
end

local function button(parent, text, w, h, fn)
    local b = parent:Button { w = w, h = h }
    local l = b:Label { text = text, align = lvgl.ALIGN.CENTER }
    b:onClicked(fn)
    return b, l
end

local function row_button(parent, text, sub, fn)
    local b = parent:Button { w = lvgl.PCT(100), h = 26 }
    b:Label { text = elide(text, 26), align = lvgl.ALIGN.LEFT_MID }
    if sub then b:Label { text = elide(sub, 18), align = lvgl.ALIGN.RIGHT_MID } end
    b:onClicked(fn)
    return b
end

-- A frame with a fill whose width the timers set: { frame, fill, w }.
local function bar(parent, h, color)
    local frame = parent:Object {
        w = lvgl.PCT(100), h = h, bg_opa = 0, border_width = 1,
        border_color = "#888888", pad_all = 0,
    }
    frame:clear_flag(lvgl.FLAG.SCROLLABLE)
    local fill = frame:Object {
        w = 0, h = h - 2, bg_color = color, border_width = 0, pad_all = 0, radius = 0,
    }
    return { frame = frame, fill = fill, w = W - 16 }
end

local function modal(build)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0, radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)
    local box = overlay:Object {
        w = W - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)
    local closed = false
    local function close()
        if closed then return end
        closed = true
        nav.pop()
        overlay:delete()
    end
    build(box, close)
    return close
end

-- cb(text) -> error text to show, or nil (+ a function to run after closing).
local function prompt(title, initial, cb)
    modal(function(box, close)
        box:Label { text = title, w = lvgl.PCT(100) }
        local ta = box:Textarea { one_line = true, text = initial or "", w = lvgl.PCT(100), h = 32 }
        ta:clear_flag(lvgl.FLAG.SCROLLABLE)
        local err_lbl = box:Label { text = "", w = lvgl.PCT(100) }
        local function submit()
            local err, after = cb(ta.text or "")
            if err then
                err_lbl.text = tostring(err)
            else
                close()
                if after then after() end
            end
        end
        local ok_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        ok_btn:Label { text = "OK", align = lvgl.ALIGN.CENTER }
        ok_btn:onevent(lvgl.EVENT.RELEASED, submit)
        local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
        cancel_btn:onevent(lvgl.EVENT.RELEASED, close)
        ta:onevent(lvgl.EVENT.KEY, function()
            if lvgl.indev.get_act():get_key() == lvgl.KEY.ENTER then submit() end
        end)
    end)
end

local function confirm(title, warn, on_yes)
    modal(function(box, close)
        box:Label { text = title, w = lvgl.PCT(100) }
        if warn then box:Label { text = warn, text_color = "#ff5555", w = lvgl.PCT(100) } end
        local yes = box:Button { w = lvgl.PCT(100), h = 26 }
        yes:Label { text = "Yes", align = lvgl.ALIGN.CENTER }
        yes:onevent(lvgl.EVENT.RELEASED, function() close(); on_yes() end)
        local no = box:Button { w = lvgl.PCT(100), h = 26 }
        no:Label { text = "No", align = lvgl.ALIGN.CENTER }
        no:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- Exits. The Home button: while recording, into the background (the
-- manager's tick keeps the launcher row current); otherwise a plain close
-- that also drops the contract, so no launcher row is left behind.
local function leave()
    stop_playback()
    if st.session then
        apps.go_background(KEY)
    else
        apps.close_background(KEY)
        apps.go_home()
    end
end

-- Every other close path (the home chord): the same, minus the status relay.
apps.set_on_close(function()
    stop_playback()
    if not st.session then apps.close_background(KEY) end
    return true
end)

local show_record, show_list, show_player

-- The line under the big time on the Record view.
local function record_status()
    if st.session then
        return "Recording to " .. base_name(st.session.path)
    end
    local last = st.last
    if last then
        local s = "Saved " .. base_name(last.path) .. ", " .. mmss(last.seconds) .. "."
        if last.ended then s = s .. " It stopped: " .. last.ended .. "." end
        return s
    end
    return "Ready."
end

-- The Record view's free-space reading: taken when the view is built, after a
-- recording ends and after the folder changes; while recording, the space
-- left is counted down from the start (see rec_tick).
local space = { free = nil, fresh = false }

local function space_text()
    if st.session then
        local left = st.free0 - st.session.bytes
        if left < 0 then left = 0 end
        return time_left_text(left) .. " of recording left there"
    end
    if not space.fresh then
        space.free, space.fresh = free_bytes(cfg.dir), true
    end
    if not space.free then return no_drive_text(cfg.dir) end
    return time_left_text(space.free) .. " of recording left there"
end

local function refresh_record()
    if not ui.rec_lbl then return end
    local rec = st.session ~= nil
    ui.rec_lbl.text = rec and "Stop" or "Record"
    ui.status.text  = record_status()
    ui.time.text    = rec and mmss(wavrec.seconds(st.session)) or "0:00"
    ui.gain.text    = "Gain " .. (cfg.gain * 6) .. " dB"
    ui.where.text   = "Save to " .. elide(cfg.dir, 30)
    ui.space.text   = space_text()
    if not rec then ui.level.fill:set { w = 0 } end
    ui.was_recording = rec
end

local function set_gain(g)
    if g < 0 then g = 0 end
    if g > 7 then g = 7 end
    cfg.gain = g
    save_cfg()
    if st.session then _mic_set_gain(g) end
    refresh_record()
end

local function toggle_record()
    if st.session then
        finish()
        space.fresh = false
        refresh_record()
        return
    end
    stop_playback()
    local ok, err = start_recording()
    if not ok then
        ui.status.text = "Cannot record: " .. tostring(err)
        return
    end
    refresh_record()
end

local function choose_folder()
    filepick.open(root, {
        mode    = "dir",
        title   = "Save recordings to",
        start   = cfg.dir,
        on_pick = function(path)
            cfg.dir = norm_dir(path)
            save_cfg()
            show_record()
            if st.session then
                ui.status.text = "New recordings will go to " .. cfg.dir .. "."
            end
        end,
    })
end

show_record = function()
    space.fresh = false
    swap_view(function(v)
        local c = content_of(v)
        c:Label { text = "Recorder", w = lvgl.PCT(70), h = 22 }
        button(c, "Home", lvgl.PCT(28), 22, leave)
        local tbox = c:Object { w = lvgl.PCT(100), h = 36, bg_opa = 0, border_width = 0, pad_all = 0 }
        tbox:clear_flag(lvgl.FLAG.SCROLLABLE)
        ui.time = tbox:Label {
            text = "0:00", align = lvgl.ALIGN.CENTER,
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_28,
        }
        ui.level = bar(c, 14, "#44cc44")
        ui.status = c:Label { text = "", w = lvgl.PCT(100) }
        ui.rec_btn, ui.rec_lbl = button(c, "Record", lvgl.PCT(100), 40, toggle_record)
        button(c, "Gain -", lvgl.PCT(30), 26, function() set_gain(cfg.gain - 1) end)
        local gbox = c:Object { w = lvgl.PCT(36), h = 26, bg_opa = 0, border_width = 0, pad_all = 0 }
        gbox:clear_flag(lvgl.FLAG.SCROLLABLE)
        ui.gain = gbox:Label { text = "", align = lvgl.ALIGN.CENTER }
        button(c, "Gain +", lvgl.PCT(30), 26, function() set_gain(cfg.gain + 1) end)
        ui.where = c:Label { text = "", w = lvgl.PCT(70) }
        button(c, "Change", lvgl.PCT(28), 26, choose_folder)
        ui.space = c:Label { text = "", w = lvgl.PCT(100) }
        button(c, "Recordings", lvgl.PCT(100), 30, function() show_list() end)
        refresh_record()
        nav.set_focused(ui.rec_btn)
    end)
end

show_list = function()
    swap_view(function(v)
        local c = content_of(v)
        c:Label { text = "Recordings", w = lvgl.PCT(70), h = 22 }
        button(c, "Back", lvgl.PCT(28), 22, function() show_record() end)
        c:Label { text = elide(cfg.dir, 40), w = lvgl.PCT(100) }
        local list = _fs_list(cfg.dir)
        local files = {}
        for _, e in ipairs(list or {}) do
            if e.type == "file" and e.name:lower():match("%.wav$") then files[#files + 1] = e end
        end
        table.sort(files, function(a, b) return a.name > b.name end)   -- date names: newest first
        if #files == 0 then
            -- A folder that does not exist yet lists as nil: it has no recordings.
            local msg = "No recordings in this folder yet."
            if not list and not free_bytes(cfg.dir) then msg = no_drive_text(cfg.dir) end
            c:Label { text = msg, w = lvgl.PCT(100) }
        end
        for i, e in ipairs(files) do
            if i > MAX_SHOW then
                c:Label { text = "(+" .. (#files - MAX_SHOW) .. " more)", w = lvgl.PCT(100) }
                break
            end
            local path = cfg.dir .. "/" .. e.name
            local live = st.session and st.session.path == path
            local sub = live and "recording"
                        or (mmss(wavrec.size_seconds(e.size)) .. "  " .. size_text(e.size))
            row_button(c, strip_wav(e.name), sub, function() show_player(path) end)
        end
    end)
end

local function play_label()
    if player.obj and not player.paused then return "Pause" end
    return "Play"
end

local function toggle_play(path)
    if st.session then
        ui.info.text = "Sound is muted while recording: stop the recording to play."
        return
    end
    if player.obj and player.path == path then
        player.obj:pause()                         -- toggles
        player.paused = not player.paused
    else
        local ok, err = start_playback(path)
        if not ok then ui.info.text = "Cannot play it: " .. err; return end
    end
    ui.play_lbl.text = play_label()
end

local function seek_by(delta)
    if not player.obj then return end
    local pos = sound.getPosition() or 0
    local dur = ui.secs or 0
    local t = pos + delta
    if t < 0 then t = 0 end
    if dur > 0 and t > dur then t = dur end
    sound.seek(t)
end

local function rename(path)
    if st.session and st.session.path == path then
        toast("Still recording into it.")
        return
    end
    local dir = path:match("^(.*)/[^/]+$")
    prompt("New name", strip_wav(base_name(path)), function(txt)
        txt = txt:gsub("^%s+", ""):gsub("%s+$", "")
        txt = strip_wav(txt)
        if txt == "" then return "Type a name." end
        if txt:find("[/\\:%*%?\"<>|]") then return "Leave out / \\ : * ? \" < > |" end
        local new = dir .. "/" .. txt .. ".wav"
        if new == path then return nil end
        if _fs_exists(new) then return "That name is taken." end
        if player.path == path then stop_playback() end
        local ok, err = _fs_rename(path, new)
        if not ok then return "Cannot rename it: " .. tostring(err) end
        return nil, function() show_player(new) end
    end)
end

local function delete(path)
    if st.session and st.session.path == path then
        toast("Stop the recording first.")
        return
    end
    confirm("Delete " .. strip_wav(base_name(path)) .. "?", "This cannot be undone.", function()
        if player.path == path then stop_playback() end
        local ok, err = _fs_remove(path)
        if not ok then toast("Cannot delete it: " .. tostring(err)); return end
        show_list()
    end)
end

show_player = function(path)
    swap_view(function(v)
        local c = content_of(v)
        c:Label { text = elide(strip_wav(base_name(path)), 24), w = lvgl.PCT(70), h = 22 }
        button(c, "Back", lvgl.PCT(28), 22, function()
            stop_playback()
            show_list()
        end)
        -- The header's length is what plays, known before playback starts and
        -- to the sample (the decoder's getDuration is whole seconds).
        local stat = _fs_stat(path)
        local secs = wavrec.header_seconds(path) or (stat and wavrec.size_seconds(stat.size)) or 0
        ui.info = c:Label {
            text = stat and (mmss(secs) .. ", " .. size_text(stat.size)) or "",
            w = lvgl.PCT(100),
        }
        if st.session and st.session.path == path then
            ui.info.text = "Still recording into this file."
        end
        ui.prog  = bar(c, 12, "#4488ff")
        ui.pos   = c:Label { text = "0:00 / " .. mmss(secs), w = lvgl.PCT(100) }
        ui.secs  = secs
        ui.path  = path
        button(c, "<< 10 s", lvgl.PCT(31), 30, function() seek_by(-10) end)
        local pb
        pb, ui.play_lbl = button(c, play_label(), lvgl.PCT(34), 30, function() toggle_play(path) end)
        button(c, "10 s >>", lvgl.PCT(31), 30, function() seek_by(10) end)
        button(c, "Rename", lvgl.PCT(48), 28, function() rename(path) end)
        button(c, "Delete", lvgl.PCT(48), 28, function() delete(path) end)
        nav.set_focused(pb)
    end)
end

-- ── UI timer: level, elapsed time, playback progress ──────────────────────────

apps.add_timer { period = 200, cb = function()
    if ui.rec_lbl then
        if st.session then
            ui.time.text  = mmss(wavrec.seconds(st.session))
            ui.space.text = space_text()
            local peak = _mic_level()
            ui.level.fill:set {
                w = math.floor(ui.level.w * peak / 32767),
                bg_color = (peak >= 32000) and "#ff4444" or "#44cc44",
            }
        elseif ui.was_recording then
            space.fresh = false                    -- it ended (storage, standby, the X)
            refresh_record()
        end
    end
    if ui.prog and player.obj and player.path == ui.path then
        if sound.fileEnded() then
            stop_playback()
            ui.play_lbl.text = play_label()
            ui.prog.fill:set { w = 0 }
            ui.pos.text = "0:00 / " .. mmss(ui.secs)
        else
            local pos = sound.getPosition() or 0
            local dur = ui.secs
            if dur > 0 then
                ui.prog.fill:set { w = math.floor(ui.prog.w * math.min(pos / dur, 1)) }
            end
            ui.pos.text = mmss(pos) .. " / " .. mmss(dur)
        end
    end
end }

show_record()

return root
