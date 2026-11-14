--[[
  Tools > Sentry — watches the device while it is left alone: noise near it,
  a change of its GPS position, power, and the screen or keys being touched.
  Each trigger has its own settings: whether it is watched, whether it
  sounds the alarm, whether it messages, and its message text. Messages go
  by DM to one contact, chosen from the active protocol's contact list
  (MeshCore or MT-Lite, through the _lora_proto_* surface); settings live in
  L:/sentry.cfg.

  Arm starts the arming delay, then the watch. The app watches while its
  screen is shown (the backlight may time out); leaving the app disarms it.
  A trigger fires at most once per "wait between messages". The alarm is a
  siren at full volume; it closes the microphone first (an open microphone
  session mutes playback) and the volume and mute come back when it stops.

  Touch: while armed, a touch anywhere or a key or trackball press fires the
  trigger at once, or with a grace period set, starts it: the disarm code
  entered within it disarms, else the trigger fires. Key and trackball
  presses are read with _input_capture_*,
  which keeps them from the UI while armed. The home chord is read before
  the capture (input_ui.cpp) and still closes the app; when that happens
  while armed with the touch trigger on, the touch message is sent as the
  app closes. Without a disarm code, Disarm is a plain button.

  Noise: the microphone's peak level (_mic_level, 0-32767) every 100 ms, gain
  step 4; it fires on two polls at or over the threshold within five, and
  ignores the first second after the microphone opens. Movement: a GPS sync cycle after arming
  takes the start position, then one cycle per check interval; it fires past
  the distance (fixes with HDOP over 5 are skipped).

  Power has no reading on this firmware: _get_battery_mv is the only power
  binding, and no board reports whether it is plugged in. The trigger is
  shown as not available.

  MeshCore delivers a DM to the first contact whose name STARTS with the
  given name (searchContactsByPrefix, in contact list order); the picker
  warns when another contact comes first for the chosen name. The contact
  list comes from _lora_proto_peers, which returns at most 128 contacts.

  Every touch detection, trigger and send result is printed to the console
  as a [SENTRY] line.
]]

local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")
local sound = require("lib/sound")
local utils = require("lib/utils")

local W, H      = lvgl.HOR_RES(), lvgl.VER_RES()
local NAME      = "Sentry"                -- title, mic session owner
local CFG       = "L:/sentry.cfg"
local TICK_MS   = 100                     -- the app timer; every count below is in these ticks
local TPS       = 1000 // TICK_MS         -- ticks per second
local MAX_PEERS = 128                     -- _lora_proto_peers' upper bound
local MSG_MAX   = 150                     -- bytes per message, text plus details
local TEXT_MAX  = 120                     -- characters a message text may have
local CLOCK_SET = 1600000000              -- _rtc_time() above this: a time source set it
local SEND_TRIES     = 5
local SEND_RETRY_S   = 30

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)
theme.show_background()

local dev = _device_caps()

-- ── Triggers ──────────────────────────────────────────────────────────────────

local TRIGGERS = {
    { key = "noise", title = "Noise",    watch = "Listen for noise",
      text = "Sentry: noise near the device." },
    { key = "move",  title = "Movement", watch = "Watch the GPS position",
      text = "Sentry: the device has moved." },
    { key = "power", title = "Power",    watch = "Watch for unplugging",
      text = "Sentry: the device was unplugged." },
    { key = "touch", title = "Touch",    watch = "Watch the screen and keys",
      text = "Sentry: someone touched the device." },
}
local TRIG = {}
for _, t in ipairs(TRIGGERS) do TRIG[t.key] = t end

-- Why a trigger cannot run on this device, or nil when it can.
local UNAVAILABLE = {
    noise = (not dev.mic or type(_mic_open) ~= "function") and "This device has no microphone." or nil,
    move  = type(_gps_sync_start) ~= "function" and "This firmware has no GPS bindings." or nil,
    power = "This firmware cannot tell whether the device is plugged in.",
    touch = nil,
}

local LEVEL_MIN, LEVEL_MAX = 50, 32767             -- noise threshold bounds (peak level)
local MOVE_M       = { 25, 50, 100, 250, 500, 1000 }
local MOVE_EVERY   = { 1, 2, 5, 10, 30 }           -- minutes
local TOUCH_GRACE  = { 0, 5, 10, 15, 30, 60 }      -- seconds; 0 = fires on contact
local EXIT_DELAY   = { 0, 5, 10, 30, 60 }          -- seconds
local ALARM_SECS   = { 15, 30, 60, 120, 300, 0 }   -- 0 = until disarmed
local COOLDOWN_MIN = { 1, 5, 15, 60 }

-- ── Settings ──────────────────────────────────────────────────────────────────

local cfg = {
    target_name = "", target_id = "",
    code = "",
    exit_delay = 10, alarm_secs = 60, cooldown_min = 5, details = true,
    noise_on = false, noise_alarm = true, noise_msg = true, noise_text = "", noise_level = 5000,
    move_on  = false, move_alarm  = true, move_msg  = true, move_text  = "", move_m = 100, move_every = 2,
    power_on = false, power_alarm = true, power_msg = true, power_text = "",
    touch_on = false, touch_alarm = true, touch_msg = true, touch_text = "", touch_grace = 0,
}

