return { body = [[
Temporary test tool for the T-Display P4 bring-up - not for release.

The device connects out to a PC on the same WiFi network (address and port are set at the top of main.lua) and lets the PC send and watch mesh traffic: it streams every captured frame (direction, length, RSSI, SNR, hash, raw bytes) and answers commands - send a DM or channel message, or run a line of Lua.

Background keeps the link alive while you use other apps. Stop link + Home closes it. The link must be opened again after every reboot; running a game (an ELF app) ends it too.]] }
