-- Meshcore > Simple Mode — pick the contacts, channels and apps a
-- simple-mode device shows, then switch the launcher.
--
-- Three picker pages, switched by the Nodes / Channels / Apps buttons:
--   Nodes     starts as the favorited user contacts (plus anything already
--             picked); the search box repopulates it with name matches.
--   Channels  every channel on the device.
--   Apps      every installed app except this one.
-- Tapping a row ticks or unticks it. Ticks are kept across searches and
-- page switches.
--
-- Start swaps two files, each the same way: the normal file is renamed
-- (L:/lua/launcher.lua -> old_launcher.lua, L:/lua/lib/topbar.lua ->
-- lib/old_topbar.lua) and a new one is written from this folder's payload
-- (simple_launcher.lua / simple_topbar.lua) with one line put in front that
-- hands it this app's install folder (APP_DIR). The simple launcher's
-- Settings > Normal Mode reverses both. The launcher takes effect at once;
-- the top bar loads once at boot, so its swap shows after a restart.
--
-- The picks are saved in this app's folder — simple_contacts.txt (one
-- "<pubkey>\t<name>[\t<nickname>]" line each), simple_channels.txt (one
-- "<name>[\t<nickname>]" line each) and simple_apps.txt (one
-- "<registry id>\t<name>" line each); nicknames are set in simple mode's own
-- Settings — which the simple launcher finds through APP_DIR. It re-adds the
-- contacts to MeshCore and favorites them at boot.

local app_dir = ...

local lvgl    = require("lvgl")
local apps    = require("lib/apps")
local nav     = require("lib/nav")
local theme   = require("lib/theme")
local utils   = require("lib/utils")
local fileman = require("lib/fileman")

if apps.proto_gate("meshcore") then return end

local W, H = lvgl.HOR_RES(), lvgl.VER_RES()
local PAD = 4
local MAX_ROWS = 30                  -- rows shown per list; a search narrows it
local LIST_PATH    = app_dir .. "/simple_contacts.txt"
local CH_PATH      = app_dir .. "/simple_channels.txt"
local APPS_PATH    = app_dir .. "/simple_apps.txt"
local LAUNCHER     = "L:/lua/launcher.lua"
local OLD_LAUNCHER = "L:/lua/old_launcher.lua"
local TOPBAR       = "L:/lua/lib/topbar.lua"
local OLD_TOPBAR   = "L:/lua/lib/old_topbar.lua"
local PAYLOAD      = app_dir .. "/simple_launcher.lua"
local TB_PAYLOAD   = app_dir .. "/simple_topbar.lua"

-- True when launcher.lua is already the simple launcher, i.e. this app was
-- opened from simple mode's Settings. Start writes the APP_DIR line as the
-- launcher's first line; the normal launcher never begins with it. In that
-- state the launcher is left alone (old_launcher.lua IS the normal launcher
-- and must not be replaced): the start buttons become Done = save and return.
local APP_DIR_MARK = "local APP_DIR = "
local function simple_mode_active()
    local txt = fileman.read(LAUNCHER)
    return txt ~= nil and txt:sub(1, #APP_DIR_MARK) == APP_DIR_MARK
end
local in_simple = simple_mode_active()
local START_LABEL = in_simple and "Done" or "Start Simple Mode"

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)
theme.show_background()

local function toast(msg)
    pcall(utils.createNotification, root, tostring(msg), 2500)
end

