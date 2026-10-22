# dao-coop — instructions for agents (Codex, Claude, anyone)

This repository is the co-op mod for **Dragon Age: Origins – Ultimate Edition**
(1.05.13263.0) built on [DAOLib](https://github.com/Whocares66-Dev/DAOLib).
The repository is the source of truth: read the code before trusting any
summary, including the ones in `docs/`.

## What the project is

Two game processes, **host authoritative**: the guest joins by receiving the
host's save (`src/SaveTransfer.*`), both games then hold the same object ids,
and each game mirrors the other player's creature and the host's hostile
through the UDP state channel (`src/Transport.*`, `src/Plugin.cpp`). Roles and
options come from `daocoop.ini` beside the plugin; environment variables
(`DAOCOOP_*`, `DAOLIB_INSTANCE`) override it for automated tests.

Read first: `docs/DESIGN.md` (architecture and milestones), `docs/STATUS.md`
(what has actually been run, with numbers), `docs/TESTING.md` (how each
milestone is tested). `src/Plugin.cpp` is large but is the whole plugin today.

## Rules that matter

- **Never invent a DAO/DAOLib API.** Verify every function, field and offset
  in `extern/DAOLib/src/dao/**` or in the docs under `extern/DAOLib/dev/`.
  If the capability is not verified, report a blocker instead of guessing.
- **Never touch the owner's real game install or saves.** The test install is
  `E:\DAO-Coop-Test`; named instances write their own profiles under
  `%LOCALAPPDATA%\DAOLib\instances\<name>`.
- **Do not reimplement the game's combat.** Hit chance, damage, armour,
  resistances, attack speed, death and effects stay in DAO's own code. The
  host's job is to *trigger* the action and to replicate the resulting state.
- **Do not rewrite working parts** (the join flow, the two-avatar selection,
  the hostile health channel) without evidence that they are wrong.
- One task = one deliverable. Do not expand scope silently; if something
  needs a larger change, report it first.
- No `git push --force`, no history rewrites, no deleting other agents'
  changes, no GitHub releases, no merging PRs. Keep the working tree clean.
- Code, comments, docs: English. Style follows DAOLib: C++20, MSVC x86,
  `/W4 /WX` (a warning fails the build), no unnecessary abstractions.
- Network input is untrusted: validate sizes, ids and enums; keep the receive
  path non-blocking on the game thread.

## Build, test, run (Windows, PowerShell)

```powershell
.\tools\build.ps1 -Preset debug              # first run installs vcpkg deps
.\tools\build.ps1 -Preset release -Offline   # release, reuses debug's packages
.\tools\deploy.ps1 -Preset debug             # into E:\DAO-Coop-Test only
.\tools\join-test.ps1                        # host+guest: join by save transfer
.\tools\two-avatar-test.ps1                  # two players, two party members
.\tools\combat-test.ps1                      # hostile health/death channel
```

Tests start two real game clients, wait for their worlds, run for a fixed time
and parse the plugin logs; each prints `VERDICT: PASS|FAIL`. The logs are under
`%LOCALAPPDATA%\DAOLib\instances\<name>\BioWare\Dragon Age\daolib\`
(`loader.log`, `daocoop.log`). A test that passes 10 times in a row is still
one scenario: say exactly what was and was not observed.

## How to report work

Return a short report, not an essay:

```text
Findings:    what exists in the code (with file:line references)
Changes:     files touched and why (or "none")
Validation:  implemented / builds / automated test / runtime-tested in DAO / not verified
Risks:       lifetime, threading, desync, compatibility
Next:        the smallest logical next step
```

Distinguish **implemented** (code exists), **builds** (compiled), **automated
tested** (a script passed), **runtime tested** (seen in Dragon Age: Origins)
and **not verified**. Never write "works" without a run.
