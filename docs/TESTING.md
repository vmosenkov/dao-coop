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

Two instances, both with the same save loaded (copy the save into the guest's
instance profile as the first step of the join flow), transport binds on
127.0.0.1. Pass: moving the guest's controlled creature changes the host
game's copy of the same creature position within a tolerance (log both sides),
and vice versa; the log prints received-state counters at both ends.

## M3 — in-game join

Host starts hosting (options screen button or key), the guest enters the host
address. Pass: from main menus only, both end up in the same area with the
same object ids (log a hash of the party ids and area id on both sides) within
60 s, and the guest's save never overwrites an existing character folder.

## M4 — combat sync

Host stages a fight; guest attacks the same hostile. Pass: one hostile, one
death, each side logs the same health curve; loot/bodybag appears once;
the guest hears the host's damage within 500 ms.

## M5 — transitions and saves

15 consecutive area transitions host-driven, guest following. Pass after each:
party ids and area id hash equal on both sides, no guest crash. Then host
saves, guest rejoins, and the world matches.

## Automated rig (later)

`DAOEluvianMCP` (public, GPLv3) is the intended bench: it drives a live game
over HTTP/MCP with its own instance/port per copy, so the same scenarios can
run with no mouse. Our plugin can register its own test tools/events through
its C interface (`src/api/EluvianAPI.h`, MIT) once the rig is set up.
