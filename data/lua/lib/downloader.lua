--[[
  downloader — shared engine for on-device catalog downloads. The App Library
  is its catalog client (apps, themes, drivers, LoRa protocols); Settings >
  Firmware and Tools > Flasher use its wifi_wait. Anything that installs
  content from the app catalogs (the meshpunk-apps and meshpunk-apps-dev
  GitHub repos) goes through here so the catalog format, staging discipline
  and .version bookkeeping can never diverge.

  The repo's catalog.toml carries four TOML table-arrays with identical
  entry shape: "apps", "themes", "drivers" (USB .drv.elf modules) and
  "protocols" (LoRa .loraproto.elf packages) — id (repo folder), name,
  version, author, description, type, category (apps only), files (relative
  paths to download from <base_url>/<kind>/<id>/), min_fw (optional
  integer: the minimum firmware API level — the _FW_API Lua global — the
  entry's files need), drivers (apps only, optional: a list of ids from the
  drivers table-array that the app depends on; the App Library auto-installs
  missing ones right after the app, to the app's location),
  requires_protocol (apps only, optional: the LoRa protocol id the app runs
  under — the App Library asks to download it first when it is missing;
  never automatic), apps (protocols only, optional: the protocol's primary
  app ids, offered for download right after the protocol installs),
  requires (optional: device-requirement tokens that must ALL hold on this
  device - the "Device requirements" section below), files_xtensa /
  files_rv32 (optional: files only that build target downloads, on top of
  files; this device's list is device_files()).
  NOTE: never write double-square-bracket TOML names inside this header —
  it is a Lua long comment and two adjacent closing square brackets in the
  text terminate it early (that exact bug has now happened twice).
  Entries with path-hostile ids/names/files ("..", "/", "\") are dropped at
  parse time — a hostile catalog must not be able to write outside its own
  staging dir.

  Install discipline (see the App Library header for the full story):
    * staging dirs live OUTSIDE the apps/themes bases (partial downloads must
      never be discovered), on the SAME drive as the destination so the final
      fileman.rename() is one atomic hop;
    * a .version file (version\nlocation\ncategory[\nlocked]\nid=<id>
      \ntarget=<build target>\ncatalog=<catalog id>) inside the installed
      dir marks it store-managed and drives update detection;
    * callers own any post-install cache refresh (apps.refresh() for apps;
      themes need none — lib/theme rescans on every list/apply).

  UI: run_install/run_remove build their own progress modal over the caller's
  root (nav scope pushed/popped here); everything else is UI-free.
]]

local lvgl    = require("lvgl")
local apps    = require("lib/apps")
local nav     = require("lib/nav")
local fileman = require("lib/fileman")
local toml    = require("lib/toml")

local M = {}

-- The catalogs the downloads come from: "stable" (the published apps) and
-- "dev" (apps in development, and versions that need a dev firmware's API
-- level). catalog_default() picks one from the running firmware; the App
-- Library's switch stores the other in CATALOG_CFG while it differs from
-- that default. S:/meshpunk/appstore.toml replaces both ("custom"):
--   [source]
--   url = "https://raw.githubusercontent.com/you/your-apps-repo/main"
-- The source is resolved once and kept; reload_source() drops it, which the
-- App Library does at every launch so an edited or deleted config takes
-- effect without a reboot. The App Library carries its own copy of these
-- rules for firmware whose downloader predates them (that app updates
-- through the catalog, this lib only with the firmware).
M.CATALOGS = {
    stable = "https://raw.githubusercontent.com/PhilMo6/meshpunk-apps/main",
    dev    = "https://raw.githubusercontent.com/PhilMo6/meshpunk-apps-dev/main",
}
local CONFIG_PATH = "S:/meshpunk/appstore.toml"
local CATALOG_CFG = "L:/app_library.cfg"

-- Staging + catalog caches. Cleanup removes DIRS here; the caches are files.
M.STAGING = { sd = "S:/meshpunk/.appstore", internal = "L:/lua/.appstore" }

-- Below this many free bytes a drive cannot hold the two folders every
-- install creates (staging dir, then the final dir): the mkdir fails before
-- any download starts. run_install refuses such a drive. App Library's
-- location picker carries its own copy of this value: that app is
-- store-updated onto firmware whose downloader may predate this field.
M.LOW_SPACE = 16 * 1024

-- The remedy line for an out-of-space refusal at `loc` ("sd" | "internal").
function M.low_space_msg(loc)
    return "Low space on " .. (loc == "sd" and "the SD card" or "internal storage")
        .. ". Free up space and try again."
end

-- The catalog this firmware reads unless its user picks the other: "dev"
-- when the installed release tag (_ota_info().installed, the git describe
-- string a release build's data pack writes) is a dev tag, or empty (a build
-- flashed without a release data pack); "stable" for any other tag and on
-- firmware without _ota_info (before FW_API 13).
function M.catalog_default()
    if type(_ota_info) ~= "function" then return "stable" end
    local tag = tostring(_ota_info().installed or "")
    if tag == "" or tag:find("-dev", 1, true) then return "dev" end
    return "stable"
end

-- The URL S:/meshpunk/appstore.toml names, or nil.
local function custom_url()
    local data = fileman.read(CONFIG_PATH)
    if not data then return nil end
    local ok, cfg = pcall(toml.parse, data)
    if ok and type(cfg) == "table" and type(cfg.source) == "table"
        and type(cfg.source.url) == "string" and cfg.source.url ~= "" then
        return (cfg.source.url:gsub("/+$", ""))
    end
    return nil
end

-- The stored choice, or nil (no file, or not a catalog id).
local function stored_catalog()
    local data = fileman.read(CATALOG_CFG)
    local id = data and data:match("catalog=(%a+)")
    return (id and M.CATALOGS[id]) and id or nil
end

local source = nil   -- { id = "stable" | "dev" | "custom", url } once resolved
local function resolve()
    if source then return source end
    local url = custom_url()
    if url then
        source = { id = "custom", url = url }
    else
        local id = stored_catalog() or M.catalog_default()
        source = { id = id, url = M.CATALOGS[id] }
    end
    return source
end

function M.reload_source()
    source = nil
end
function M.base_url()
    return resolve().url
end
-- "stable" | "dev" | "custom" (appstore.toml names the source).
function M.catalog()
    return resolve().id
end

-- Choose "stable" or "dev": stored while it differs from catalog_default(),
-- the file removed when it matches. True when the choice is on disk.
function M.set_catalog(id)
    assert(M.CATALOGS[id], "not a catalog: " .. tostring(id))
    source = nil
    if id == M.catalog_default() then
        return (not fileman.exists(CATALOG_CFG)) or fileman.remove(CATALOG_CFG)
    end
    return fileman.write(CATALOG_CFG, "catalog=" .. id .. "\n")
end

-- The cached copy of the catalog in use (one file per catalog).
function M.cache_path()
    local id = M.catalog()
    if id == "stable" then return M.STAGING.sd .. "/catalog.toml" end
    return M.STAGING.sd .. "/catalog-" .. id .. ".toml"
end

-- ── Catalog parsing ──────────────────────────────────────────────────────────

-- Safe as a single path segment: no separators, no ".." traversal.
local function safe_segment(s)
    return type(s) == "string" and s ~= ""
        and not s:find("/", 1, true)
        and not s:find("\\", 1, true)
        and not s:find("..", 1, true)
end

-- File entries may use subdirs ("assets/x.bin") but must stay inside the
-- entry's folder: relative, no "..", no leading slash.
local function safe_relpath(s)
    return type(s) == "string" and s ~= ""
        and s:sub(1, 1) ~= "/"
        and not s:find("\\", 1, true)
        and not s:find("..", 1, true)
end

-- Validate one [[apps]]/[[themes]] array; bad entries are dropped rather than
-- failing the whole catalog. Display fields are coerced to strings so UI code
-- can concatenate them blindly.
local function sanitize_list(list)
    if type(list) ~= "table" then return {} end
    local kept = {}
    for _, e in ipairs(list) do
        local good = type(e) == "table"
            and safe_segment(e.id) and safe_segment(e.name)
            and type(e.files) == "table" and #e.files > 0
            and (e.category == nil or safe_segment(e.category))
        if good then
            for _, f in ipairs(e.files) do
                if not safe_relpath(f) then good = false end
            end
        end
        if good then
            e.version = tostring(e.version or "?")
            e.author = e.author and tostring(e.author) or nil
            e.description = e.description and tostring(e.description) or nil
            e.min_fw = tonumber(e.min_fw)   -- nil = no firmware requirement
            -- Optional LoRa-protocol dependency: the app only runs under this
            -- installed protocol package (the App Library offers the protocol
            -- download first when it is missing).
            e.requires_protocol = (e.requires_protocol and safe_segment(e.requires_protocol))
                and e.requires_protocol or nil
            -- Optional legacy install key: the pre-id installed-tracking key
            -- (folder name) this entry migrates from after an id change.
            -- ONLY entries carrying `was` join old un-id'd installs by name.
            e.was = (e.was and safe_segment(e.was)) and e.was or nil
            -- Optional USB-driver dependencies: ids from the [[drivers]]
            -- list. Installed automatically after the app (App Library).
            if type(e.drivers) == "table" then
                local ds = {}
                for _, id in ipairs(e.drivers) do
                    if safe_segment(id) then ds[#ds + 1] = id end
                end
                e.drivers = (#ds > 0) and ds or nil
            else
                e.drivers = nil
            end
            -- Optional per-target file lists, validated like files; a bad
            -- list is dropped, not the entry.
            for _, key in ipairs({ "files_xtensa", "files_rv32" }) do
                local extra = e[key]
                if type(extra) == "table" then
                    local kept_files = {}
                    for _, f in ipairs(extra) do
                        if safe_relpath(f) then kept_files[#kept_files + 1] = f end
                    end
                    e[key] = (#kept_files > 0) and kept_files or nil
                else
                    e[key] = nil
                end
            end
            -- Optional device requirements: token strings. A token that is
            -- not a string is kept as text — it matches nothing, so the
            -- entry reads as not installable here rather than unrestricted.
            if type(e.requires) == "table" then
                local toks = {}
                for _, t in ipairs(e.requires) do toks[#toks + 1] = tostring(t) end
                e.requires = (#toks > 0) and toks or nil
            else
                e.requires = nil
            end
            kept[#kept + 1] = e
        end
    end
    return kept
end

function M.parse_catalog(body)
    local ok, parsed = pcall(toml.parse, body)
    if not ok or type(parsed) ~= "table"
        or (type(parsed.apps) ~= "table" and type(parsed.themes) ~= "table") then
        return nil, "Bad catalog format"
    end
    parsed.apps = sanitize_list(parsed.apps)
    parsed.themes = sanitize_list(parsed.themes)
    parsed.drivers = sanitize_list(parsed.drivers)   -- [[drivers]]: USB .drv.elf
    parsed.protocols = sanitize_list(parsed.protocols)   -- LoRa protocol packages
    return parsed
end

function M.fetch_catalog()
    local res = _wifi_fetch(M.base_url() .. "/catalog.toml")
    if not (res and res.success) then
        return nil, (res and res.error) or "Fetch failed"
    end
    if res.status ~= 200 then
        return nil, "HTTP " .. tostring(res.status)
    end
    local cat, err = M.parse_catalog(res.body or "")
    if not cat then return nil, err end
    -- Cache the raw body for offline browsing (SD only; skip silently without one).
    if fileman.mkdir(M.STAGING.sd) then
        fileman.write(M.cache_path(), res.body)
    end
    return cat
end

function M.load_cached_catalog()
    local data = fileman.read(M.cache_path())
    if not data then return nil end
    return M.parse_catalog(data)
end

-- ── Firmware gating ──────────────────────────────────────────────────────────
-- Catalog entries may carry min_fw (integer): the minimum firmware API level
-- their files need. The firmware registers _FW_API at Lua boot (version.h);
-- firmware too old to register it reads as 0, so every gated entry blocks
-- there — the safe default. NOTE: this lib ships with FIRMWARE (data/lua/lib),
-- not through the store, so the App Library carries its own fallback copy of
-- this check — it must gate correctly even where this lib predates min_fw.

function M.fw_api()
    return tonumber(_FW_API) or 0
end

-- nil when the entry is installable on this firmware, else the API level it
-- requires (for the caller's messaging).
function M.fw_required(entry)
    local need = tonumber(entry and entry.min_fw)
    if need and need > M.fw_api() then return need end
    return nil
end

-- ── Device requirements ──────────────────────────────────────────────────────
-- Catalog entries may carry `requires`: tokens that must ALL hold on this
-- device (a token may list alternatives with "|", e.g. "keyboard|touch"), and
-- per-target file lists `files_<target>`. Targets are the build targets of
-- ELF modules: "xtensa" (the ESP32-S3 boards) and "rv32" (the T-Display P4).
-- Facts come from _device_caps() and _input_caps(); firmware too old to
-- report `arch` is an Xtensa build (every released firmware is one), and any
-- other missing fact is false. A token this code does not know is NOT met:
-- nothing installs that cannot be checked. The App Library carries its own
-- copy of this check (same reason as fw_required above).
--
--   xtensa | rv32            the module build target     _device_caps().arch
--   esp32s3 | esp32p4        the SoC                     _device_caps().chip
--   board:<name>             exact board                 _device_caps().name
--   keyboard | touch | trackball                         _input_caps()
--   sound (i2s or buzzer) | i2s                          _device_caps().audio
--   battery | mic | gps | usb_host                       _device_caps() booleans
--   psram>=N                 at least N MB PSRAM         _device_caps().psram_mb

-- The build target of this device's ELF modules.
function M.target()
    local ok, caps = pcall(_device_caps)
    local arch = ok and type(caps) == "table" and caps.arch or nil
    return (arch == "rv32") and "rv32" or "xtensa"
end

local BOARD_LABEL = { tdeck = "T-Deck", heltec_v4 = "Heltec V4",
                      wio_l2 = "Wio L2", tdisplay_p4 = "T-Display P4" }

local function device_facts()
    local okc, caps = pcall(_device_caps)
    if not okc or type(caps) ~= "table" then caps = {} end
    local oki, inp = pcall(_input_caps)
    if not oki or type(inp) ~= "table" then inp = {} end
    local target = M.target()
    local f = {
        [target]  = true,
        keyboard  = inp.keyboard == true,
        touch     = inp.touch == true,
        trackball = inp.trackball == true,
        sound     = caps.audio == "i2s" or caps.audio == "buzzer",
        i2s       = caps.audio == "i2s",
        battery   = caps.battery == true,
        mic       = caps.mic ~= nil,
        gps       = caps.gps == true,
        usb_host  = caps.usb_host == true,
    }
    local chip = caps.chip or ((target == "xtensa") and "esp32s3") or nil
    if chip then f[chip] = true end
    if type(caps.name) == "string" then f["board:" .. caps.name] = true end
    return f, caps
end

local function token_holds(tok, f, caps)
    local n = tok:match("^psram>=(%d+)$")
    if n then
        return type(caps.psram_mb) == "number" and caps.psram_mb >= tonumber(n)
    end
    return f[tok] == true
end

-- The reason one token is not met, for the detail panels.
local function token_reason(tok)
    local b = tok:match("^board:(.+)$")
    if b then return (BOARD_LABEL[b] or b) .. " only" end
    local n = tok:match("^psram>=(%d+)$")
    if n then return "needs " .. n .. " MB PSRAM" end
    local R = {
        xtensa = "ESP32-S3 devices only", rv32 = "T-Display P4 only",
        esp32s3 = "ESP32-S3 devices only", esp32p4 = "T-Display P4 only",
        keyboard = "needs a keyboard", touch = "needs a touch screen",
        trackball = "needs a trackball", sound = "needs audio out",
        i2s = "needs a speaker", battery = "needs a battery",
        mic = "needs a microphone", gps = "needs GPS", usb_host = "needs USB host",
    }
    return R[tok] or "needs a newer App Library"
end

-- nil when every `requires` token holds on this device, else the reason the
-- first failing token gives (its alternatives joined with " or ").
function M.requires_unmet(entry)
    local req = entry and entry.requires
    if type(req) ~= "table" or #req == 0 then return nil end
    local f, caps = device_facts()
    for _, tok in ipairs(req) do
        local held, reasons = false, {}
        for alt in tostring(tok):gmatch("[^|]+") do
            if token_holds(alt, f, caps) then held = true break end
            reasons[#reasons + 1] = token_reason(alt)
        end
        if not held then return table.concat(reasons, " or ") end
    end
    return nil
end

-- Whether a file runs on a build target: ELF modules exist per architecture
-- (the data_filter.py rules — "*.rv32.elf" is RISC-V; "*.app.elf",
-- "*.loraproto.elf", "*.bleproto.elf" and "*.drv.elf" are Xtensa).
local XTENSA_SUFFIXES = { ".app.elf", ".loraproto.elf", ".bleproto.elf", ".drv.elf" }

local function runs_on(target, fname)
    local n = fname:lower()
    if n:sub(-9) == ".rv32.elf" then return target == "rv32" end
    for _, suf in ipairs(XTENSA_SUFFIXES) do
        if n:sub(-#suf) == suf then return target == "xtensa" end
    end
    return true
end

-- The files this device downloads for an entry: `files` plus
-- `files_<target>`, minus the files its target cannot run.
function M.device_files(entry)
    local target = M.target()
    local out, seen = {}, {}
    local function add(list)
        for _, f in ipairs(list or {}) do
            if not seen[f] and runs_on(target, f) then
                seen[f] = true
                out[#out + 1] = f
            end
        end
    end
    add(entry.files)
    add(entry["files_" .. target])
    return out
end

-- Ordered version compare: true only when the catalog version is strictly
-- newer than the installed one — plain inequality offered DOWNGRADES whenever
-- a device was ahead of the catalog (freshly flashed firmware, repo not
-- pushed yet). Versions split into numeric segments ("1.0.10" -> 1,0,10;
-- missing segments = 0); if either side has no digits at all, fall back to
-- inequality so exotic version strings keep updating. Rollback convention:
-- republish old content under a HIGHER version. Used by Settings/Theme (in
-- sync with this lib — both ship with firmware); the App Library carries its
-- own copy, same reason as the fw gate above.
function M.version_newer(cat_v, inst_v)
    cat_v, inst_v = tostring(cat_v or ""), tostring(inst_v or "")
    local a, b = {}, {}
    for n in cat_v:gmatch("%d+") do a[#a + 1] = tonumber(n) end
    for n in inst_v:gmatch("%d+") do b[#b + 1] = tonumber(n) end
    if #a == 0 or #b == 0 then return cat_v ~= inst_v end
    for i = 1, math.max(#a, #b) do
        local x, y = a[i] or 0, b[i] or 0
        if x ~= y then return x > y end
    end
    return false
end

-- ── WiFi ─────────────────────────────────────────────────────────────────────

-- Wait for WiFi (kicking auto-connect with saved creds), then cb(connected).
-- UI-free: the caller shows its own "Connecting..." view around this.
function M.wifi_wait(wait_ms, cb)
    if _wifi_status() == "connected" then
        cb(true)
        return
    end
    pcall(_wifi_auto_connect)
    local waited = 0
    apps.add_timer { period = 500, cb = function(t)
        if _wifi_status() == "connected" then
            t:delete()
            cb(true)
            return
        end
        waited = waited + 500
        if waited >= wait_ms then
            t:delete()
            cb(false)
        end
    end }
end

-- ── .version bookkeeping ─────────────────────────────────────────────────────
-- Store-managed dirs are exactly those with a .version file (3 lines:
-- version / location / category). Built-in firmware content never has one.

-- .version is positional: line 1 version, 2 location, 3 category (may be
-- empty for a top-level app), 4 "locked" when the app must not be removable
-- (e.g. the App Library itself). Split must PRESERVE empty interior lines so
-- a locked top-level app (empty line 3) still reads its lock on line 4.
-- An "id=<catalog id>" line may follow (prefix-tagged, position-independent):
-- the device-side registry identity, letting `name` (= folder + display)
-- repeat across categories. Absent on old markers -> callers key by folder
-- name. Old readers ignore it (their line-4 check is the exact "locked").
-- A "target=<build target>" line (same rule) names the ELF build the install
-- carries; absent on markers written before it. A "catalog=<id>" line (same
-- rule) names the catalog the install came from ("stable" | "dev" |
-- "custom"); absent on markers written before it.
function M.read_version(dir)
    local data = fileman.read(dir .. "/.version")
    if not data then return nil end
    data = data:gsub("\r", "")
    local lines, start = {}, 1
    while true do
        local nl = data:find("\n", start, true)
        if nl then
            lines[#lines + 1] = data:sub(start, nl - 1)
            start = nl + 1
        else
            lines[#lines + 1] = data:sub(start)
            break
        end
    end
    local id, target, catalog = nil, nil, nil
    for _, ln in ipairs(lines) do
        if ln:sub(1, 3) == "id=" and #ln > 3 then id = ln:sub(4) end
        if ln:sub(1, 7) == "target=" and #ln > 7 then target = ln:sub(8) end
        if ln:sub(1, 8) == "catalog=" and #ln > 8 then catalog = ln:sub(9) end
    end
    return {
        version  = (lines[1] ~= "" and lines[1]) or "?",
        location = (lines[2] ~= "" and lines[2]) or "?",
        category = (lines[3] ~= "" and lines[3]) or nil,
        locked   = lines[4] == "locked",
        id       = id,
        target   = target,
        catalog  = catalog,
    }
end

-- ── Staging cleanup ──────────────────────────────────────────────────────────
-- Silently delete a tree in the background (no modal) — leftover staging dirs
-- from interrupted installs and post-cancel cleanup. `cleaning` marks paths a
-- background remover is still walking, so run_install won't start writing into
-- a staging dir that's being deleted out from under it.
local cleaning = {}

local function silent_remove(path)
    path = fileman.normalize(path)
    if cleaning[path] or not fileman.exists(path) then return end
    cleaning[path] = true
    local task = fileman.task_remove(path)
    apps.add_timer { period = 20, cb = function(t)
        for _ = 1, 4 do
            local done = task.step()
            if done then
                cleaning[path] = nil
                t:delete()
                return
            end
        end
    end }
end

function M.cleanup_staging()
    for _, base in pairs(M.STAGING) do
        local entries = fileman.list(base, { sizes = false })
        if entries then
            for _, e in ipairs(entries) do
                if e.type == "dir" then
                    silent_remove(base .. "/" .. e.name)
                end
            end
        end
    end
end

-- ── Install / update runner ──────────────────────────────────────────────────

-- Download + install a catalog entry. opts:
--   entry     catalog entry ({id, name, version, files, category?, ...})
--   kind      "apps" | "themes" | "drivers" | "protocols" — the repo subdir
--             the files download from
--   loc       "sd" | "internal" — which drive (staging + .version location)
--   final_dir full drive-prefixed destination dir
--   old_dir   set for an update. old_dir == final_dir: staging completes,
--             then each staged file is copied OVER the live dir (manifest
--             files only — user data living inside the app dir, e.g.
--             saves/configs, survives). old_dir ~= final_dir (the entry's
--             category moved): a fresh install at final_dir, then old_dir
--             is removed — in-dir user data does NOT survive a migration.
--   on_done   fn(err) — err nil on success, "cancelled" on user cancel
--             (failed staging is cleaned up silently either way)
--
-- Builds a progress modal over `root`. Runs as a phase machine on a timer:
-- each tick does one bounded unit (one file download, one task_remove step,
-- one file applied). _wifi_download_file is synchronous, so the UI freezes
-- for one file's duration — the label updates between files (two ticks per
-- file: label renders only after the callback returns, so label and download
-- alternate). Cancel works during download; once the update starts applying,
-- it runs to completion (a half-applied app is worse than a short wait).
function M.run_install(root, opts)
    local entry, kind, loc = opts.entry, opts.kind, opts.loc
    local final_dir, old_dir, on_done = opts.final_dir, opts.old_dir, opts.on_done
    -- Firmware gate, last ditch: UIs check first and never offer the action,
    -- but no caller may install an entry this firmware can't run.
    local need = M.fw_required(entry)
    if need then
        on_done("Needs firmware update (app needs API " .. need
            .. ", device has " .. M.fw_api() .. ")")
        return
    end
    -- Device gate, same standing: the UIs leave such entries out of their
    -- lists; nothing may install what this device cannot run.
    local why = M.requires_unmet(entry)
    if why then
        on_done("Not for this device: " .. why)
        return
    end
    -- This device's download list (files + files_<target>, minus the other
    -- architecture's modules).
    local files = M.device_files(entry)
    if #files == 0 then
        on_done("Nothing in this entry runs on this device")
        return
    end
    -- Drive gate: the staging dir below needs free blocks; an update stages
    -- on the live drive too, so this applies to both.
    local total, used = fileman.df(loc == "sd" and "S" or "L")
    if total and total - used < M.LOW_SPACE then
        on_done(M.low_space_msg(loc))
        return
    end
    -- Kind-prefixed staging name so ids can never collide across kinds.
    local prefix = (kind == "themes" and "th_")
                or (kind == "drivers" and "dr_")
                or (kind == "protocols" and "pr_")
                or "app_"
    local staging = fileman.normalize(M.STAGING[loc] .. "/" .. prefix .. entry.id)

    local W, H = lvgl.HOR_RES(), lvgl.VER_RES()
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0,
        radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local box = overlay:Object {
        w = math.min(250, lvgl.HOR_RES() - 20), h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    box:Label { text = (old_dir and "Updating " or "Installing ") .. entry.name,
                w = lvgl.PCT(100), h = 18 }
    local cur_lbl = box:Label { text = "...", w = lvgl.PCT(100), h = 18 }
    local cnt_lbl = box:Label { text = "", w = lvgl.PCT(100), h = 16 }
    local cancelled = false
    local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
    cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel_btn:onevent(lvgl.EVENT.RELEASED, function()
        cancelled = true
    end)

    local function close(err)
        nav.pop()
        overlay:delete()
        pcall(_wifi_download_end)
        if err then silent_remove(staging) end
        -- The two out-of-space errors _wifi_download_file reports get the
        -- remedy appended; every caller shows err verbatim.
        if err and (err:find("free space", 1, true) or err:find("Disk full", 1, true)) then
            err = err .. ". Free up space on "
                .. (loc == "sd" and "the SD card" or "internal storage") .. " and try again."
        end
        on_done(err)
    end

    -- Phases: clear (old staging) -> download -> then either
    --   fresh install: finish (atomic same-drive rename), or
    --   update:        apply (copy staged files over the live dir) -> cleanup
    local phase = "clear"
    local clear_checked = false
    local clear_task = nil
    local cleanup_task = nil
    local file_idx = 0
    local labeled = false   -- label tick / download tick alternation
    local apply_list = nil  -- files + .version, built when an update applies
    local apply_idx = 0

    apps.add_timer { period = 30, cb = function(t)
        -- Cancel is only honored before anything touches the live dir.
        if cancelled and (phase == "clear" or phase == "download") then
            t:delete()
            close("cancelled")
            return
        end

        if phase == "clear" then
            -- A background cleanup (startup sweep / earlier cancel) may still
            -- be walking this exact dir — wait it out, don't race it.
            if cleaning[staging] then return end
            if not clear_checked then
                clear_checked = true
                if fileman.exists(staging) then
                    clear_task = fileman.task_remove(staging)
                end
            end
            if clear_task then
                local done, err = clear_task.step()
                if not done then return end
                if err then
                    t:delete()
                    close("Staging cleanup failed: " .. tostring(err))
                    return
                end
                clear_task = nil
            end
            if not fileman.mkdir(staging) then
                t:delete()
                close("Cannot create staging dir")
                return
            end
            phase = "download"
            return
        end

        if phase == "download" then
            if not labeled then
                file_idx = file_idx + 1
                local fname = files[file_idx]
                if not fname then
                    -- All files down. Record the install, then swap into place.
                    -- Line 4 "locked" is carried from the catalog so an update
                    -- can't silently make a locked app removable; target=
                    -- names the build the files are for (the App Library's
                    -- "adds this device's build" check); catalog= the
                    -- catalog the files came from.
                    if not fileman.write(staging .. "/.version",
                        tostring(entry.version or "?") .. "\n" .. loc .. "\n"
                        .. tostring(entry.category or "")
                        .. (entry.locked and "\nlocked" or "")
                        .. "\nid=" .. tostring(entry.id)
                        .. "\ntarget=" .. M.target()
                        .. "\ncatalog=" .. M.catalog()) then
                        t:delete()
                        close("Cannot write .version")
                        return
                    end
                    if old_dir and old_dir == final_dir then
                        phase = "apply"
                        apply_list = {}
                        for i = 1, #files do apply_list[i] = files[i] end
                        apply_list[#apply_list + 1] = ".version"   -- committed last
                    else
                        -- Fresh install, or a category MIGRATION (old_dir set
                        -- but the entry's home moved): rename to the new home,
                        -- then the finish branch removes the old one.
                        phase = "finish"
                    end
                    return
                end
                cur_lbl.text = fname
                cnt_lbl.text = file_idx .. " / " .. #files
                labeled = true
                return
            end
            labeled = false
            local fname = files[file_idx]
            local dst = staging .. "/" .. fname
            local parent = fileman.parent(dst)
            if parent then fileman.mkdir(parent) end
            local res = _wifi_download_file(
                M.base_url() .. "/" .. kind .. "/" .. entry.id .. "/" .. fname, dst)
            if not (res and res.success) then
                t:delete()
                close("Download failed (" .. fname .. "): "
                    .. tostring(res and res.error or "?"))
                return
            end
            return
        end

        if phase == "apply" then
            -- Update: copy one staged file per tick over the live dir. Only
            -- manifest files are touched, so saves/configs the app keeps in
            -- its own dir survive; files dropped between versions linger
            -- (harmless). .version goes last — the version bump is only
            -- recorded once every file made it.
            apply_idx = apply_idx + 1
            local fname = apply_list[apply_idx]
            if not fname then
                phase = "cleanup"
                cleanup_task = fileman.task_remove(staging)
                return
            end
            cur_lbl.text = fname
            cnt_lbl.text = "applying " .. apply_idx .. " / " .. #apply_list
            local ok, err = fileman.copy_file(staging .. "/" .. fname,
                                              final_dir .. "/" .. fname)
            if not ok then
                t:delete()
                close("Update apply failed (" .. fname .. "): " .. tostring(err))
                return
            end
            return
        end

        if phase == "cleanup" then
            local done = cleanup_task.step()
            if not done then return end
            -- Removes staging after an in-place update, or the OLD location
            -- after a category migration. Leftovers are non-fatal either way
            -- (staging: the startup sweep; an old dir: retried next update).
            t:delete()
            close(nil)
            return
        end

        -- finish: destination parent, then the atomic same-drive rename.
        local parent = fileman.parent(final_dir)
        if parent then fileman.mkdir(parent) end
        local ok, err = fileman.rename(staging, final_dir)
        if not ok then
            t:delete()
            close("Install failed: " .. tostring(err))
            return
        end
        if old_dir and old_dir ~= final_dir then
            -- Category migration: the update landed at its new home; remove
            -- the old location like an uninstall.
            phase = "cleanup"
            cleanup_task = fileman.task_remove(old_dir)
            return
        end
        t:delete()
        close(nil)
    end }
end

-- ── Uninstall ────────────────────────────────────────────────────────────────

-- Recursively remove an installed dir behind a progress modal. opts:
--   on_done     fn(err)
--   parent_base when set: after a successful remove, the now-empty parent dir
--               is removed too UNLESS it is parent_base itself. App installs
--               use this to drop an emptied category folder (an empty SD
--               category with no internal namesake shows as a blank launcher
--               page); themes pass nil — their parent IS the themes base.
function M.run_remove(root, name, dir, opts)
    local on_done = opts.on_done
    local task = fileman.task_remove(dir)

    local W, H = lvgl.HOR_RES(), lvgl.VER_RES()
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0,
        radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local box = overlay:Object {
        w = math.min(240, lvgl.HOR_RES() - 20), h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    box:Label { text = "Removing " .. name, w = lvgl.PCT(100), h = 18 }
    local cur_lbl = box:Label { text = "...", w = lvgl.PCT(100), h = 18 }
    local cnt_lbl = box:Label { text = "", w = lvgl.PCT(100), h = 16 }

    apps.add_timer { period = 15, cb = function(t)
        local done, err
        for _ = 1, 3 do
            done, err = task.step()
            if done then break end
        end
        if not done then
            cur_lbl.text = tostring(task.current or "")
            cnt_lbl.text = (task.files_done or 0) .. " / " .. (task.files_total or "?")
            return
        end
        t:delete()
        if not err and opts.parent_base then
            local parent = fileman.parent(dir)
            if parent and fileman.normalize(parent) ~= fileman.normalize(opts.parent_base) then
                local left = fileman.list(parent, { sizes = false })
                if left and #left == 0 then fileman.remove(parent) end
            end
        end
        nav.pop()
        overlay:delete()
        on_done(err)
    end }
end

return M
