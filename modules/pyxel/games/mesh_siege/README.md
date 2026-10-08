# Mesh Siege

A LoRa mesh tower defense for the Meshpunk Pyxel player (160x120, 30 fps).

You run a mesh on a 32x32-tile map that scrolls under a 20x13-tile view.
Noise flies in from every side, straight through terrain, and goes for your
nodes. A node only works while it is linked: inside the radio circle of an
online node, with a clear line of sight, back to the gateway, within 6 hops.
Hills, mountains and rooftops link and hit farther; valleys and forests hide
links. Homes the mesh reaches are users, and serving them earns cred.

- **Money and cred.** Money comes from a steady baseline income and from
  donations. It pays for nodes, node levels, batteries, panels, repairs and
  the money half of the specials. Cred comes from homes served, the mesh's
  size, kills, cleared waves, early calls and bad actors taken down. Every
  2 s the gateway is down costs cred, and so does each nullified node. HQ
  levels cost cred: crews, trucks, tools, armor, and the donations' amount and
  frequency.
- **Specials** cost money and cred and unlock during a game. Node specials
  are fitted by a crew from a node's SPEC menu: SIGNED FW (cannot be
  nullified), TALL MAST (higher antenna, more radio), HARDENED (cannot be
  jammed) and ARMORED CASE (more HP). They unlock by taking down a bad actor,
  building on high ground, silencing jammers and repairing downed nodes. HQ
  specials are one-off buys in the HQ list: SPARE PARTS (repairs cost half
  and take half the time), NET MONITOR (crews go to down and nullified nodes
  unasked), DIRECTION FINDING (two nodes pin a bad actor) and BACKUP UPLINK
  (the gateway may stay down twice as long). They unlock by HQ rank (levels
  bought) and the most homes served at once.
- **Crews.** Every build, upgrade, repair, reset and take-down is a job for
  an install crew that drives out from the gateway on the trucks' route map
  (roads and bridges are quick). Far and high sites take longer, and no work
  goes on while noise is on top of a site.
- **The gateway.** If it stays down for 20 s the game is lost. A crew is sent
  to it at once, dropping whatever it was doing if none is free. Bricks and
  flares camp on a downed gateway, which keeps the crew off it.
- **Bad actors.** Rogue nodes, installed as the mesh grows, next to the
  branches they can hurt most and out of the mesh's radio. Their signals
  nullify a node, and with it everything linked through it, until a crew
  resets it. A nullified node draws a purple search zone; each online node
  whose radio reaches the actor narrows it to a ring, and the third pins the
  actor. A crew can then take it down for cred.
- **Power.** Grid nodes only go within 2 tiles of a building, a home, the
  gateway or a substation. Solar nodes go anywhere and run on a battery that
  charges in the sun and only drains under a cloud; empty, the node is dark
  until it recharges. A solar node's menu buys a bigger battery or panel.

Survive 20 waves to win (then overtime).

## Files

| File | What |
|---|---|
| `main.py` | App, title (a demo starts after 15 s idle), map select (EASY/NORMAL/HARD and medals), how-to-play pages, save file |
| `game.py` | One game: view, mesh links, power and clouds, bad actors, money and cred, specials, crews and jobs, waves, noise, combat, input, HUD, panels, overview |
| `pilot.py` | Autopilot that plays the title demo |
| `level.py` | Map parsing, the trucks' route map, line of sight, where grid power reaches; no pyxel calls, so plain Python can check maps |
| `data.py` | Terrain, maps, node and noise stats, power, money and cred, bad actors, HQ upgrades, specials, waves, modes, palette |
| `gfx.py` | Sprites and logo (image bank 0), map art (bank 1); the game draws search zones into bank 2 |
| `audio.py` | Sound effects, the wave and flare stings, the in-game ambient piece and the title music; music on/off (saved by `main.py`) |
| `ctl.py` | Controls: every action answers to a gamepad button and to keys |
| `bot.py` | Test bot, not packaged (see below) |

## How things move

Noise flies straight at its target. What it aims at depends on its kind
(`NoiseKind.aim` in `data.py`): the nearest node, the gateway, the node
carrying the most of the mesh, node after node, or nothing (storms ride
across). Every noise has an end: kamikaze hits, a brick's `SMASH_TIME`, a
flare's `FLARE_TIME`, a jammer's `JAM_PARK`, `LOST_TIME` without a target, so
every wave finishes.

Trucks follow one flow field toward the gateway, built when the map loads
(`level.flow_field`, a reverse Dijkstra over 8 neighbours that never cuts the
corner of a tile it cannot enter). `Level.path(a, b)` drives up a's chain to
where it meets b's chain and down to b, so no trip needs a search of its own.

## Links

Node A covers node B when B is inside A's radio and `Level.los()` finds no
tile along the line between them whose `block` stands above it. Antenna height
is the terrain's `elev` plus the node kind's mast (`MASTS`). Links form a
breadth-first tree from the gateway; jammed, down, nullified and flat nodes
drop out of it, and so does everything linked only through them. The gateway
cannot be jammed.

## Maps

`MAPS` in `data.py` holds each map as 32 strings of 32 tile codes:
`.` field, `,` valley, `^` hill, `M` mountain, `~` water, `t` forest, `=` road,
`b` bridge, `#` rooftop, `G` gateway (exactly one), `h` homes, `S` substation.
Trucks cannot cross water except on bridges, and only tiles trucks can reach
are buildable.

## Build the .pyxapp

    python ../pack.py mesh_siege

`pack.py` leaves out `bot.py` and every file or folder starting with `_`.

## Tuning

All numbers live in `data.py`: `TERRAIN`, `MAPS` (starting money,
`hp_scale`, clouds, the node counts that install bad actors), `NODE_KINDS`,
`NOISE_KINDS` (`reward` is the cred for a kill), power (`BAT_CAP`,
`PANEL_RATE`, ...), money and cred (`INCOME`, `DONATE_AMOUNT`,
`DONATE_EVERY`, `CRED_EVERY`, `CRED_NODES`, the losses, `wave_cred`,
`bad_cred`), crews and jobs, bad actors (`BAD_RANGE`, `BAD_SEND`,
`ACTOR_CAP`, ...), `HQ` (cred costs), `NODE_SPECS` and `HQ_SPECS` (prices and
what unlocks them), `WAVES`, `wave_hp_mult` and `MODES`.

## Testing

A file `_bot.txt` next to `main.py` holding `MAP STEPS MODE` (for example
`2 8 1`) skips the title and lets `bot.py` play the autopilot from `pilot.py`.
`STEPS` runs that many game steps per frame; `MODE` is 0 EASY, 1 NORMAL,
2 HARD. It prints one line per wave (gateway HP, money, cred, nodes, down,
nullified and flat nodes, users, crews, HQ levels, specials, bad actors taken
down, live heap) and a line per bad actor (zone size, hits, nodes that heard
it), then a final `[bot] END ...` line and the node count per kind, and quits.
In `spec=`, each node special shows its letter (S, M, H, C) and the nodes
fitted with it once unlocked, or `.` while locked; after the `/`, each HQ
special (P, N, D, U) shows its letter when owned, lower case when unlocked,
or `.`. `[bot] BAD ...` lines report broken bookkeeping: negative cred, a crew
and its job out of step, a job neither queued nor held, a downed gateway
without a repair job, a noise without a target for 30 s, or a wave running
over 5 minutes (with every live noise listed).

## License

MIT, Meshpunk.