-- A message too long for a toast: dimmed overlay + box with wrapped text and
-- an OK button, as its own nav scope (nav.pop BEFORE overlay:delete).
-- `after` (optional) runs once OK is pressed.
local function notice(text, after)
    local overlay = root:Object {
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
    box:Label { text = text, w = lvgl.PCT(100) }
    local ok_btn = box:Button { w = lvgl.PCT(100), h = 32 }
    ok_btn:Label { text = "OK", align = lvgl.ALIGN.CENTER }
    ok_btn:onevent(lvgl.EVENT.RELEASED, function()
        nav.pop()
        overlay:delete()
        if after then after() end
    end)
end

-- ── Saved picks (same formats the simple launcher reads) ────────────────────
-- chosen:      pubkey -> name, the ticked contacts.
-- chosen_ch:   channel name -> true, the ticked channels.
-- chosen_apps: registry id -> display name, the ticked apps.
-- All load once and are kept across searches and page switches.
-- A contact line may end in a third field, the nickname set in simple mode's
-- Settings > Nicknames. It is not edited here, only carried through a save.
local function load_saved()
    local set, nicks = {}, {}
    local f = io.open(LIST_PATH, "r")
    if not f then return set, nicks end
    local txt = f:read("*a") or ""
    f:close()
    for line in txt:gmatch("[^\r\n]+") do
        local pubkey, name, nick = line:match("^(%x+)\t([^\t]+)\t?(.*)$")
        if pubkey and #pubkey == 64 then
            pubkey = pubkey:lower()
            set[pubkey] = name
            if nick ~= "" then nicks[pubkey] = nick end
        end
    end
    return set, nicks
end

-- A channel line is <name>[\t<nickname>]; the nickname is carried through
-- a save the same way as a contact's.
local function load_saved_channels()
    local set, ch_nicks = {}, {}
    local f = io.open(CH_PATH, "r")
    if not f then return set, ch_nicks end
    local txt = f:read("*a") or ""
    f:close()
    for line in txt:gmatch("[^\r\n]+") do
        local name, nick = line:match("^([^\t]+)\t?(.*)$")
        if name then
            set[name] = true
            if nick ~= "" then ch_nicks[name] = nick end
        end
    end
    return set, ch_nicks
end

-- An app line is <registry id>\t<name>: the id is what apps.get resolves (a
-- catalog id from the app's .version, else its folder name); the name is
-- shown when the app is no longer installed.
local function load_saved_apps()
    local set = {}
    local f = io.open(APPS_PATH, "r")
    if not f then return set end
    local txt = f:read("*a") or ""
    f:close()
    for line in txt:gmatch("[^\r\n]+") do
        local id, name = line:match("^([^\t]+)\t([^\t]+)")
        if id then set[id] = name end
    end
    return set
end

local chosen, nicks = load_saved()
local chosen_ch, ch_nicks = load_saved_channels()
local chosen_apps = load_saved_apps()

local function count(set)
    local n = 0
    for _ in pairs(set) do n = n + 1 end
    return n
end

local function save_picks()
    local names = {}
    for key, name in pairs(chosen) do names[#names + 1] = { pubkey = key, name = name } end
    table.sort(names, function(a, b) return a.name:lower() < b.name:lower() end)
    local out = {}
    for _, c in ipairs(names) do
        local nick = nicks[c.pubkey]
        out[#out + 1] = c.pubkey .. "\t" .. c.name .. (nick and ("\t" .. nick) or "")
    end
    local ok, err = fileman.write(LIST_PATH, table.concat(out, "\n") .. "\n")
    if not ok then return false, err end

    local chans = {}
    for name in pairs(chosen_ch) do chans[#chans + 1] = name end
    table.sort(chans, function(a, b) return a:lower() < b:lower() end)
    local lines = {}
    for _, name in ipairs(chans) do
        local nick = ch_nicks[name]
        lines[#lines + 1] = name .. (nick and ("\t" .. nick) or "")
    end
    ok, err = fileman.write(CH_PATH, table.concat(lines, "\n") .. "\n")
    if not ok then return false, err end

    local app_rows = {}
    for id, name in pairs(chosen_apps) do app_rows[#app_rows + 1] = { id = id, name = name } end
    table.sort(app_rows, function(a, b) return a.name:lower() < b.name:lower() end)
    local app_lines = {}
    for _, a in ipairs(app_rows) do app_lines[#app_lines + 1] = a.id .. "\t" .. a.name end
    return fileman.write(APPS_PATH, table.concat(app_lines, "\n") .. "\n")
end

-- ── File swaps ──────────────────────────────────────────────────────────────
-- A payload becomes the live file with `local APP_DIR = "<this app's
-- folder>"` as its first line, so the installed copy stays on internal flash
-- and knows where the pick lists live. The payload must end with `tail` (a
-- short read is refused). The normal file is renamed aside first; the written
-- file is read back and compared, and on any failure the normal file is put
-- straight back.
local function build_payload(path, tail)
    local payload, perr = fileman.read(path)
    if not payload or payload == "" then
        return nil, "cannot read " .. fileman.basename(path) .. ": " .. tostring(perr)
    end
    if not payload:find(tail, 1, true) then
        return nil, fileman.basename(path) .. " read incomplete"
    end
    return APP_DIR_MARK .. string.format("%q", app_dir) .. "\n" .. payload
end

local function swap_in(live, old, text)
    if fileman.exists(old) then
        -- Left over from an earlier simple-mode run that a firmware update
        -- overwrote; the current live file is the one to keep.
        local ok, err = fileman.remove(old)
        if not ok then return false, "cannot remove stale " .. fileman.basename(old) .. ": " .. tostring(err) end
    end
    local ok, err = fileman.rename(live, old)
    if not ok then return false, "rename failed: " .. tostring(err) end
    ok, err = fileman.write(live, text)
    if ok and fileman.read(live) ~= text then
        ok, err = false, fileman.basename(live) .. " read back different"
    end
    if not ok then
        fileman.remove(live)
        fileman.rename(old, live)
        return false, "write failed: " .. tostring(err)
    end
    return true
end

local function swap_out(live, old)
    if not fileman.exists(old) then return false, fileman.basename(old) .. " is missing" end
    local ok, err = fileman.remove(live)
    if not ok then return false, "remove failed: " .. tostring(err) end
    ok, err = fileman.rename(old, live)
    if not ok then return false, "rename failed: " .. tostring(err) end
    return true
end

local function enable_simple_mode()
    local launcher_text, err = build_payload(PAYLOAD, "create = create_launcher")
    if not launcher_text then return false, err end
    local topbar_text
    topbar_text, err = build_payload(TB_PAYLOAD, "return M")
    if not topbar_text then return false, err end

    local ok
    ok, err = swap_in(LAUNCHER, OLD_LAUNCHER, launcher_text)
    if not ok then return false, "launcher: " .. tostring(err) end
    ok, err = swap_in(TOPBAR, OLD_TOPBAR, topbar_text)
    if not ok then
        swap_out(LAUNCHER, OLD_LAUNCHER)
        return false, "top bar: " .. tostring(err)
    end
    -- The next require("launcher") (apps.go_home) must load the new file.
    -- The top bar stays loaded until the next restart.
    package.loaded["launcher"] = nil
    return true
end

-- ── Rows for the Nodes page ─────────────────────────────────────────────────
-- query == "": favorited user contacts + everything already picked (a picked
-- contact whose MeshCore record is gone still shows, so it can be unticked).
-- query ~= "": user contacts whose name contains it (case-insensitive).
-- Rows come back picked first, then by name.
local function collect_nodes(query)
    local rows, seen = {}, {}
    local lq = query:lower()
    local ok_c, raw = pcall(_mesh_get_contacts)
    if not ok_c or type(raw) ~= "table" then raw = {} end
    for _, c in ipairs(raw) do
        if (c.type or 1) == 1 and c.pubkey and c.pubkey ~= "" then
            local key = c.pubkey:lower()
            seen[key] = true
            if chosen[key] then chosen[key] = c.name end   -- follow a rename
            local match
            if lq == "" then match = c.favorite or chosen[key] ~= nil
            else match = c.name:lower():find(lq, 1, true) ~= nil end
            if match then rows[#rows + 1] = { pubkey = key, name = c.name } end
        end
    end
    pcall(_mesh_drop_contacts_cache)
    if lq == "" then
        for key, name in pairs(chosen) do
            if not seen[key] then
                rows[#rows + 1] = { pubkey = key, name = name, missing = true }
            end
        end
    end
    table.sort(rows, function(a, b)
        local ca, cb = chosen[a.pubkey] ~= nil, chosen[b.pubkey] ~= nil
        if ca ~= cb then return ca end
        return a.name:lower() < b.name:lower()
    end)
    return rows
end

-- ── Rows for the Channels page ──────────────────────────────────────────────
-- Every channel on the device in slot order, then any picked channel that no
-- longer exists (so it can be unticked).
local function collect_channels()
    local rows, seen = {}, {}
    local ok, chans = pcall(_mesh_get_channels)
    if ok and type(chans) == "table" then
        for _, ch in ipairs(chans) do
            seen[ch.name] = true
            rows[#rows + 1] = { name = ch.name }
        end
    end
    for name in pairs(chosen_ch) do
        if not seen[name] then rows[#rows + 1] = { name = name, missing = true } end
    end
    return rows
end

-- ── Rows for the Apps page ──────────────────────────────────────────────────
-- Every launchable app in the registry except this one, by name, then any
-- picked app that is no longer installed (so it can be unticked).
local function collect_apps()
    local rows, seen = {}, {}
    for _, rec in ipairs(apps.all()) do
        if not rec.is_category and rec.entrypoint and rec.dir ~= app_dir then
            local id = rec.id or rec.name
            seen[id] = true
            rows[#rows + 1] = { id = id, name = rec.name }
        end
    end
    table.sort(rows, function(a, b) return a.name:lower() < b.name:lower() end)
    for id, name in pairs(chosen_apps) do
        if not seen[id] then rows[#rows + 1] = { id = id, name = name, missing = true } end
    end
    return rows
end

-- ── UI: one wrap-flex body per page, every control a direct child (gridnav) ─
local page = nil
local build

-- Rebuild the page (view = "nodes" | "channels" | "apps"), off the event
-- that asked.
local function request_build(view, query)
    nav.reset()
    apps.add_timer { period = 1, cb = function(t) t:delete(); build(view, query) end }
end

build = function(view, query)
    local old = page
    page = root:Object {
        flex = {
            flex_direction = "row",
            flex_wrap = "wrap",
            align_items = "center",
            align_content = "flex-start",
        },
        w = W, h = H, x = 0, y = 0,
        border_width = 0, pad_all = PAD, pad_row = PAD, pad_column = PAD, bg_opa = 0,
    }
    nav.replace(page)

    -- Row 1: Home / Save / Start (Start repeats under the list).
    local home_btn = page:Button { w = lvgl.PCT(22), h = 30 }
    home_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    local save_btn = page:Button { w = lvgl.PCT(22), h = 30 }
    save_btn:Label { text = "Save", align = lvgl.ALIGN.CENTER }
    local enable_btn = page:Button { w = lvgl.PCT(50), h = 30 }
    enable_btn:Label { text = START_LABEL, align = lvgl.ALIGN.CENTER }

    -- Row 2: page switch. The current page's button is marked.
    local function page_btn(key, label)
        local b = page:Button { w = lvgl.PCT(31), h = 30 }
        b:Label { text = (view == key) and ("[ " .. label .. " ]") or label, align = lvgl.ALIGN.CENTER }
        nav.tap(b, function()
            if view ~= key then request_build(key, "") end
        end)
    end
    page_btn("nodes", "Nodes")
    page_btn("channels", "Channels")
    page_btn("apps", "Apps")

    local search = nil   -- the Nodes page's search box; nil elsewhere

    -- One tick row per entry: `on(r)` says whether it is ticked, `flip(r)`
    -- toggles it, `extra(r)` is the note for a missing entry.
    local function tick_rows(rows, limit, on, flip, extra, refresh_status)
        local function row_text(r)
            return (on(r) and "[x] " or "[ ] ") .. utils.emojiText(r.name)
                .. (r.missing and extra or "")
        end
        for i = 1, math.min(#rows, limit) do
            local r = rows[i]
            local btn = page:Button { w = lvgl.PCT(100), h = 30 }
            local lbl = btn:Label { text = row_text(r), align = lvgl.ALIGN.LEFT_MID }
            nav.tap(btn, function()
                flip(r)
                lbl.text = row_text(r)
                refresh_status()
            end)
        end
    end

    if view == "nodes" then
        -- Row 3: search.
        search = page:Textarea {
            one_line = true, max_length = 31, text = query,
            w = lvgl.PCT(70), h = 34,
        }
        local search_btn = page:Button { w = lvgl.PCT(26), h = 34 }
        search_btn:Label { text = "Search", align = lvgl.ALIGN.CENTER }

        local status = page:Label { text = "", w = lvgl.PCT(100) }

        local rows = collect_nodes(query)
        local total = #rows
        local function refresh_status()
            local what = (query == "") and "Favorites" or ("Matches for '" .. query .. "'")
            local s = what .. " - " .. count(chosen) .. " picked"
            if total > MAX_ROWS then
                s = s .. " - showing " .. MAX_ROWS .. " of " .. total .. ", narrow the search"
            end
            status.text = s
        end
        refresh_status()

        if total == 0 then
            page:Label {
                text = (query == "") and "No favorite contacts. Search for a name to add one."
                    or "No user contacts match.",
                w = lvgl.PCT(100),
            }
        end
        tick_rows(rows, MAX_ROWS,
            function(r) return chosen[r.pubkey] ~= nil end,
            function(r)
                if chosen[r.pubkey] then chosen[r.pubkey] = nil
                else chosen[r.pubkey] = r.name end
            end,
            "  (not in contacts)", refresh_status)

        local function do_search()
            request_build("nodes", search.text or "")
        end
        search:onevent(lvgl.EVENT.KEY, function()
            local indev = lvgl.indev.get_act()
            if indev:get_key() == lvgl.KEY.ENTER then do_search() end
        end)
        nav.tap(search_btn, do_search)
    elseif view == "channels" then
        local status = page:Label { text = "", w = lvgl.PCT(100) }
        local function refresh_status()
            status.text = "Channels - " .. count(chosen_ch) .. " picked"
        end
        refresh_status()

        local rows = collect_channels()
        if #rows == 0 then
            page:Label { text = "No channels on this device.", w = lvgl.PCT(100) }
        end
        tick_rows(rows, #rows,
            function(r) return chosen_ch[r.name] ~= nil end,
            function(r)
                if chosen_ch[r.name] then chosen_ch[r.name] = nil
                else chosen_ch[r.name] = true end
            end,
            "  (channel removed)", refresh_status)
    else
        local status = page:Label { text = "", w = lvgl.PCT(100) }
        local function refresh_status()
            status.text = "Apps - " .. count(chosen_apps) .. " picked"
        end
        refresh_status()

        local rows = collect_apps()
        if #rows == 0 then
            page:Label { text = "No other apps installed.", w = lvgl.PCT(100) }
        end
        tick_rows(rows, #rows,
            function(r) return chosen_apps[r.id] ~= nil end,
            function(r)
                if chosen_apps[r.id] then chosen_apps[r.id] = nil
                else chosen_apps[r.id] = r.name end
            end,
            "  (not installed)", refresh_status)
    end

    local function do_save()
        local ok, err = save_picks()
        if ok then
            toast("Saved " .. count(chosen) .. " contact(s), " .. count(chosen_ch)
                .. " channel(s), " .. count(chosen_apps) .. " app(s)")
        else
            toast("Save failed: " .. tostring(err))
        end
        return ok
    end
    nav.tap(save_btn, do_save)

    local function start_simple()
        if not do_save() then return end
        if in_simple then
            apps.go_home()   -- back to the simple home page, rebuilt from the new picks
            return
        end
        -- The app's own gate (proto_gate) checks the protocol RUNNING now; a
        -- change made in Settings > Lora applies at the next restart. Starting
        -- with another protocol requested would leave simple mode without
        -- MeshCore after that restart.
        local okp, _, requested = pcall(_lora_proto)
        if okp and requested and requested ~= "meshcore" then
            notice("The LoRa protocol is set to change to '" .. tostring(requested)
                .. "' at the next restart. Simple Mode needs MeshCore. Set it back in Settings > Lora, then start Simple Mode.")
            return
        end
        local ok, err = enable_simple_mode()
        if not ok then
            notice(err)
            return
        end
        notice("Simple Mode is on. The top bar's message count switches to the picked chats after the next restart.",
            function() apps.go_home() end)
    end
    nav.tap(enable_btn, start_simple)

    -- Same button again under the list, so neither end needs a scroll.
    local enable_btn2 = page:Button { w = lvgl.PCT(100), h = 34 }
    enable_btn2:Label { text = START_LABEL, align = lvgl.ALIGN.CENTER }
    nav.tap(enable_btn2, start_simple)

    -- Space does the Start/Done action from anywhere on the page. Gridnav forwards
    -- every non-arrow key through the page to its focused child, so the page
    -- sees the spaces typed into the search box too; those are left alone.
    local FOCUS_BITS = lvgl.STATE.FOCUSED | lvgl.STATE.FOCUS_KEY
    page:onevent(lvgl.EVENT.KEY, function()
        local indev = lvgl.indev.get_act()
        if not indev or indev:get_key() ~= 32 then return end
        if search then
            local ok, st = pcall(function() return search:get_state() end)
            if ok and st and (st & FOCUS_BITS) ~= 0 then return end
        end
        start_simple()
    end)

    nav.tap(home_btn, function() apps.go_home() end)

    if old then apps.delete_view(old) end
end

build("nodes", "")

return root