local function load_cfg()
    local f = io.open(CFG, "r")
    if not f then return end
    local txt = f:read("*a") or ""
    f:close()
    for line in (txt .. "\n"):gmatch("([^\n]*)\n") do
        local k, v = line:match("^([%w_]+)=(.*)$")
        if k and cfg[k] ~= nil then
            v = v:gsub("\r$", "")
            local t = type(cfg[k])
            if t == "boolean" then
                cfg[k] = (v == "1")
            elseif t == "number" then
                local n = tonumber(v)
                if n then cfg[k] = n end
            else
                cfg[k] = v
            end
        end
    end
    if cfg.noise_level < LEVEL_MIN or cfg.noise_level > LEVEL_MAX then cfg.noise_level = 5000 end
end

local function save_cfg()
    local keys = {}
    for k in pairs(cfg) do keys[#keys + 1] = k end
    table.sort(keys)
    local out = {}
    for _, k in ipairs(keys) do
        local v = cfg[k]
        if type(v) == "boolean" then v = v and "1" or "0" end
        local s = tostring(v):gsub("[\r\n]", " ")
        out[#out + 1] = k .. "=" .. s
    end
    local f = io.open(CFG, "w")
    if not f then return end
    f:write(table.concat(out, "\n") .. "\n")
    f:close()
end

load_cfg()

-- ── Text helpers ──────────────────────────────────────────────────────────────

-- At most `max` bytes, never ending inside a UTF-8 sequence.
local function cut(s, max)
    if #s <= max then return s end
    s = s:sub(1, max)
    local i = #s
    while i > 0 do
        local b = s:byte(i)
        if b < 0x80 then break end
        if b >= 0xC0 then
            local need = (b >= 0xF0) and 4 or ((b >= 0xE0) and 3 or 2)
            if #s - i + 1 < need then s = s:sub(1, i - 1) end
            break
        end
        i = i - 1
    end
    return s
end

local function elide(s, n)
    s = tostring(s or "")
    if #s <= n then return s end
    return cut(s, n - 2) .. ".."
end

local function onoff(b) return b and "On" or "Off" end

local function secs_text(s)
    if s >= 60 and s % 60 == 0 then return (s // 60) .. " min" end
    return s .. " s"
end

-- The entry after `cur` in `list`, wrapping; the first entry when `cur` is not in it.
local function cycle(list, cur)
    for i, v in ipairs(list) do
        if v == cur then return list[i % #list + 1] end
    end
    return list[1]
end

local function msg_text(key)
    local t = cfg[key .. "_text"]
    if not t or t == "" then t = TRIG[key].text end
    return t
end

-- ── State ─────────────────────────────────────────────────────────────────────

local st = {
    phase   = "idle",     -- "idle" | "arming" | "armed"
    ticks   = 0,          -- app timer ticks since launch (TICK_MS each)
    arm_at  = 0,          -- tick the watch starts at (arming)
    fired   = {},         -- trigger key -> tick it last fired at
    alarm   = nil,        -- { snd, kind, until_tick | nil, vol, muted } while it sounds
    entry   = nil,        -- { deadline } while the touch grace period runs
    outbox  = {},         -- { text, tries, next_tick } waiting to be sent, in order
    events  = {},         -- recent event lines, newest last
    capture = false,      -- _input_capture armed
    touched = false,      -- the armed view saw a press
    prompt  = nil,        -- { close, title, err } while the disarm prompt is open
    test    = false,      -- the noise level test runs (Noise settings view)
    mic     = { open = false, ignore_until = 0, hist = {}, peak = 0, err = nil, retry_at = 0 },
    gps     = nil,        -- { phase, ref, next_tick, seen_running, waited, dist, note }
}

local function note(text)
    local now = utils.now()
    local stamp = (now > CLOCK_SET) and (utils.clockHM(now) .. " ") or ""
    st.events[#st.events + 1] = stamp .. text
    if #st.events > 20 then table.remove(st.events, 1) end
end

-- ── Hardware seams ────────────────────────────────────────────────────────────

-- A refused open is tried again after 5 s.
local function mic_open()
    if st.mic.open then return true end
    if st.ticks < st.mic.retry_at then return false end
    local ok, err = _mic_open(NAME)
    if not ok then
        st.mic.err = tostring(err)
        st.mic.retry_at = st.ticks + 5 * TPS
        return false
    end
    st.mic.err = nil
    st.mic.open = true
    _mic_set_gain(4)
    _mic_level()                                   -- drop the peak held from before
    st.mic.ignore_until = st.ticks + TPS
    st.mic.hist = {}
    return true
end

local function mic_close()
    if st.mic.open then
        _mic_close()
        st.mic.open = false
    end
end

local function capture_on()
    if not st.capture then
        _input_capture_start()
        st.capture = true
    end
end

local function capture_off()
    if st.capture then
        _input_capture_stop()
        st.capture = false
    end
end

-- _screen_timeout_set resets the activity clock and restores the backlight;
-- the same value is stored again.
local function wake_screen()
    pcall(function() _screen_timeout_set(_screen_timeout_get()) end)
end

-- ── Alarm ─────────────────────────────────────────────────────────────────────

local SIREN = { { freq = 960, ms = 400 }, { freq = 770, ms = 400 } }

local function alarm_start(kind)
    if st.alarm then
        st.alarm.kind = kind
        if st.alarm.until_tick then st.alarm.until_tick = st.ticks + cfg.alarm_secs * TPS end
        return
    end
    mic_close()
    local a = { kind = kind, vol = sound.getVolume(), muted = sound.isMuted() }
    if cfg.alarm_secs > 0 then a.until_tick = st.ticks + cfg.alarm_secs * TPS end
    a.snd = sound.generateMelody(SIREN, { waveform = "square", attack = 2, decay = 10,
                                          sustain = 0.9, release = 10 })
    st.alarm = a
    sound.unmute()
    sound.setVolume(21)
    if a.snd then
        a.snd:setLoop(true)
        a.snd:play()
    end
    wake_screen()
end

local function alarm_stop()
    local a = st.alarm
    if not a then return end
    st.alarm = nil
    if a.snd then
        a.snd:stop()
        a.snd:delete()
    end
    sound.setVolume(a.vol)
    if a.muted then sound.mute() end
end

-- ── Messages ──────────────────────────────────────────────────────────────────

local function queue_message(text)
    st.outbox[#st.outbox + 1] = { text = cut(text, MSG_MAX), tries = 0, next_tick = st.ticks }
end

-- One send attempt per tick at most, oldest first. MT-Lite refuses a DM
-- until it has the peer's key (it asks for it), hence the retries.
local function service_outbox()
    local item = st.outbox[1]
    if not item or st.ticks < item.next_tick then return end
    item.tries = item.tries + 1
    local sent = cfg.target_name ~= "" and _lora_proto_send_text(cfg.target_name, item.text)
    print("[SENTRY] DM to " .. cfg.target_name .. ", try " .. item.tries .. ": " ..
          (sent and "sent" or "refused"))
    if sent then
        table.remove(st.outbox, 1)
        note("Sent to " .. cfg.target_name)
    elseif item.tries >= SEND_TRIES then
        table.remove(st.outbox, 1)
        note("Not sent: the protocol refused it " .. SEND_TRIES .. " times")
    else
        item.next_tick = st.ticks + SEND_RETRY_S * TPS
    end
end

local function compose(key, detail)
    local t = msg_text(key)
    if cfg.details then
        local bits = {}
        if detail then bits[#bits + 1] = detail end
        local now = utils.now()
        if now > CLOCK_SET then bits[#bits + 1] = utils.clockHM(now) end
        if #bits > 0 then t = t .. " (" .. table.concat(bits, ", ") .. ")" end
    end
    return t
end

local function fire(key, detail)
    local last = st.fired[key]
    if last and st.ticks - last < cfg.cooldown_min * 60 * TPS then
        print("[SENTRY] " .. key .. " again within the wait between messages: not fired")
        return
    end
    st.fired[key] = st.ticks
    local what = TRIG[key].title .. (detail and (": " .. detail) or "")
    print("[SENTRY] fire " .. what .. " (alarm " .. onoff(cfg[key .. "_alarm"]) ..
          ", message " .. onoff(cfg[key .. "_msg"]) .. ")")
    note(what)
    pcall(_notify_post, NAME .. ": " .. what)
    if cfg[key .. "_alarm"] then alarm_start(key) end
    if cfg[key .. "_msg"] then queue_message(compose(key, detail)) end
end

-- ── Watchers (run while armed) ────────────────────────────────────────────────

local function watching(key)
    return cfg[key .. "_on"] and not UNAVAILABLE[key]
end

local function watch_noise()
    if not watching("noise") or st.alarm then return end
    if not st.mic.open and not mic_open() then return end
    local peak = _mic_level() or 0
    st.mic.peak = peak
    if st.ticks < st.mic.ignore_until then return end
    local hist = st.mic.hist
    hist[#hist + 1] = (peak >= cfg.noise_level) and 1 or 0
    if #hist > 5 then table.remove(hist, 1) end
    local hits = 0
    for _, h in ipairs(hist) do hits = hits + h end
    if hits >= 2 then
        st.mic.hist = {}
        fire("noise", "level " .. peak .. " of 32767")
    end
end

local function haversine_m(lat1, lon1, lat2, lon2)
    local p1, p2 = math.rad(lat1), math.rad(lat2)
    local dp, dl = p2 - p1, math.rad(lon2 - lon1)
    local a = math.sin(dp / 2) ^ 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ^ 2
    return 2 * 6371000 * math.asin(math.min(1, math.sqrt(a)))
end

-- A sync cycle per check: start one (or join the one running), wait until
-- it has been seen running and has ended, then read its fix.
local function watch_move()
    if not watching("move") then return end
    local g = st.gps
    if g.phase == "rest" then
        if st.ticks < g.next_tick then return end
        g.phase = "start"
    end
    if g.phase == "start" then
        _gps_sync_start()
        g.phase, g.seen_running, g.waited = "wait", false, 0
        return
    end
    local done, has_loc = _gps_sync_status()
    if not done then g.seen_running = true; return end
    if not g.seen_running then
        g.waited = g.waited + 1
        if g.waited < 3 * TPS then return end
    end
    g.phase = "rest"
    g.next_tick = st.ticks + cfg.move_every * 60 * TPS
    local _, _, have, lat, lng, _, hdop = _gps_info()
    if not (has_loc and have) then
        g.note = "no GPS fix in the last check"
        if not g.ref then g.next_tick = st.ticks + 30 * TPS end
        return
    end
    if hdop and hdop > 5 then
        g.note = string.format("fix too coarse (HDOP %.1f)", hdop)
        return
    end
    if not g.ref then
        g.ref, g.note, g.dist = { lat = lat, lng = lng }, nil, 0
        note("Movement: start position taken")
        return
    end
    g.note = nil
    g.dist = haversine_m(g.ref.lat, g.ref.lng, lat, lng)
    if g.dist >= cfg.move_m then
        fire("move", string.format("moved %d m, now %.5f,%.5f", math.floor(g.dist + 0.5), lat, lng))
    end
end

local open_code_prompt     -- defined with the views

local function start_entry()
    if st.entry or st.prompt then return end
    capture_off()                                  -- the code prompt needs the keys
    if cfg.touch_grace <= 0 then
        fire("touch", nil)
        open_code_prompt()
        return
    end
    st.entry = { deadline = st.ticks + cfg.touch_grace * TPS }
    wake_screen()
    open_code_prompt()
end

local function watch_touch()
    if not watching("touch") or st.entry or st.prompt then
        st.touched = false
        return
    end
    local seen = {}
    if st.touched then seen[#seen + 1] = "screen press on the armed view" end
    st.touched = false
    if st.capture then
        local code = _input_capture_poll()
        if code then seen[#seen + 1] = string.format("key or trackball 0x%02X", code) end
    end
    if _touch_pressed() then seen[#seen + 1] = "pointer down (_touch_pressed)" end
    if #seen > 0 then
        print("[SENTRY] touch: " .. table.concat(seen, ", "))
        start_entry()
    end
end

local function watch_entry()
    local e = st.entry
    if e and st.ticks >= e.deadline then
        st.entry = nil
        fire("touch", nil)
    end
end

-- ── Views ─────────────────────────────────────────────────────────────────────

local vw = nil          -- the current view (one child of root at a time)
local ui = {}           -- the current view's live widgets; ui.refresh updates them
local view_leave = nil  -- run when the current view is replaced
local shown = setmetatable({}, { __mode = "k" })

local function set_text(lbl, s)
    if lbl and shown[lbl] ~= s then
        lbl.text = s
        shown[lbl] = s
    end
end

local function swap_view(builder)
    if view_leave then
        local f = view_leave
        view_leave = nil
        f()
    end
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
    if fn then b:onClicked(fn) end
    return b, l
end

-- A full-width row: its name on the left, a value on the right; fn(value_label).
local function row(parent, text, value, fn)
    local b = parent:Button { w = lvgl.PCT(100), h = 26 }
    b:Label { text = elide(text, 24), align = lvgl.ALIGN.LEFT_MID }
    local v = b:Label { text = value or "", align = lvgl.ALIGN.RIGHT_MID }
    b:onClicked(function() fn(v) end)
    return b, v
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

-- cb(text) -> error text to show, or nil (+ a function to run after closing:
-- a view swap must come after the modal has left the nav stack).
local function prompt(title, initial, opts, cb)
    opts = opts or {}
    modal(function(box, close)
        box:Label { text = title, w = lvgl.PCT(100) }
        local ta = box:Textarea {
            one_line = true, text = initial or "", w = lvgl.PCT(100), h = 32,
            max_length = opts.max_length or TEXT_MAX, password_mode = opts.password or false,
        }
        ta:clear_flag(lvgl.FLAG.SCROLLABLE)
        if opts.accepted then ta:set { accepted_chars = opts.accepted } end
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

local function close_prompt()
    if st.prompt then
        local p = st.prompt
        st.prompt = nil
        p.close()
    end
end

local show_main, show_trigger, show_general, show_picker, show_armed

local function disarm()
    st.phase = "idle"
    st.entry = nil
    st.touched = false
    capture_off()
    alarm_stop()
    mic_close()
    _mic_hold(NAME, false)
    close_prompt()
    note("Disarmed")
    print("[SENTRY] disarmed")
    show_main()
end

-- The disarm prompt: the code (when one is set) or a plain Disarm button.
-- Cancel is offered only while no grace period runs and no alarm sounds.
open_code_prompt = function()
    if st.prompt then return end
    local p = {}
    st.prompt = p
    p.close = modal(function(box, close)
        p.title = box:Label { text = "", w = lvgl.PCT(100) }
        local ta
        if cfg.code ~= "" then
            ta = box:Textarea { one_line = true, text = "", w = lvgl.PCT(100), h = 32,
                                password_mode = true, max_length = 16 }
            ta:clear_flag(lvgl.FLAG.SCROLLABLE)
        end
        p.err = box:Label { text = "", w = lvgl.PCT(100) }
        local function try()
            if ta and (ta.text or "") ~= cfg.code then
                p.err.text = "Wrong code."
                ta.text = ""
                return
            end
            disarm()
        end
        local ok_btn = box:Button { w = lvgl.PCT(100), h = 28 }
        ok_btn:Label { text = "Disarm", align = lvgl.ALIGN.CENTER }
        ok_btn:onevent(lvgl.EVENT.RELEASED, try)
        if ta then
            ta:onevent(lvgl.EVENT.KEY, function()
                if lvgl.indev.get_act():get_key() == lvgl.KEY.ENTER then try() end
            end)
        end
        if not st.entry and not st.alarm then
            local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
            cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
            cancel_btn:onevent(lvgl.EVENT.RELEASED, function()
                st.prompt = nil
                close()
                if st.phase == "armed" and watching("touch") then capture_on() end
            end)
        end
    end)
end

local function prompt_title()
    local p = st.prompt
    if not p then return end
    local what = (cfg.code ~= "") and "Enter the code" or "Press Disarm"
    if st.alarm then
        set_text(p.title, "ALARM (" .. TRIG[st.alarm.kind].title .. "). " .. what .. " to stop it.")
    elseif st.entry then
        local left = math.max(0, (st.entry.deadline - st.ticks + TPS - 1) // TPS)
        set_text(p.title, what .. " within " .. left .. " s.")
    else
        set_text(p.title, what .. ".")
    end
end

-- ── Disarmed screen ───────────────────────────────────────────────────────────

local function trigger_state(key)
    if UNAVAILABLE[key] then return "n/a" end
    return onoff(cfg[key .. "_on"])
end

local function can_arm()
    local any = false
    for _, t in ipairs(TRIGGERS) do
        if watching(t.key) then
            any = true
            if cfg[t.key .. "_msg"] and cfg.target_name == "" then
                return nil, "Choose who gets the messages (Send to) first."
            end
        end
    end
    if not any then return nil, "Turn on at least one trigger first." end
    return true
end

local main_note = nil

local function arm()
    local ok, err = can_arm()
    if not ok then
        main_note = err
        show_main()
        return
    end
    main_note = nil
    st.phase = "arming"
    st.arm_at = st.ticks + cfg.exit_delay * TPS
    st.fired = {}
    st.gps = { phase = "start", ref = nil, next_tick = 0, seen_running = false, waited = 0 }
    -- The microphone hardware stays up while armed (_mic_hold): the session
    -- the alarm closes reopens on running hardware.
    if watching("noise") then _mic_hold(NAME, true) end
    note("Armed")
    local on = {}
    for _, t in ipairs(TRIGGERS) do
        if watching(t.key) then on[#on + 1] = t.key end
    end
    print(string.format("[SENTRY] arming in %d s: %s; noise threshold %d, touch grace %d s, send to '%s'",
                        cfg.exit_delay, table.concat(on, " "), cfg.noise_level, cfg.touch_grace,
                        cfg.target_name))
    show_armed()
end

local function begin_watch()
    st.phase = "armed"
    if watching("touch") then capture_on() end
    print("[SENTRY] armed")
    show_armed()
end

local function last_event()
    return st.events[#st.events] or ""
end

show_main = function()
    swap_view(function(v)
        local c = content_of(v)
        c:Label { text = NAME, w = lvgl.PCT(70), h = 22 }
        button(c, "Home", lvgl.PCT(28), 22, function() apps.go_home() end)
        c:Label { text = main_note or "Disarmed.", w = lvgl.PCT(100) }
        local arm_btn = button(c, "Arm", lvgl.PCT(100), 36, arm)
        for _, t in ipairs(TRIGGERS) do
            row(c, t.title, trigger_state(t.key), function() show_trigger(t.key) end)
        end
        row(c, "Send to", cfg.target_name ~= "" and elide(cfg.target_name, 16) or "(choose)",
            function() show_picker("") end)
        row(c, "Disarm code", cfg.code ~= "" and "Set" or "None", function(val)
            prompt("Disarm code (empty = none)", "", { max_length = 16, password = true }, function(text)
                cfg.code = text
                save_cfg()
                val.text = cfg.code ~= "" and "Set" or "None"
            end)
        end)
        row(c, "Delays and alarm", "", function() show_general() end)
        row(c, "Send a test message", "", function()
            if cfg.target_name == "" then
                main_note = "Choose who gets the messages (Send to) first."
                show_main()
                return
            end
            queue_message(compose("test", nil))
            note("Test message queued")
        end)
        ui.event = c:Label { text = "", w = lvgl.PCT(100) }
        ui.refresh = function() set_text(ui.event, last_event()) end
        nav.set_focused(arm_btn)
    end)
end

-- ── Trigger settings ──────────────────────────────────────────────────────────

local function level_test(on)
    if on then
        st.test = mic_open()
    else
        st.test = false
        if st.phase == "idle" then mic_close() end
    end
end

show_trigger = function(key)
    local t = TRIG[key]
    swap_view(function(v)
        local c = content_of(v)
        c:Label { text = t.title, w = lvgl.PCT(70), h = 22 }
        local back = button(c, "Back", lvgl.PCT(28), 22, function() show_main() end)
        local why = UNAVAILABLE[key]
        if why then c:Label { text = why, text_color = "#ff8844", w = lvgl.PCT(100) } end
        row(c, t.watch, why and "n/a" or onoff(cfg[key .. "_on"]), function(val)
            if why then return end
            cfg[key .. "_on"] = not cfg[key .. "_on"]
            save_cfg()
            val.text = onoff(cfg[key .. "_on"])
        end)
        row(c, "Alarm", onoff(cfg[key .. "_alarm"]), function(val)
            cfg[key .. "_alarm"] = not cfg[key .. "_alarm"]
            save_cfg()
            val.text = onoff(cfg[key .. "_alarm"])
        end)
        row(c, "Message", onoff(cfg[key .. "_msg"]), function(val)
            cfg[key .. "_msg"] = not cfg[key .. "_msg"]
            save_cfg()
            val.text = onoff(cfg[key .. "_msg"])
        end)
        local text_lbl
        row(c, "Message text", "Edit", function()
            prompt("Message text (" .. TEXT_MAX .. " characters at most)", msg_text(key), nil,
                function(text)
                    if text == TRIG[key].text then text = "" end
                    cfg[key .. "_text"] = text
                    save_cfg()
                    text_lbl.text = msg_text(key)
                end)
        end)
        text_lbl = c:Label { text = msg_text(key), w = lvgl.PCT(100) }

        if key == "noise" then
            row(c, "Threshold", tostring(cfg.noise_level), function(val)
                prompt("Threshold: the peak level that fires (" .. LEVEL_MIN .. " to " .. LEVEL_MAX .. ")",
                       tostring(cfg.noise_level), { max_length = 5, accepted = "0123456789" },
                       function(text)
                           local n = math.tointeger(tonumber(text))
                           if not n or n < LEVEL_MIN or n > LEVEL_MAX then
                               return "A whole number from " .. LEVEL_MIN .. " to " .. LEVEL_MAX .. "."
                           end
                           cfg.noise_level = n
                           save_cfg()
                           val.text = tostring(n)
                       end)
            end)
            if not why then
                row(c, "Level test", "Off", function(val)
                    level_test(not st.test)
                    val.text = onoff(st.test)
                end)
                ui.level = c:Label { text = "", w = lvgl.PCT(100) }
                ui.refresh = function()
                    if st.test and st.mic.open then
                        st.mic.peak = _mic_level() or 0
                        set_text(ui.level, "Peak " .. st.mic.peak .. "; it fires at " ..
                                           cfg.noise_level .. ".")
                    elseif st.mic.err then
                        set_text(ui.level, "Microphone: " .. st.mic.err)
                    else
                        set_text(ui.level, "")
                    end
                end
                view_leave = function() level_test(false) end
            end
        elseif key == "move" then
            row(c, "Distance", cfg.move_m .. " m", function(val)
                cfg.move_m = cycle(MOVE_M, cfg.move_m)
                save_cfg()
                val.text = cfg.move_m .. " m"
            end)
            row(c, "Check every", cfg.move_every .. " min", function(val)
                cfg.move_every = cycle(MOVE_EVERY, cfg.move_every)
                save_cfg()
                val.text = cfg.move_every .. " min"
            end)
            c:Label { text = "Each check runs a GPS sync; the first one after arming takes the start position.",
                      w = lvgl.PCT(100) }
        elseif key == "touch" then
            local hint
            local function hint_text()
                local s = (cfg.touch_grace > 0)
                    and ("A touch or key press starts " .. cfg.touch_grace ..
                         " s; the disarm code within them disarms, else it fires.")
                    or  "The first touch or key press fires it; the disarm code then stops the alarm."
                if cfg.code == "" then s = s .. " No disarm code is set: Disarm is a plain button." end
                return s
            end
            row(c, "Grace period", secs_text(cfg.touch_grace), function(val)
                cfg.touch_grace = cycle(TOUCH_GRACE, cfg.touch_grace)
                save_cfg()
                val.text = secs_text(cfg.touch_grace)
                hint.text = hint_text()
            end)
            hint = c:Label { text = hint_text(), w = lvgl.PCT(100) }
        end
        nav.set_focused(back)
    end)
end

-- ── General settings ──────────────────────────────────────────────────────────

local function alarm_len_text(s)
    return s > 0 and secs_text(s) or "Until disarmed"
end

show_general = function()
    swap_view(function(v)
        local c = content_of(v)
        c:Label { text = "Delays and alarm", w = lvgl.PCT(70), h = 22 }
        local back = button(c, "Back", lvgl.PCT(28), 22, function() show_main() end)
        row(c, "Arming delay", secs_text(cfg.exit_delay), function(val)
            cfg.exit_delay = cycle(EXIT_DELAY, cfg.exit_delay)
            save_cfg()
            val.text = secs_text(cfg.exit_delay)
        end)
        row(c, "Alarm length", alarm_len_text(cfg.alarm_secs), function(val)
            cfg.alarm_secs = cycle(ALARM_SECS, cfg.alarm_secs)
            save_cfg()
            val.text = alarm_len_text(cfg.alarm_secs)
        end)
        row(c, "Wait between messages", cfg.cooldown_min .. " min", function(val)
            cfg.cooldown_min = cycle(COOLDOWN_MIN, cfg.cooldown_min)
            save_cfg()
            val.text = cfg.cooldown_min .. " min"
        end)
        row(c, "Add details", onoff(cfg.details), function(val)
            cfg.details = not cfg.details
            save_cfg()
            val.text = onoff(cfg.details)
        end)
        c:Label { text = "Details: the noise level, the distance and position, the time.",
                  w = lvgl.PCT(100) }
        nav.set_focused(back)
    end)
end

-- ── Contact picker ────────────────────────────────────────────────────────────

local function set_target(p)
    cfg.target_name = p.name
    cfg.target_id = p.id
    save_cfg()
    main_note = nil
    show_main()
end

-- MeshCore sends to the first contact (list order) whose name starts with
-- the given name; that contact, when it is not `p`.
local function prefix_catch(p, peers)
    local proto = _lora_proto()
    if proto ~= "meshcore" then return nil end
    for _, q in ipairs(peers) do
        if q.name:sub(1, #p.name) == p.name then
            if q ~= p then return q end
            return nil
        end
    end
    return nil
end

show_picker = function(filter)
    local ok, peers = pcall(_lora_proto_peers, MAX_PEERS)
    if not ok or type(peers) ~= "table" then peers = {} end
    local f = filter:lower()
    local rows = {}
    for _, p in ipairs(peers) do
        if f == "" or p.name:lower():find(f, 1, true) then rows[#rows + 1] = p end
    end
    table.sort(rows, function(a, b) return a.name:lower() < b.name:lower() end)
    swap_view(function(v)
        local c = content_of(v)
        c:Label { text = "Send to", w = lvgl.PCT(70), h = 22 }
        local back = button(c, "Back", lvgl.PCT(28), 22, function() show_main() end)
        local proto = _lora_proto()
        local head = #rows .. " of " .. #peers .. " contacts (" .. tostring(proto) .. ")"
        if #peers >= MAX_PEERS then head = head .. "; the list holds " .. MAX_PEERS .. " at most" end
        c:Label { text = head, w = lvgl.PCT(100) }
        row(c, "Filter", filter ~= "" and elide(filter, 12) or "(none)", function()
            prompt("Show contacts whose name contains", filter, { max_length = 31 }, function(text)
                return nil, function() show_picker(text) end
            end)
        end)
        local first
        for _, p in ipairs(rows) do
            local b = row(c, p.name, p.id, function()
                local other = prefix_catch(p, peers)
                if other then
                    confirm("Send to " .. p.name .. "?",
                            "On MeshCore a message to this name goes to " .. other.name ..
                            ", the first contact whose name starts with it.",
                            function() set_target(p) end)
                else
                    set_target(p)
                end
            end)
            first = first or b
        end
        if #peers == 0 then
            c:Label { text = "The contact list is empty.", w = lvgl.PCT(100) }
        end
        nav.set_focused(first or back)
    end)
end

-- ── Armed screen ──────────────────────────────────────────────────────────────

local function noise_line()
    if not watching("noise") then return nil end
    if st.alarm then return "Noise: paused while the alarm sounds" end
    if st.mic.err then return "Noise: microphone " .. st.mic.err end
    if not st.mic.open then return "Noise: waiting" end
    return "Noise: level " .. st.mic.peak .. " (fires at " .. cfg.noise_level .. ")"
end

local function move_line()
    if not watching("move") then return nil end
    local g = st.gps
    if not g then return nil end
    if not g.ref then
        return "Movement: getting the start position" .. (g.note and (" (" .. g.note .. ")") or "")
    end
    local s = string.format("Movement: %d m from the start (fires at %d m)",
                            math.floor((g.dist or 0) + 0.5), cfg.move_m)
    if g.note then s = s .. "; " .. g.note end
    return s
end

local function touch_line()
    if not watching("touch") then return nil end
    if st.entry then return "Touch: grace period running" end
    return "Touch: watching"
end

local function standby_line()
    local ok, on = pcall(_auto_standby_get)
    if ok and on and not watching("noise") then
        local okm, mins = pcall(_auto_standby_mins_get)
        return "Auto standby is on: after " .. tostring(okm and mins or "?") ..
               " min without input the device sleeps and stops watching (Settings > Power)."
    end
    return nil
end

show_armed = function()
    swap_view(function(v)
        local c = content_of(v)
        c:onevent(lvgl.EVENT.PRESSED, function()
            if st.phase == "armed" then st.touched = true end
        end)
        ui.big = c:Label { text = "", w = lvgl.PCT(100),
                           text_font = lvgl.BUILTIN_FONT.MONTSERRAT_28, text_color = "#ff4444" }
        ui.lines = c:Label { text = "", w = lvgl.PCT(100) }
        ui.events = c:Label { text = "", w = lvgl.PCT(100), text_color = "#bbbbbb" }
        local btn, lbl = button(c, "", lvgl.PCT(100), 32, function()
            if st.phase == "arming" then
                st.phase = "idle"
                _mic_hold(NAME, false)
                note("Arming cancelled")
                show_main()
            elseif watching("touch") then
                start_entry()
            else
                open_code_prompt()
            end
        end)
        ui.btn_lbl = lbl
        local flash, painted = false, false
        ui.refresh = function()
            if st.phase == "arming" then
                local left = math.max(0, (st.arm_at - st.ticks + TPS - 1) // TPS)
                set_text(ui.big, "Arming " .. left .. " s")
                set_text(ui.btn_lbl, "Cancel")
            elseif st.alarm then
                set_text(ui.big, "ALARM")
                set_text(ui.btn_lbl, "Disarm")
                if st.ticks % 5 == 0 then
                    flash = not flash
                    painted = true
                    v:set { bg_color = flash and "#aa0000" or "#000000", bg_opa = 255 }
                end
            else
                set_text(ui.big, "Armed")
                set_text(ui.btn_lbl, "Disarm")
                if painted then
                    flash, painted = false, false
                    v:set { bg_opa = 0 }
                end
            end
            local lines = {}
            for _, fn in ipairs({ noise_line, move_line, touch_line }) do
                local s = fn()
                if s then lines[#lines + 1] = s end
            end
            if watching("power") then lines[#lines + 1] = "Power: watching" end
            if #st.outbox > 0 then lines[#lines + 1] = "Message: sending (" .. #st.outbox .. " waiting)" end
            local sb = standby_line()
            if sb then lines[#lines + 1] = sb end
            set_text(ui.lines, table.concat(lines, "\n"))
            local ev = {}
            for i = math.max(1, #st.events - 2), #st.events do ev[#ev + 1] = st.events[i] end
            set_text(ui.events, table.concat(ev, "\n"))
            prompt_title()
        end
        ui.refresh()
        nav.set_focused(btn)
    end)
end

-- ── Lifecycle ─────────────────────────────────────────────────────────────────

local function tick_body()
    st.ticks = st.ticks + 1
    if st.phase == "arming" then
        if st.ticks >= st.arm_at then begin_watch() end
    elseif st.phase == "armed" then
        watch_noise()
        watch_move()
        watch_touch()
        watch_entry()
    end
    local a = st.alarm
    if a and a.until_tick and st.ticks >= a.until_tick then alarm_stop() end
    service_outbox()
    if ui.refresh then ui.refresh() end
end

-- An error in a tick disarms everything and lands on the disarmed screen
-- with the error shown; each distinct error is printed once.
local last_err = nil
apps.add_timer { period = TICK_MS, cb = function()
    local ok, err = pcall(tick_body)
    if ok then return end
    err = tostring(err)
    local was_watching = st.phase ~= "idle"
    st.phase, st.entry = "idle", nil
    capture_off()
    pcall(alarm_stop)
    pcall(mic_close)
    pcall(_mic_hold, NAME, false)
    if err ~= last_err or was_watching then
        if err ~= last_err then
            last_err = err
            print("[SENTRY] " .. err)
            note("Error: " .. err)
        end
        main_note = "Stopped by an error: " .. err
        pcall(close_prompt)
        pcall(show_main)
    end
end }

-- Leaving the app (Home or the home chord) disarms it. Closed while armed
-- with the touch trigger on (the home chord), the touch message goes out
-- now: one send attempt, no retries once the app is gone.
apps.set_on_close(function()
    if st.phase == "armed" and watching("touch") then
        local what = TRIG.touch.title .. ": the app was closed while armed"
        pcall(_notify_post, NAME .. ": " .. what)
        if cfg.touch_msg and cfg.target_name ~= "" then
            pcall(_lora_proto_send_text, cfg.target_name,
                  cut(compose("touch", "the app was closed while armed"), MSG_MAX))
        end
    end
    st.phase = "idle"
    capture_off()
    alarm_stop()
    mic_close()
    _mic_hold(NAME, false)
    return true
end)

TRIG.test = { title = "Test", text = "Sentry test message." }
show_main()

return root
