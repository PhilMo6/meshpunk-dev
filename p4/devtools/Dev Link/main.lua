-- Tools/Dev Link — TEMPORARY test link (dev tree only, not for release).
-- Connects OUT to a PC-side server (scratchpad devlink_server.py) over
-- _tcp_open and speaks newline-delimited JSON, so the mesh on this device can
-- be driven and watched from the PC without anyone at the device.
--   device -> PC: {"ev":"hello", node, proto, ip} on connect;
--                 {"ev":"pkt", seq, ts, ms, dir, len, rssi, snr, score,
--                  hash, raw} for every frame in the capture ring
--                 (dir = rx / tx / txfail, raw = hex wire frame);
--                 {"ev":"unread", total} when the unread total changes;
--                 {"re": id, "ok": bool, "result": ..., "err": ...} per
--                 command.
--   PC -> device: one JSON object per line, {"id": n, "cmd": ...}:
--                 "eval"  {"lua": "return ..."}  runs Lua in this app's
--                         environment (every _mesh_* binding is reachable);
--                 "send_dm" {"to", "text"}, "send_channel" {"idx", "text"},
--                 "capture" {"on"}, "status", "ping".
-- One file (the JSON codec is inline) so the same folder runs from internal
-- flash or from an SD card's /meshpunk/apps/Tools/Dev Link (the SD loader
-- runs the entry file alone). Background contract
-- (apps.register_background): the link keeps running while other apps are
-- open; an ELF launch tears it down with Lua, and the app has to be opened
-- again after every boot.
local app_dir = ...
local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")
local utils = require("lib/utils")

