# Testing dao-coop

What "works" means at each milestone, and the exact commands that produce the
evidence. A human playtest is not a test plan; these steps are scripted so a
bot or an agent can run them.

## Rules

- Only `E:\DAO-Coop-Test` is ever launched or written to. The owner's real
  install and real Documents profile are read-only; the named-instance
  profiles (`%LOCALAPPDATA%\DAOLib\instances\<name>`) are ours.
- One instance = one `DAOLIB_INSTANCE` name; `host` and `guest` are the rig's
  two names. Two copies run side by side only as named instances.
- Every test records: command run, log file read, the lines that prove it, and
  the date. Claims without a run go to `DESIGN.md` as unknowns instead.

## M0 — plugin loads and reads live state

```powershell
.\tools\build.ps1 -Preset debug
.\tools\deploy.ps1 -Preset debug
.\tools\launch.ps1 -Instance host -Seconds 45
```

Read `%LOCALAPPDATA%\DAOLib\instances\host\BioWare\Dragon Age\daolib\daocoop.log`.

Pass: the log has `dao-coop loaded (plugin API 2)`, and then, once a game is
loaded (a save loaded or a new game started), `---- frame … mode … paused …` with
`party bar: N selected` and `controlled 0x…: area 0x… pos (…, …, …) health …
/ … team …`.

At the main menu only the load line is expected: no game, no objects.

## M1 — two isolated instances

```powershell
.\tools\play-test.ps1 -Seconds 60
```

Pass: two `daorigins` processes alive together; each instance's `loader.log`
says its profile was seeded; **no** line in the owner's real profile logs has
a timestamp from this run (check the real `loader.log` mtime is unchanged).

## M2 — loopback replication (puppets)

```powershell
.\tools\build.ps1 -Preset debug
.\tools\deploy.ps1 -Preset debug
.\tools\sync-test.ps1 -SyncSeconds 90
```

The script starts the host, waits for its world (the intro movie only advances
while its window is foreground, so a guest started too early can leave the
host stuck in it), starts the guest, waits for its world, runs 90 s of sync,
and reads the plugin logs. Pass: both instances report packets received, the
guest reports applied positions, its last reported distance to the host is
small, and the host reports drive orders.

**Verified 2026-10-20 (PASS):** host 34 drive orders; host 3,404 sent / 2,185
received; guest 2,169 sent / 3,346 received; 0 dropped; guest applied 830
positions with the engine's `SetPosition`; last host→guest distance 0.00 m;
both in area `0x000000ca`.

What this does **not** prove: a second avatar/player, a join handshake (the
rig starts both games on the same save), combat, or area transitions — M3+.

## M3 — in-game join

```powershell
.\tools\build.ps1 -Preset debug
.\tools\deploy.ps1 -Preset debug
.\tools\join-test.ps1
```

The host loads its save, plays for ~8 s, writes manual slot 91 and serves that
slot's folder over TCP. The guest starts at its main menu (auto-load **off**),
connects, receives the files, writes them into a manual slot of its own profile
and asks the game to load it; then the M2 sync continues. The script starts the
guest immediately after the host reaches the world (the host's transfer window
opens a few seconds later and waits for the guest), and asserts the join lines
on both sides, `game loaded: slot 91` on the guest, applied positions, a small
final distance and the same area id in both last net lines.

**Verified 2026-10-20 (PASS):** host: save requested, slot folder sent, 25 drive
orders; guest: `join: received` (4 files), `join: wrote 4 file(s)`,
`join: asked the game to load`, `game loaded: slot 91 kind manual`; 569
positions applied after the join, last distance 0.15 m, both in area
`0x000000ca`.

What this does **not** prove: two separate avatars, combat sync, transitions,
or a player-facing host/join UI (the rig drives it by environment variables).

## M4 — two separate avatars

```powershell
.\tools\build.ps1 -Preset debug
.\tools\deploy.ps1 -Preset debug
.\tools\two-avatar-test.ps1
```

