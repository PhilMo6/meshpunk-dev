--[[
  Simple Mode launcher.

  Meshcore > Simple Mode installs this file as L:/lua/launcher.lua (the
  normal launcher waits as L:/lua/old_launcher.lua). Same contract as the
  normal launcher: `create()` runs at boot and after every app exit
  (apps.go_home).

  APP_DIR is not defined in this file: the Simple Mode app writes
  `local APP_DIR = "<its install folder>"` as the first line of the installed
  copy. This file does not run from the app folder.

  Home lists the picked contacts (APP_DIR/simple_contacts.txt), the picked
  channels (APP_DIR/simple_channels.txt) and the picked apps
  (APP_DIR/simple_apps.txt); tapping a chat opens it, tapping an app launches
  it. The app also swaps L:/lua/lib/topbar.lua for simple_topbar.lua (the
  normal one waits as lib/old_topbar.lua); Normal Mode and the auto-revert
  below put both files back. Settings holds the volume bar, the theme
  picker and the Normal Mode switch, which puts old_launcher.lua back.

  The first create() after this file loads (= boot) checks the saved
  contacts against the MeshCore contact list: a missing one is re-imported
  from its saved public key, and every saved contact is favorited so the
  firmware does not evict it when the contact list fills.

  Every page is ONE wrap-flex body with its controls as direct children
  (gridnav only walks direct children); full-width items stack as rows.
]]

local lvgl     = require("lvgl")
local topbar   = require("lib/topbar")
local apps     = require("lib/apps")
local nav      = require("lib/nav")
local theme    = require("lib/theme")
local utils    = require("lib/utils")
local sound    = require("lib/sound")
local fileman  = require("lib/fileman")
local messages = require("lib/mesh/messages")

local W, H = lvgl.HOR_RES(), lvgl.VER_RES()
local TOP = 20                       -- topbar height
local PAD = 4                        -- body padding / row gap
local LIST_PATH    = APP_DIR .. "/simple_contacts.txt"
local CH_PATH      = APP_DIR .. "/simple_channels.txt"
local APPS_PATH    = APP_DIR .. "/simple_apps.txt"
local LAUNCHER     = "L:/lua/launcher.lua"
local OLD_LAUNCHER = "L:/lua/old_launcher.lua"
local TOPBAR       = "L:/lua/lib/topbar.lua"
local OLD_TOPBAR   = "L:/lua/lib/old_topbar.lua"
local PAGE = 20                      -- DM records shown when a chat opens
local MAX_TEXT_LEN = 160             -- wire cap for a DM

local COL_ME_BG, COL_ME_TX     = "#0b3d2e", "#d7f5e6"
local COL_THEM_BG, COL_THEM_TX = "#262626", "#f0f0f0"
local COL_META = "#9aa0a6"
local COL_FOCUS = "#ffffff"          -- selected-bubble border

local body = nil
local boot_checked = false           -- false = run the contact guard on the next create()
local guard_note = nil               -- what the guard did, shown on the home page
local show_home, show_chat, show_settings, show_names

-- ── Saved contact list ──────────────────────────────────────────────────────
-- One line per contact: <64-hex pubkey>\t<name>[\t<nickname>]. The pubkey is
-- the identity; the name is what the send/favorite/history bindings resolve
-- by; the nickname (optional, set in Settings > Nicknames) is display only.
local function load_contacts()
    local list = {}
    local f = io.open(LIST_PATH, "r")
    if not f then return list end
    local txt = f:read("*a") or ""
    f:close()
    for line in txt:gmatch("[^\r\n]+") do
        local pubkey, name, nick = line:match("^(%x+)\t([^\t]+)\t?(.*)$")
        if pubkey and #pubkey == 64 then
            list[#list + 1] = {
                pubkey = pubkey:lower(), name = name,
                nick = (nick ~= "") and nick or nil,
            }
        end
    end
    return list
end

-- What simple mode shows for a contact or channel: its nickname when one is
-- set, composed for the emoji font (names reach Lua raw; message text is
-- already composed C-side).
local function shown_name(c)
    return utils.emojiText(c.nick or c.name)
end

