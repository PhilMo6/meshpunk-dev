--[[
  Simple Mode top bar.

  Meshcore > Simple Mode installs this file as L:/lua/lib/topbar.lua (the
  normal top bar waits as L:/lua/lib/old_topbar.lua). APP_DIR is not defined
  in this file: the Simple Mode app writes `local APP_DIR = "<its install
  folder>"` as the first line of the installed copy, the same as for the
  launcher. This file does not run from the app folder.

  It loads the normal top bar and replaces one thing: the unread badge
  counts only the picked contacts and channels (APP_DIR/simple_contacts.txt
  and simple_channels.txt). The normal top bar's updateUnread reads the total
  through messages:countUnread(), so that call is pointed at the picked sum
  for the duration of each refresh. Clock, battery, the notification bell and
  the power panel are the normal top bar's own code.

  The top bar loads once at boot and is held by several libraries, so a swap
  takes effect at the next restart.
]]

local messages = require("lib/mesh/messages")
local M = require("lib/old_topbar")

M.simple_mode = true   -- the launcher checks this to know the swap is live

local LIST_PATH = APP_DIR .. "/simple_contacts.txt"
local CH_PATH   = APP_DIR .. "/simple_channels.txt"

local function read_all(path)
    local f = io.open(path, "r")
    if not f then return "" end
    local txt = f:read("*a") or ""
    f:close()
    return txt
end

-- Unread across the picked threads only. Contact lines are
-- <pubkey>\t<name>[\t<nick>]; channel lines are <name>[\t<nick>]. Both files
-- are a few lines, read on each refresh.
local function picked_unread()
    local n = 0
    for line in read_all(LIST_PATH):gmatch("[^\r\n]+") do
        local name = line:match("^%x+\t([^\t]+)")
        if name then n = n + messages:unreadInDM(name) end
    end
    for line in read_all(CH_PATH):gmatch("[^\r\n]+") do
        local name = line:match("^([^\t]+)")
        if name then n = n + messages:unreadInChannel(name) end
    end
    return n
end

local normal_update = M.updateUnread
function M.updateUnread()
    local real = messages.countUnread
    messages.countUnread = function() return picked_unread() end
    local ok, err = pcall(normal_update)
    messages.countUnread = real
    if not ok then error(err, 0) end
end

return M