The host creates a mabari next to the player and adds it to the party with the
engine's own `AddToParty` (command 396) *before* it writes the handoff save, so
the guest's world already holds the second party member with the same id. After
joining, the guest selects party index 1 with the party bar's own message
(`SelectPartyMemberIndexMessage <index> 0`, the click that replaces the
selection; `<index> 1` only adds to it and leaves control on the first entry).
Both players then drive their own creature and each game mirrors the other's.

Pass: host logs the companion and a party list of 2, the guest logs the join,
the selection and a control change, and both last net lines show different
controlled ids, each side's remote id being the other's controlled id, puppet
applies above the threshold, a small puppet distance and the same area.

**Verified 2026-10-20 (PASS):** host controlled `0x00000082` and mirrored
`0x00000363` (4,815 applies, 0.26 m); guest controlled `0x00000363` and
mirrored `0x00000082` (5,274 applies, 0.21 m); both in area `0x000000ca`.

What this does **not** prove: combat, effects, area transitions, a second
inventory/dialogue arbitration, internet transport, or a UI.

## M5 — hostile health/death channel

```powershell
.\tools\build.ps1 -Preset debug
.\tools\deploy.ps1 -Preset debug
.\tools\combat-test.ps1
```

The host spawns an ogre with `CreateObject` before the handoff save, but sets
the scripts' own AI-off switch (`SetLocalInt(creature, "CREATURE_RULES_FLAG0",
8)`) and makes it inactive, so no fight starts and the host's save is allowed
(the engine refuses saves in combat — the first version of this test sat in a
dead/paused GUI instead). After the guest joins, the host damages the ogre on a
schedule and carries its id, position and health in the state packet; the guest
sets its copy's current health with `CCCreature::SetProperty`.

Pass: host logs the spawn and ≥5 damage ticks, its health falls, the guest's
last net line names the same hostile id, its received health equals the host's
live value (±1) and it applied health changes.

**Verified 2026-10-20 (PASS):** hostile `0x00000363`; host 12 damage ticks,
`220 → 0`; guest received `hostile_health 0.0` and applied it 13 times (the
hostile reached its death state, so the channel carried death as well as
damage).

What this does **not** prove: real combat (the damage is applied by the host
plugin as a stand-in), enemy AI/animations, loot, or guest attack intents.

## Release build, config file and LAN addressing

```powershell
.\tools\build.ps1 -Preset debug            # first: installs the vcpkg packages
.\tools\build.ps1 -Preset release -Offline # release, reusing those packages
.\tools\package.ps1                        # dist\dao-coop-<version>.zip
.\tools\deploy.ps1 -Preset release         # into the isolated test copy
.\tools\join-test.ps1                      # must PASS on the release build
```

Config file: put `daocoop.ini` beside `daocoop.dll` (template:
`daocoop.ini.example`). An instance started with only `DAOLIB_INSTANCE` set and
no `DAOCOOP_*` variables must log `config: N value(s) from …`, the role, the
peer and the join/handoff switches from the file. Verified 2026-10-20 with a
host instance: 8 values read, `autoload on`, `net: role host`, `handoff on`,
then a normal load.

LAN addressing: set `DAOCOOP_PEERIP` and `DAOCOOP_HOSTIP` to the other
player's IP (or an interface address of this machine to exercise the path),
then run the join test. Verified 2026-10-20 on this machine's non-loopback
address `192.168.56.1`: both instances bound `0.0.0.0`, logged `sending to
192.168.56.1:…`, and the join + sync test passed. A second physical PC has not
been tried; the host must allow inbound UDP 45830 and TCP 45832, the guest UDP
45831.

## M7 — combat arbitration

```powershell
.\tools\attack-test.ps1
```

The guest's controlled creature attacks the host's ogre. The guest's plugin
turns its attack command into a request (attacker, target, request id) instead
of letting its local game resolve it; the host validates the request against the
live world (known guest, monotonic request id, both objects exist, target
alive, same area, hostile target), gives its own game the attack order
(`dao::orders::Attack`) and answers with the result. The host repeats the order
every ~120 frames so the swing completes.

Pass: the host logs `attack: request … accepted`, the guest logs the same
request id as `accepted`, the ogre's health falls in the host's game, and the
guest's mirrored copy follows. **Verified 2026-10-23 (PASS)** on the debug and
the release build.

## M8 — death and revival

```powershell
.\tools\death-test.ps1
```

The host player is killed on purpose (test hook `DAOCOOP_TEST_KILL_PLAYER`); the
world must keep running while a party member is alive. Three seconds after the
fall the hostile is defeated through the second test hook
(`DAOCOOP_TEST_VICTORY`), and the revive path (`RevivePartyAfterFight`) must set
the fallen party's health from zero to its maximum and return control to the
host's avatar. A corpse left in the hostile table must not count as a fight.

Pass: the log says the host player is down, state lines continue after the fall,
`revive:` lines appear, and the avatar's health is above zero again.
**Verified 2026-10-23 (PASS).**

## M9 — guest rejoin after the host loads another save

```powershell
.\tools\reload-test.ps1
```

Mid-session the host loads a second save (test hook `DAOCOOP_TEST_RELOAD`). The
host's `OnGameLoaded` (second and later loads) marks the world reloaded,
re-saves the transfer slot and serves it again; the guest sees the flag, resets
its join state, receives the new slot, loads it (loading screen) and re-selects
its party member.

Pass: host logs that it loaded and served the new world, guest logs the reload
flag, the received save and `game loaded: slot 91`, and the guest rejoins.
**Verified 2026-10-23 (PASS)** on the debug and the release build.

## M10 — in-game event log and its lifecycle

```powershell
.\tools\movie.py --ffdec C:\Modding\ffdec_26.3.0 coop_eventlog   # the SWF
.\tools\ui-shot.ps1          # in a session: the panel appears
.\tools\ui-menu-test.ps1     # at the main menu: the panel does not appear
.\tools\ui-join-shot.ps1     # two clients: a real event reaches the panel
```

The panel is a plugin-owned SWF 8 screen (ActionScript 2, built by `movie.py`
with FFDec) added with `dao::gui::AddScreen` and filled from `EventLog` through
`Movie::SetArray`/`SetVariable` when the log's revision changes. It is shown
only in explore/combat mode and hidden in the main menu, the pregame and the
loading screens.

Pass: `ui-shot` logs `ui: event log screen shown` and the panel is in the
screenshot; `ui-menu-test` logs `ui: event log screen added` but no `shown` and
the menu screenshot has no panel; `ui-join-shot` shows a real
`Another player joined the game` event in the panel. **Verified 2026-10-23
(PASS)**, including after the lifecycle fix, on the debug and the release build.

## M12 — mirrored creature walks (run animation)

```powershell
.\tools\walk-test.ps1
```

Both players walk their own creature (the plugin's drive orders), so each game
has to move the other player's creature. A gap under 3 m is walked with
`dao::orders::MoveTo` (the creature's own movement controller plays walk/run),
larger gaps and stalled walks are teleported; the net line carries
`puppet_walks`.

Pass: both sides log drive orders and at least ten puppet move orders, the
puppet distance stays under 2 m, and both are in the same area.
**Verified 2026-10-23 (PASS):** host 33 drive orders / 248 puppet walks /
0.56 m; guest 15 / 209 / 0.77 m.
## M13 — independent players and a shared event log

```powershell
.\tools\independence-test.ps1
```

The host stands still and serves the handoff save; the guest joins with no
party index configured, so the plugin picks index 1 for it, and then walks its
own creature. The test asserts that the two controlled ids differ, that the
mirror never rewrites the guest's own position (`applied` stays 0), that the
guest gets at least 1 m away from the standing host, that the host's copy
follows it, and that both logs contain both join lines (one written locally,
the other arriving in the event packet).

**Verified 2026-10-23 (PASS):** host `0x0000007a`, guest `0x0000211c`,
guest applied 0, furthest distance 2.14 m, host puppet distance 1.12 m, both
logs hold "Another player joined the game" and "Joined the host's game".
## Automated rig (later)

`DAOEluvianMCP` (public, GPLv3) is the intended bench: it drives a live game
over HTTP/MCP with its own instance/port per copy, so the same scenarios can
run with no mouse. Our plugin can register its own test tools/events through
its C interface (`src/api/EluvianAPI.h`, MIT) once the rig is set up.