-- ── JSON codec ──────────────────────────────────────────────────────────────
-- Inline so the one-file app also runs from an SD card (the SD loader runs
-- the entry file alone). encode: nil -> null, booleans, numbers (integers as
-- integers, non-finite as null), strings (control characters and quotes
-- escaped, other bytes passed through), tables as arrays when the keys are
-- 1..n, else as objects with string keys (other key types skipped); depth
-- capped at 8 so a cyclic value cannot recurse forever. decode: objects,
-- arrays, strings (\uXXXX to UTF-8), numbers, true/false/null (null -> nil).
local json = (function()
    local M = {}

    local escapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
                      ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }

    local function encode_string(s)
        return '"' .. s:gsub('[%c"\\]', function(c)
            return escapes[c] or string.format("\\u%04x", c:byte())
        end) .. '"'
    end

    local function is_array(t)
        local n = 0
        for k in pairs(t) do
            if math.type(k) ~= "integer" or k < 1 then return false end
            n = n + 1
        end
        return n == #t
    end

    local function encode_value(v, depth)
        local tv = type(v)
        if v == nil then return "null"
        elseif tv == "boolean" then return v and "true" or "false"
        elseif tv == "number" then
            if v ~= v or v == math.huge or v == -math.huge then return "null" end
            if math.type(v) == "integer" then return string.format("%d", v) end
            return string.format("%.14g", v)
        elseif tv == "string" then return encode_string(v)
        elseif tv == "table" then
            if depth > 8 then return '"<depth>"' end
            local parts = {}
            if is_array(v) then
                for i = 1, #v do parts[#parts + 1] = encode_value(v[i], depth + 1) end
                return "[" .. table.concat(parts, ",") .. "]"
            end
            for k, val in pairs(v) do
                if type(k) == "string" then
                    parts[#parts + 1] = encode_string(k) .. ":" .. encode_value(val, depth + 1)
                elseif type(k) == "number" then
                    parts[#parts + 1] = encode_string(tostring(k)) .. ":" .. encode_value(val, depth + 1)
                end
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
        return encode_string(tostring(v))
    end

    function M.encode(v) return encode_value(v, 0) end

    local decode_value

    local function skip_ws(s, i)
        local _, e = s:find("^[ \t\r\n]*", i)
        return e + 1
    end

    local function decode_string(s, i)
        -- i points at the opening quote
        local out = {}
        i = i + 1
        while true do
            local c = s:sub(i, i)
            if c == "" then return nil, "unterminated string" end
            if c == '"' then return table.concat(out), i + 1 end
            if c == "\\" then
                local e = s:sub(i + 1, i + 1)
                if e == "u" then
                    local hex = s:sub(i + 2, i + 5)
                    if not hex:match("^%x%x%x%x$") then return nil, "bad \\u escape" end
                    out[#out + 1] = utf8.char(tonumber(hex, 16))
                    i = i + 6
                else
                    local map = { ['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t' }
                    if not map[e] then return nil, "bad escape" end
                    out[#out + 1] = map[e]
                    i = i + 2
                end
            else
                out[#out + 1] = c
                i = i + 1
            end
        end
    end

    local function decode_number(s, i)
        local num = s:match("^-?%d+%.?%d*[eE]?[-+]?%d*", i)
        if not num or num == "" then return nil, "bad number" end
        local v = tonumber(num)
        if v == nil then return nil, "bad number" end
        return v, i + #num
    end

    decode_value = function(s, i)
        i = skip_ws(s, i)
        local c = s:sub(i, i)
        if c == "{" then
            local obj = {}
            i = skip_ws(s, i + 1)
            if s:sub(i, i) == "}" then return obj, i + 1 end
            while true do
                i = skip_ws(s, i)
                if s:sub(i, i) ~= '"' then return nil, "expected key" end
                local k; k, i = decode_string(s, i)
                if k == nil then return nil, i end
                i = skip_ws(s, i)
                if s:sub(i, i) ~= ":" then return nil, "expected colon" end
                local v; v, i = decode_value(s, i + 1)
                if i == nil then return nil, v end
                obj[k] = v
                i = skip_ws(s, i)
                local d = s:sub(i, i)
                if d == "," then i = i + 1
                elseif d == "}" then return obj, i + 1
                else return nil, "expected , or }" end
            end
        elseif c == "[" then
            local arr = {}
            i = skip_ws(s, i + 1)
            if s:sub(i, i) == "]" then return arr, i + 1 end
            while true do
                local v; v, i = decode_value(s, i)
                if i == nil then return nil, v end
                arr[#arr + 1] = v
                i = skip_ws(s, i)
                local d = s:sub(i, i)
                if d == "," then i = i + 1
                elseif d == "]" then return arr, i + 1
                else return nil, "expected , or ]" end
            end
        elseif c == '"' then
            return decode_string(s, i)
        elseif s:sub(i, i + 3) == "true" then return true, i + 4
        elseif s:sub(i, i + 4) == "false" then return false, i + 5
        elseif s:sub(i, i + 3) == "null" then return nil, i + 4
        else
            return decode_number(s, i)
        end
    end

    -- Returns the value (nil for JSON null or on error) and, on error, the reason.
    function M.decode(s)
        local v, i = decode_value(s, 1)
        if i == nil then return nil, v end
        return v
    end

    return M
end)()

local HOST, PORT   = "192.168.0.117", 7777   -- the PC running devlink_server.py
local TICK_MS      = 100
local RECONNECT_S  = 5
local PKT_MAX      = 16

-- Survivable state (background contract): the socket and queues live here so
-- a relaunch rebinds to the running link instead of opening a second one.
local KEY = "devlink"
local rec = apps.background_of(KEY)
local st = (rec and rec.state) or {}
st.rx_buf     = st.rx_buf or ""
st.tx_queue   = st.tx_queue or {}
st.last_try   = st.last_try or 0
st.stats      = st.stats or { in_lines = 0, out_lines = 0, pkts = 0, connects = 0, drops = 0 }
st.last_event = st.last_event or "-"
st.unread     = st.unread or -1
st.armed      = st.armed or false
st.enabled    = (st.enabled == nil) and true or st.enabled

-- ── Link plumbing ───────────────────────────────────────────────────────────

local function queue_line(obj)
    st.tx_queue[#st.tx_queue + 1] = json.encode(obj) .. "\n"
end

local function sock_state()
    if not st.sock then return "none" end
    local ok, s = pcall(function() return st.sock:state() end)
    return ok and s or "closed"
end

local function close_sock(reason)
    if st.sock then pcall(function() st.sock:close() end) end
    st.sock = nil
    st.rx_buf = ""
    st.last_event = "closed: " .. tostring(reason)
end

local function try_connect()
    local now = utils.now()
    if now - st.last_try < RECONNECT_S then return end
    st.last_try = now
    local ok, sock, err = pcall(_tcp_open, HOST, PORT, { timeout_ms = 8000 })
    if ok and sock then
        st.sock = sock
        st.rx_buf = ""
        st.last_event = "connecting to " .. HOST .. ":" .. PORT
    else
        st.last_event = "open failed: " .. tostring(ok and err or sock)
    end
end

local hello_sent = false

local function send_hello()
    local info = {}
    pcall(function() info = _mesh_get_node_info() or {} end)
    local wstate, ssid, ip = "", "", ""
    pcall(function() wstate, ssid, ip = _wifi_status() end)
    local active = ""
    pcall(function() active = (_lora_proto()) end)
    queue_line({ ev = "hello", node = info.name, pubkey = info.pubkey, proto = active,
                 wifi = wstate, ssid = ssid, ip = ip, board = (_device_caps and _device_caps().name) })
    st.stats.connects = st.stats.connects + 1
end

-- Flush queued lines; stop at the first the TX ring cannot take whole.
local function flush_tx()
    while #st.tx_queue > 0 do
        local line = st.tx_queue[1]
        local ok, n, err = pcall(function() return st.sock:send(line) end)
        if not ok or not n then
            if ok and err and err ~= "" and err ~= "buffer full" and err ~= "tx full" then
                st.last_event = "send: " .. tostring(err)
            end
            return
        end
        table.remove(st.tx_queue, 1)
        st.stats.out_lines = st.stats.out_lines + 1
    end
end

-- ── Commands ────────────────────────────────────────────────────────────────

-- eval runs the PC's Lua in this chunk's global environment; a chunk that
-- is a bare expression is wrapped in "return".
local function run_eval(code)
    local fn, err = load(code, "devlink", "t")
    if not fn then
        fn, err = load("return " .. code, "devlink", "t")
        if not fn then return false, err end
    end
    local res = table.pack(pcall(fn))
    if not res[1] then return false, tostring(res[2]) end
    if res.n <= 1 then return true, nil end
    if res.n == 2 then return true, res[2] end
    local out = {}
    for i = 2, res.n do out[i - 1] = res[i] end
    return true, out
end

local handlers = {}

handlers.ping = function() return true, "pong" end

handlers.status = function()
    local wstate, ssid, ip = "", "", ""
    pcall(function() wstate, ssid, ip = _wifi_status() end)
    return true, { stats = st.stats, armed = st.armed, wifi = wstate, ssid = ssid, ip = ip,
                   unread = st.unread, queue = #st.tx_queue, uptime = utils.now() }
end

handlers.eval = function(c)
    if type(c.lua) ~= "string" then return false, "lua missing" end
    return run_eval(c.lua)
end

handlers.send_dm = function(c)
    if type(c.to) ~= "string" or type(c.text) ~= "string" then return false, "to/text missing" end
    local ok, sent, err = pcall(_mesh_send_direct, c.to, c.text)
    if not ok then return false, tostring(sent) end
    if not sent then return false, tostring(err or "send refused") end
    return true, true
end

handlers.send_channel = function(c)
    if type(c.idx) ~= "number" or type(c.text) ~= "string" then return false, "idx/text missing" end
    local ok, sent, err = pcall(_mesh_send_channel, math.floor(c.idx), c.text)
    if not ok then return false, tostring(sent) end
    if not sent then return false, tostring(err or "send refused") end
    return true, true
end

handlers.capture = function(c)
    local on = c.on ~= false
    local ok, armed = pcall(_mesh_pkt_capture, on)
    st.armed = ok and on and armed and true or false
    return ok, st.armed
end

local function handle_line(line)
    st.stats.in_lines = st.stats.in_lines + 1
    local cmd, derr = json.decode(line)
    if type(cmd) ~= "table" then
        queue_line({ re = 0, ok = false, err = "bad json: " .. tostring(derr) })
        return
    end
    local h = handlers[cmd.cmd or ""]
    if not h then
        queue_line({ re = cmd.id or 0, ok = false, err = "unknown cmd " .. tostring(cmd.cmd) })
        return
    end
    local ok, res_ok, result = pcall(h, cmd)
    if not ok then
        queue_line({ re = cmd.id or 0, ok = false, err = tostring(res_ok) })
    elseif res_ok then
        queue_line({ re = cmd.id or 0, ok = true, result = result })
    else
        queue_line({ re = cmd.id or 0, ok = false, err = tostring(result) })
    end
    st.last_event = "cmd " .. tostring(cmd.cmd)
end

-- ── Event sources ───────────────────────────────────────────────────────────

local function poll_packets()
    if not st.armed then return end
    local ok, pkts, dropped = pcall(_mesh_pkt_poll, PKT_MAX)
    if not ok or type(pkts) ~= "table" then return end
    if type(dropped) == "number" and dropped > 0 then
        st.stats.drops = st.stats.drops + dropped
    end
    for _, p in ipairs(pkts) do
        p.ev = "pkt"
        queue_line(p)
        st.stats.pkts = st.stats.pkts + 1
        st.last_event = p.dir .. " " .. tostring(p.len) .. "B"
    end
end

local function poll_unread()
    local ok, total = pcall(_mesh_unread_total)
    if ok and type(total) == "number" and total ~= st.unread then
        st.unread = total
        queue_line({ ev = "unread", total = total })
    end
end

-- ── Tick (foreground timer while open; the manager's heartbeat after) ───────

local function tick()
    if not st.enabled then return end
    local s = sock_state()
    if s == "none" or s == "closed" then
        if s == "closed" then
            local _, _, reason = pcall(function() return st.sock:state() end)
            close_sock(reason)
        end
        hello_sent = false
        try_connect()
        return
    end
    if s ~= "open" then return end          -- connecting / untrusted
    if not hello_sent then send_hello(); hello_sent = true end

    -- Inbound commands.
    local ok, data = pcall(function() return st.sock:recv(4096) end)
    if ok and type(data) == "string" and #data > 0 then
        st.rx_buf = st.rx_buf .. data
        while true do
            local nl = st.rx_buf:find("\n", 1, true)
            if not nl then break end
            local line = st.rx_buf:sub(1, nl - 1):gsub("\r$", "")
            st.rx_buf = st.rx_buf:sub(nl + 1)
            if #line > 0 then handle_line(line) end
        end
        if #st.rx_buf > 16384 then st.rx_buf = "" end   -- a line that long is garbage
    end

    poll_packets()
    poll_unread()
    flush_tx()
end

-- Arm the capture ring on first start (the Packets app disarms it when it
-- closes; capture {"on":true} re-arms from the PC).
if not st.armed then
    local ok, armed = pcall(_mesh_pkt_capture, true)
    st.armed = ok and armed and true or false
end

apps.register_background {
    key      = KEY,
    app_name = "Dev Link",
    state    = st,
    period   = TICK_MS,
    tick     = tick,
    on_close = function()
        st.enabled = false
        close_sock("closed by user")
        pcall(_mesh_pkt_capture, false)
        st.armed = false
    end,
    status   = function()
        return "Dev Link: " .. sock_state() .. " | " .. st.last_event
    end,
}

-- ── UI ──────────────────────────────────────────────────────────────────────

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)
theme.show_background()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6, bg_opa = 0,
}
nav.replace(content)

content:Label { text = "Dev Link (test)", w = lvgl.PCT(70), h = 26 }
local bg_btn = content:Button { w = 70, h = 22 }
bg_btn:Label { text = "Background", align = lvgl.ALIGN.CENTER }

local l_target = content:Label { text = "PC: " .. HOST .. ":" .. PORT, w = lvgl.PCT(100), h = 16 }
local l_state  = content:Label { text = "", w = lvgl.PCT(100), h = 16 }
local l_stats  = content:Label { text = "", w = lvgl.PCT(100), h = 16 }
local l_event  = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local function refresh()
    local wstate, ssid, ip = "", "", ""
    pcall(function() wstate, ssid, ip = _wifi_status() end)
    l_state.text = "link: " .. sock_state() .. "  wifi: " .. tostring(wstate) .. " " .. tostring(ip)
    l_stats.text = string.format("in %d  out %d  pkts %d  drops %d  queue %d  capture %s",
        st.stats.in_lines, st.stats.out_lines, st.stats.pkts, st.stats.drops,
        #st.tx_queue, st.armed and "on" or "off")
    l_event.text = st.last_event
end

local reconnect_btn = content:Button { w = lvgl.PCT(48), h = 30 }
reconnect_btn:Label { text = "Reconnect now", align = lvgl.ALIGN.CENTER }
reconnect_btn:onClicked(function()
    close_sock("reconnect requested")
    st.last_try = 0
    st.enabled = true
    hello_sent = false
end)

local stop_btn = content:Button { w = lvgl.PCT(48), h = 30 }
stop_btn:Label { text = "Stop link + Home", align = lvgl.ALIGN.CENTER }
stop_btn:onClicked(function()
    apps.close_background(KEY)
    apps.go_home()
end)

content:Label {
    text = "Temporary test tool: the PC drives the mesh over this link. Background keeps it running while other apps are open; it must be opened again after a reboot.",
    w = lvgl.PCT(100),
}

bg_btn:onClicked(function() apps.go_background(KEY) end)

-- Foreground heartbeat: the same tick the manager runs while backgrounded.
apps.add_timer { period = TICK_MS, cb = function() tick(); refresh() end }
refresh()

return root