-- Picked channels, in file order: <name>[\t<nickname>]. A channel is matched
-- to its live slot by name each time the home page builds.
local function load_channels()
    local list = {}
    local f = io.open(CH_PATH, "r")
    if not f then return list end
    local txt = f:read("*a") or ""
    f:close()
    for line in txt:gmatch("[^\r\n]+") do
        local name, nick = line:match("^([^\t]+)\t?(.*)$")
        if name then
            list[#list + 1] = { name = name, nick = (nick ~= "") and nick or nil }
        end
    end
    return list
end

-- Picked apps: <registry id>\t<name> per line. The id is what apps.get
-- resolves; the name is shown when the app is no longer installed.
local function load_apps()
    local list = {}
    local f = io.open(APPS_PATH, "r")
    if not f then return list end
    local txt = f:read("*a") or ""
    f:close()
    for line in txt:gmatch("[^\r\n]+") do
        local id, name = line:match("^([^\t]+)\t([^\t]+)")
        if id then list[#list + 1] = { id = id, name = name } end
    end
    return list
end

local function save_channels(list)
    local out = {}
    for _, c in ipairs(list) do
        out[#out + 1] = c.name .. (c.nick and ("\t" .. c.nick) or "")
    end
    return fileman.write(CH_PATH, table.concat(out, "\n") .. "\n")
end

-- Every message listener a page may have set. Each page drops them all
-- before setting its own.
local function drop_listeners()
    messages:onDirectMessage(nil)
    messages:onMessage(nil)
    messages:onAck(nil)
end

local function save_contacts(list)
    local out = {}
    for _, c in ipairs(list) do
        out[#out + 1] = c.pubkey .. "\t" .. c.name .. (c.nick and ("\t" .. c.nick) or "")
    end
    return fileman.write(LIST_PATH, table.concat(out, "\n") .. "\n")
end

local function url_encode(s)
    return (tostring(s or ""):gsub("[^%w%-_%.~]", function(c)
        if c == " " then return "+" end
        return string.format("%%%02X", string.byte(c))
    end))
end

-- The MeshCore app's contact URI; the firmware's importCard parses it back.
local function contact_uri(name, pubkey)
    return "meshcore://contact/add?name=" .. url_encode(name)
        .. "&public_key=" .. pubkey .. "&type=1"
end

-- Boot-time guard: re-add missing saved contacts, favorite all of them.
-- Returns a status string when it changed anything, else nil.
local function contact_guard()
    local saved = load_contacts()
    if #saved == 0 then return nil end
    local ok, live = pcall(_mesh_get_contacts)
    if not ok or type(live) ~= "table" then return nil end   -- protocol not active
    local by_key = {}
    for _, c in ipairs(live) do
        if c.pubkey then by_key[c.pubkey:lower()] = c end
    end
    local readded, favorited, failed, renamed = 0, 0, 0, false
    for _, s in ipairs(saved) do
        local c = by_key[s.pubkey]
        if not c then
            pcall(_mesh_import_contact, contact_uri(s.name, s.pubkey))
            -- importCard prints "contact list full" and adds nothing when the
            -- list is full; the favorite call below then fails = counted.
            local okf, res = pcall(_mesh_set_contact_favorite, s.name, true)
            if okf and res then readded = readded + 1 else failed = failed + 1 end
        else
            if c.name ~= s.name then s.name = c.name; renamed = true end
            if not c.favorite then
                local okf, res = pcall(_mesh_set_contact_favorite, c.name, true)
                if okf and res then favorited = favorited + 1 end
            end
        end
    end
    pcall(_mesh_drop_contacts_cache)
    if renamed then save_contacts(saved) end
    local parts = {}
    if readded > 0 then parts[#parts + 1] = "Re-added " .. readded end
    if favorited > 0 then parts[#parts + 1] = "Favorited " .. favorited end
    if failed > 0 then parts[#parts + 1] = "Could not re-add " .. failed end
    if #parts == 0 then return nil end
    return table.concat(parts, ", ")
end

-- ── Page plumbing (mirrors launcher.lua) ────────────────────────────────────
-- Page swaps are deferred one tick: deleting the focused gridnav body inside
-- its own button event is a use-after-free on hardware.
local function request_swap(rebuild)
    nav.reset()
    lvgl.Timer({
        period = 1,
        cb = function(t)
            t:delete()
            rebuild()
        end,
    })
end

local bind_tap = nav.tap

-- A fresh full-screen wrap-flex page under the topbar. Scrollable unless the
-- caller says otherwise (the chat page scrolls its message list instead).
local function new_page(flags, scrollable)
    pcall(_gridnav_edge_lock, false)   -- release the chat's edge lock on any page change
    apps.set_background_listener(nil)
    if body then
        pcall(function() body:delete() end)
        body = nil
    end
    body = lvgl.Object({
        flex = {
            flex_direction = "row",
            flex_wrap = "wrap",
            align_items = "center",
            align_content = "flex-start",
        },
        w = W, h = H - TOP, x = 0, y = TOP,
        border_width = 0, pad_all = PAD, pad_row = PAD, pad_column = PAD,
        bg_opa = 0,
    })
    if not scrollable then body:clear_flag(lvgl.FLAG.SCROLLABLE) end
    nav.replace(body, { flags = flags })
    topbar.raise()
    theme.ensure_background()
    apps.clear_current()
    apps.set_root(body)
    return body
end

-- A non-interactive child: gridnav skips it and taps fall through to the
-- scrolling container beneath.
local function passive(obj)
    obj:clear_flag(lvgl.FLAG.SCROLLABLE)
    obj:clear_flag(lvgl.FLAG.CLICKABLE)
    return obj
end

-- Floating toast over whatever page is up. The holder is parentless (top of
-- the screen) and not clickable, so it never swallows a tap.
local function toast(msg)
    local holder = lvgl.Object { w = W, h = H, x = 0, y = 0, bg_opa = 0, border_width = 0, pad_all = 0 }
    holder:clear_flag(lvgl.FLAG.SCROLLABLE)
    holder:clear_flag(lvgl.FLAG.CLICKABLE)
    utils.createNotification(holder, tostring(msg), 2500)
    lvgl.Timer {
        period = 2800,
        cb = function(t)
            t:delete()
            pcall(function() holder:delete() end)
        end,
    }
end

-- Dimmed overlay + centered box as its own nav scope (house popup pattern:
-- nav.pop BEFORE overlay:delete).
local function modal(build)
    local overlay = lvgl.Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0,
        radius = 0,
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
end

local function self_name()
    local ok, info = pcall(_mesh_get_node_info)
    if ok and type(info) == "table" and info.name then return info.name end
    return "me"
end

-- ── Normal Mode switch ──────────────────────────────────────────────────────
-- Removes this launcher and renames old_launcher.lua back, then the same for
-- the top bar (lib/topbar.lua <- lib/old_topbar.lua). The caller rebuilds
-- the home screen through apps.go_home, which re-requires "launcher" — the
-- cleared package.loaded entry makes that load the restored file. The top
-- bar module already loaded stays until the next restart.
local function revert_to_normal()
    if not fileman.exists(OLD_LAUNCHER) then
        return false, "old_launcher.lua is missing"
    end
    local ok, err = fileman.remove(LAUNCHER)
    if not ok then return false, "remove failed: " .. tostring(err) end
    ok, err = fileman.rename(OLD_LAUNCHER, LAUNCHER)
    if not ok then
        return false, "rename failed: " .. tostring(err) .. " - no launcher.lua left, reflash"
    end
    package.loaded["launcher"] = nil
    if fileman.exists(OLD_TOPBAR) then
        ok, err = fileman.remove(TOPBAR)
        if ok then ok, err = fileman.rename(OLD_TOPBAR, TOPBAR) end
        if not ok then
            return false, "top bar restore failed: " .. tostring(err) .. " - lib/topbar.lua needs a reflash"
        end
    end
    return true
end

-- True when L:/lua/lib/topbar.lua is the simple top bar but the one running
-- is not: the swap was made this session and shows after a restart.
local function topbar_swap_pending()
    if topbar.simple_mode then return false end
    local f = io.open(TOPBAR, "r")
    if not f then return false end
    local head = f:read("*a") or ""
    f:close()
    return head:sub(1, 16) == "local APP_DIR = "
end

-- ── HOME: the picked contacts and channels ──────────────────────────────────
show_home = function()
    drop_listeners()
    local page = new_page(nav.ROLLOVER, true)

    passive(page:Label { text = "Chats", w = lvgl.PCT(60) })
    local set_btn = page:Button { w = lvgl.PCT(36), h = 32 }
    set_btn:Label { text = "Settings", align = lvgl.ALIGN.CENTER }
    bind_tap(set_btn, function() request_swap(show_settings) end)

    if guard_note then
        passive(page:Label { text = guard_note, w = lvgl.PCT(100), text_color = COL_META })
        guard_note = nil
    end

    local saved = load_contacts()
    local saved_ch = load_channels()
    local saved_apps = load_apps()
    local okn, info = pcall(_mesh_get_node_info)
    if not (okn and type(info) == "table") then
        passive(page:Label {
            text = "The MeshCore LoRa protocol is not running. Messaging needs it; Settings still works.",
            w = lvgl.PCT(100),
        })
    elseif #saved == 0 and #saved_ch == 0 and #saved_apps == 0 then
        passive(page:Label {
            text = "Nothing picked. Open Settings > Contacts, Channels and Apps to pick what to show.",
            w = lvgl.PCT(100),
        })
    end
    if topbar_swap_pending() then
        passive(page:Label {
            text = "Restart to make the top bar count only these chats.",
            w = lvgl.PCT(100), text_color = COL_META,
        })
    end

    local rows = {}   -- real name -> { lbl, c }, for live unread refresh
    local function row_text(c)
        local n = messages:unreadInDM(c.name)
        if n > 0 then return shown_name(c) .. "   " .. n .. " new" end
        return shown_name(c)
    end
    for _, c in ipairs(saved) do
        local btn = page:Button { w = lvgl.PCT(100), h = 40 }
        local lbl = btn:Label { text = row_text(c), align = lvgl.ALIGN.LEFT_MID }
        rows[c.name] = { lbl = lbl, c = c }
        bind_tap(btn, function()
            request_swap(function()
                show_chat { type = "dm", name = c.name, title = shown_name(c) }
            end)
        end)
    end

    -- Picked channels, matched by name to their live slot. A picked channel
    -- that no longer exists shows as a plain line, not a button.
    local idx_of = {}
    local okc, chans = pcall(_mesh_get_channels)
    if okc and type(chans) == "table" then
        for _, ch in ipairs(chans) do idx_of[ch.name] = ch.idx end
    end
    local ch_rows = {}   -- slot idx -> { lbl, c }, for live unread refresh
    local function ch_text(c)
        local n = messages:unreadInChannel(c.name)
        if n > 0 then return shown_name(c) .. "   " .. n .. " new" end
        return shown_name(c)
    end
    for _, c in ipairs(saved_ch) do
        local idx = idx_of[c.name]
        if idx then
            local btn = page:Button { w = lvgl.PCT(100), h = 40 }
            local lbl = btn:Label { text = ch_text(c), align = lvgl.ALIGN.LEFT_MID }
            ch_rows[idx] = { lbl = lbl, c = c }
            bind_tap(btn, function()
                request_swap(function()
                    show_chat { type = "channel", name = c.name, idx = idx, title = shown_name(c) }
                end)
            end)
        else
            passive(page:Label {
                text = shown_name(c) .. "  (channel removed)", w = lvgl.PCT(100), text_color = COL_META,
            })
        end
    end

    -- Picked apps, launched through the normal app loader; its exit comes
    -- back to this page through apps.go_home. An app that is no longer
    -- installed shows as a plain line.
    for _, a in ipairs(saved_apps) do
        local rec = apps.get(a.id)
        if rec and not rec.is_category and rec.entrypoint then
            local btn = page:Button { w = lvgl.PCT(100), h = 40 }
            btn:Label { text = rec.name, align = lvgl.ALIGN.LEFT_MID }
            bind_tap(btn, function() apps.launch(rec) end)
        else
            passive(page:Label {
                text = a.name .. "  (not installed)", w = lvgl.PCT(100), text_color = COL_META,
            })
        end
    end

    -- A message landing while the list is up bumps that row's count.
    messages:onDirectMessage(function(msg)
        local row = rows[msg.from]
        if row then pcall(function() row.lbl.text = row_text(row.c) end) end
    end)
    messages:onMessage(function(msg)
        local i = msg.channel_idx or 0
        if i < 0 then i = 0 end   -- unknown-channel traffic is stored under Public
        local row = ch_rows[i]
        if row then pcall(function() row.lbl.text = ch_text(row.c) end) end
    end)
end

-- ── CHAT: one thread ────────────────────────────────────────────────────────
-- target = { type = "dm", name, title } or { type = "channel", name, idx, title }.
-- `name` is the real contact/channel name every binding is keyed by; `title`
-- is what the screen shows for it (its nickname, if set; emoji-composed).
show_chat = function(target)
    drop_listeners()
    local page = new_page(nav.ROLLOVER, false)
    local me = self_name()
    local name = target.name
    local is_channel = (target.type == "channel")
    local title = target.title or utils.emojiText(name)

    -- In a channel, a sender who is a picked contact with a nickname shows
    -- under that nickname (matched by real name — channel messages carry no
    -- key); everyone else shows under their own name.
    local nick_of = {}
    if is_channel then
        for _, c in ipairs(load_contacts()) do
            if c.nick then nick_of[c.name] = c.nick end
        end
    end
    local function sender_name(from)
        return utils.emojiText(nick_of[from] or from or "?")
    end

    local back_btn = page:Button { w = 60, h = 30 }
    back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    passive(page:Label { text = title, w = W - 2 * PAD - 60 - PAD, pad_left = 6 })

    -- Rows: back strip 30 + list + input 34, three row gaps + body padding.
    local list_h = (H - TOP) - 2 * PAD - 30 - 34 - 2 * PAD
    local msg_list = page:Object {
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
        w = lvgl.PCT(100), h = list_h, border_width = 0, pad_all = 2,
        pad_row = 3, bg_opa = 0,
    }
    msg_list:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)

    -- Message select (the Messenger's scheme): a tap/click on the list (a
    -- touch drag scrolls instead — nav.tap) pushes a nav scope onto it, so
    -- the trackball steps bubble to bubble and gridnav scrolls each into
    -- view. NONE + the edge lock keep focus on the edge bubble: no wrap, no
    -- escape to the Back/Send buttons. 'q' returns to the controls.
    local in_msg_select = false
    bind_tap(msg_list, function()
        if in_msg_select then return end
        in_msg_select = true
        pcall(_gridnav_edge_lock, true)
        nav.push(msg_list, { flags = nav.NONE, preserve = true })
    end)
    msg_list:onevent(lvgl.EVENT.KEY, function()
        local indev = lvgl.indev.get_act()
        if indev:get_key() == 113 and in_msg_select then   -- 'q'
            in_msg_select = false
            pcall(_gridnav_edge_lock, false)
            nav.pop()
        end
    end)

    local status_labels = {}   -- own msg table -> its header label (ack updates)

    local function status_word(msg)
        local s = msg.status
        if s == "delivered" then return "delivered"
        elseif s == "failed" then return "failed"
        elseif s == "retrying" then
            return "retry " .. tostring(msg.retry_n or "?") .. "/" .. tostring(msg.retry_total or "?")
        elseif s == "sent" then return "sent" end
        return nil
    end

    local function head_text(msg, is_me)
        -- In a DM the other side is always this contact, so its bubbles carry
        -- the title (nickname); a channel names each sender (see sender_name).
        local who = is_me and "You" or ((not is_channel) and title or sender_name(msg.from))
        local head = who .. "  " .. utils.clockHM(msg.timestamp)
        if is_me then
            local w = status_word(msg)
            if w then head = head .. "  " .. w end
        end
        return head
    end

    local function add_bubble(msg)
        local is_me = (msg.from == me)
        local bubble = msg_list:Object {
            w = lvgl.PCT(92), h = lvgl.SIZE_CONTENT,
            bg_color = is_me and COL_ME_BG or COL_THEM_BG, bg_opa = 255,
            radius = 6, border_width = 1,
            border_color = is_me and COL_ME_BG or COL_THEM_BG,
            pad_all = 4, pad_bottom = 5,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        bubble:clear_flag(lvgl.FLAG.SCROLLABLE)
        -- Direct child of msg_list and focusable, so message select can land
        -- on it; the white border marks the selected bubble.
        bubble:set_style({ border_width = 1, border_color = COL_FOCUS }, lvgl.STATE.FOCUS_KEY)
        local hdr = bubble:Label {
            text = head_text(msg, is_me), w = lvgl.PCT(100), text_color = COL_META,
        }
        bubble:Label {
            text = msg.text or "", w = lvgl.PCT(100),
            text_color = is_me and COL_ME_TX or COL_THEM_TX,
        }
        if is_me and msg.status ~= nil then status_labels[msg] = hdr end
        return bubble
    end

    -- Newest PAGE records off the disk log.
    local okp, r
    if is_channel then okp, r = pcall(_store_chat_page_channel, name, 0, 0, PAGE)
    else okp, r = pcall(_store_chat_page_dm, name, 0, 0, PAGE) end
    local last = nil
    if okp and type(r) == "table" and type(r.list) == "table" then
        for _, m in ipairs(r.list) do last = add_bubble(m) end
    end
    if not last then
        msg_list:Label { text = "No messages yet.", w = lvgl.PCT(100), text_color = COL_META }
    end

    if is_channel then messages:markChannelSeen(name) else messages:markDMSeen(name) end
    topbar.updateUnread()

    -- Live arrivals (and our own send echo, which comes back through the same
    -- listener) are added from a one-shot timer, never inside the event that
    -- produced them.
    local pending = {}
    local scheduled = false
    local function flush()
        scheduled = false
        if not body then return end
        local newest = nil
        for _, m in ipairs(pending) do newest = add_bubble(m) end
        pending = {}
        if newest then pcall(function() newest:scroll_to_view(false) end) end
    end
    local function queue(msg)
        pending[#pending + 1] = msg
        if scheduled then return end
        scheduled = true
        apps.add_timer { period = 1, cb = function(t) t:delete(); flush() end }
    end

    if is_channel then
        messages:onMessage(function(msg)
            local i = msg.channel_idx or 0
            if i < 0 then i = 0 end   -- unknown-channel traffic is stored under Public
            if i == target.idx then
                queue(msg)
                messages:clearUnreadChannel(name)
            end
        end)
    else
        messages:onDirectMessage(function(msg)
            if msg.from == name or msg.to == name then
                queue(msg)
                messages:clearUnreadDM(name)
            end
        end)
        messages:onAck(function(msg)
            local lbl = status_labels[msg]
            if lbl then pcall(function() lbl.text = head_text(msg, true) end) end
        end)
    end

    -- A channel message goes on the wire as "<our name>: <text>", so the name
    -- and the ": " count against the cap; a DM gets all of it.
    local max_len = MAX_TEXT_LEN
    if is_channel then max_len = math.max(1, MAX_TEXT_LEN - #me - 2) end
    local ta = page:Textarea {
        one_line = true, max_length = max_len,
        w = lvgl.PCT(74), h = 34,
    }
    local send_btn = page:Button { w = lvgl.PCT(22), h = 34 }
    send_btn:Label { text = "Send", align = lvgl.ALIGN.CENTER }

    local function do_send()
        local text = ta.text
        if not text or #text == 0 then return end
        local sent
        if not is_channel then sent = messages:sendDirect(name, text)
        elseif target.idx == 0 then sent = messages:broadcast(text)
        else sent = messages:sendToChannel(target.idx, text) end
        if sent then
            ta.text = ""
        else
            toast("Send failed")
        end
    end
    ta:onevent(lvgl.EVENT.KEY, function()
        local indev = lvgl.indev.get_act()
        if indev:get_key() == lvgl.KEY.ENTER then do_send() end
    end)
    send_btn:onevent(lvgl.EVENT.RELEASED, do_send)

    bind_tap(back_btn, function()
        drop_listeners()
        request_swap(show_home)
    end)

    -- Open on the newest message.
    if last then
        page:update_layout()
        pcall(function() last:scroll_to_view(false) end)
    end
end

-- ── SETTINGS ────────────────────────────────────────────────────────────────
show_settings = function()
    drop_listeners()
    local page = new_page(nav.ROLLOVER + nav.SCROLL_FIRST, true)

    local back_btn = page:Button { w = 60, h = 32 }
    back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    passive(page:Label { text = "Settings", w = W - 2 * PAD - 60 - PAD, pad_left = 6 })
    bind_tap(back_btn, function() request_swap(show_home) end)

    -- Volume (same 21-step bar as Settings > Sound).
    local vol_label = passive(page:Label { text = "", w = lvgl.PCT(100) })
    local bar_row = passive(page:Object {
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
        w = lvgl.PCT(100), h = 18, border_width = 0,
        pad_hor = 0, pad_ver = 2, pad_column = 1, bg_opa = 0,
    })
    local segs = {}
    local seg_w = math.min(13, math.floor((W - 12) / 21) - 1)
    for i = 1, 21 do
        segs[i] = passive(bar_row:Object { w = seg_w, h = 14, border_width = 1, pad_all = 0, bg_color = "#24ba24" })
    end
    local btn_dn = page:Button { w = lvgl.PCT(30), h = 32 }
    btn_dn:Label { text = "Vol -", align = lvgl.ALIGN.CENTER }
    local btn_up = page:Button { w = lvgl.PCT(30), h = 32 }
    btn_up:Label { text = "Vol +", align = lvgl.ALIGN.CENTER }
    local btn_mute = page:Button { w = lvgl.PCT(34), h = 32 }
    local lbl_mute = btn_mute:Label { align = lvgl.ALIGN.CENTER }

    local function refresh_volume()
        local v, muted = sound.getVolume(), sound.isMuted()
        vol_label.text = "Volume: " .. v .. "/21" .. (muted and " (muted)" or "")
        for i = 1, 21 do
            segs[i]:set { bg_opa = (muted or i > v) and 40 or 255 }
        end
        lbl_mute.text = muted and "Unmute" or "Mute"
    end
    refresh_volume()
    btn_dn:onClicked(function()
        local v = sound.getVolume()
        if v > 0 then sound.setVolume(v - 1) end
        refresh_volume()
    end)
    btn_up:onClicked(function()
        local v = sound.getVolume()
        if v < 21 then sound.setVolume(v + 1) end
        refresh_volume()
    end)
    btn_mute:onClicked(function()
        sound.toggleMute()
        refresh_volume()
    end)

    -- Theme picker: the normal Settings > Theme app; its Back returns to the
    -- simple home page through apps.go_home.
    local theme_btn = page:Button { w = lvgl.PCT(100), h = 40 }
    theme_btn:Label { text = "Themes", align = lvgl.ALIGN.CENTER }
    bind_tap(theme_btn, function()
        if apps.get("Theme") then apps.launch("Theme")
        else toast("Theme app not found") end
    end)

    -- The picker is the Simple Mode app itself, found by its install folder
    -- (its registry key changes once it is published with a catalog id). It
    -- sees launcher.lua is already this file and offers Done, not Start.
    local pick_btn = page:Button { w = lvgl.PCT(100), h = 40 }
    pick_btn:Label { text = "Contacts, Channels and Apps", align = lvgl.ALIGN.CENTER }
    bind_tap(pick_btn, function()
        local rec = nil
        for _, r in ipairs(apps.all()) do
            if r.dir == APP_DIR then rec = r break end
        end
        if rec then
            boot_checked = false   -- re-run the contact guard over the edited picks on return
            apps.launch(rec)
        else
            toast("Simple Mode app not found")
        end
    end)

    local names_btn = page:Button { w = lvgl.PCT(100), h = 40 }
    names_btn:Label { text = "Nicknames", align = lvgl.ALIGN.CENTER }
    bind_tap(names_btn, function() request_swap(show_names) end)

    local normal_btn = page:Button { w = lvgl.PCT(100), h = 40 }
    normal_btn:Label { text = "Normal Mode", align = lvgl.ALIGN.CENTER }
    bind_tap(normal_btn, function()
        modal(function(box, close)
            box:Label {
                text = "Switch back to the normal launcher with all apps? The top bar's message count returns to all chats after the next restart.",
                w = lvgl.PCT(100),
            }
            local yes = box:Button { w = lvgl.PCT(100), h = 32 }
            yes:Label { text = "Switch to Normal Mode", align = lvgl.ALIGN.CENTER }
            yes:onevent(lvgl.EVENT.RELEASED, function()
                local ok, err = revert_to_normal()
                close()
                if ok then
                    apps.go_home()
                else
                    toast(err)
                end
            end)
            local no = box:Button { w = lvgl.PCT(100), h = 32 }
            no:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
            no:onevent(lvgl.EVENT.RELEASED, close)
        end)
    end)
end

-- ── NICKNAMES (Settings > Nicknames) ────────────────────────────────────────
-- One row per picked contact and per picked channel; tapping it opens a box
-- to set or clear the name simple mode shows for it. Only the nickname field
-- of that line changes — the real name (and a contact's pubkey) is untouched.
show_names = function()
    drop_listeners()
    local page = new_page(nav.ROLLOVER, true)

    local back_btn = page:Button { w = 60, h = 32 }
    back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    passive(page:Label { text = "Nicknames", w = W - 2 * PAD - 60 - PAD, pad_left = 6 })
    bind_tap(back_btn, function() request_swap(show_settings) end)

    local contacts = load_contacts()
    local channels = load_channels()
    if #contacts == 0 and #channels == 0 then
        passive(page:Label { text = "Nothing picked.", w = lvgl.PCT(100) })
        return
    end
    passive(page:Label { text = "Tap a row to change the name shown for it.", w = lvgl.PCT(100) })

    local function row_text(c)
        if c.nick then return shown_name(c) .. "  (" .. utils.emojiText(c.name) .. ")" end
        return utils.emojiText(c.name)
    end

    -- `save` writes the list the entry belongs to; `lbl` is its row label.
    local function edit_nick(c, save, lbl)
        modal(function(box, close)
            box:Label { text = "Nickname for " .. utils.emojiText(c.name), w = lvgl.PCT(100) }
            local ta = box:Textarea {
                one_line = true, max_length = 31, text = c.nick or "",
                w = lvgl.PCT(100), h = 34,
            }
            local function apply(nick)
                -- Trim; a tab would break the line format, an empty name clears.
                nick = (nick or ""):gsub("\t", " "):match("^%s*(.-)%s*$")
                local before = c.nick
                c.nick = (nick ~= "") and nick or nil
                local ok, err = save()
                close()
                if ok then
                    lbl.text = row_text(c)
                else
                    c.nick = before
                    toast("Save failed: " .. tostring(err))
                end
            end
            local save_b = box:Button { w = lvgl.PCT(100), h = 32 }
            save_b:Label { text = "Save", align = lvgl.ALIGN.CENTER }
            save_b:onevent(lvgl.EVENT.RELEASED, function() apply(ta.text) end)
            ta:onevent(lvgl.EVENT.KEY, function()
                local indev = lvgl.indev.get_act()
                if indev:get_key() == lvgl.KEY.ENTER then apply(ta.text) end
            end)
            local clear_b = box:Button { w = lvgl.PCT(100), h = 32 }
            clear_b:Label { text = "Use real name", align = lvgl.ALIGN.CENTER }
            clear_b:onevent(lvgl.EVENT.RELEASED, function() apply("") end)
            local cancel_b = box:Button { w = lvgl.PCT(100), h = 32 }
            cancel_b:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
            cancel_b:onevent(lvgl.EVENT.RELEASED, close)
        end)
    end

    local function section(title, list, save)
        if #list == 0 then return end
        passive(page:Label { text = title, w = lvgl.PCT(100), text_color = COL_META })
        for _, c in ipairs(list) do
            local btn = page:Button { w = lvgl.PCT(100), h = 40 }
            local lbl = btn:Label { text = row_text(c), align = lvgl.ALIGN.LEFT_MID }
            bind_tap(btn, function() edit_nick(c, save, lbl) end)
        end
    end
    section("Contacts", contacts, function() return save_contacts(contacts) end)
    section("Channels", channels, function() return save_channels(channels) end)
end

-- Simple mode needs the MeshCore protocol. If it is not the one running (the
-- protocol was switched and the device restarted), simple mode turns itself
-- off: the normal launcher is put back and built in this same call. The picks
-- stay saved for the next Start.
local function meshcore_running()
    if type(_lora_proto) ~= "function" then return true end
    return _lora_proto() == "meshcore"
end

local function create_launcher()
    if not meshcore_running() then
        local ok, err = revert_to_normal()
        if ok then
            print("[SimpleMode] MeshCore not running - switched back to the normal launcher")
            require("launcher").create()
            return
        end
        print("[SimpleMode] MeshCore not running and revert failed: " .. tostring(err))
        guard_note = "Could not switch to Normal Mode: " .. tostring(err)
        show_home()
        return
    end
    if not boot_checked then
        boot_checked = true
        guard_note = contact_guard()
        if guard_note then print("[SimpleMode] " .. guard_note) end
    end
    show_home()
end

return {
    create = create_launcher,
}
