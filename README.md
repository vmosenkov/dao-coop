# dao-coop

Co-op for **Dragon Age: Origins – Ultimate Edition** (1.05.13263.0), built as a
[DAOLib](https://github.com/Whocares66-Dev/DAOLib) native plugin. Work in
progress; see `docs/DESIGN.md` for the plan and `docs/STATUS.md` for what has
actually been run.

- The engine has no netcode and one controlled character; co-op means **two
  game processes**, a host that owns the simulation and a guest that follows
  it, joined by transferring the host's save and then syncing state.
- The plugin is 32-bit C++20, built with VS 2022 + CMake + vcpkg, and loaded
  from `bin_ship\daolib\plugins\daocoop\`.
- Milestone 0 (this repository's first rung): load in-game and log the
  controlled creature and party state once a second. That proves the loader,
  the plugin API and live engine reads before any networking exists.

## Play (release package)

`tools\package.ps1` builds the release preset and writes
`E:\dao-coop-build\dist\dao-coop-<version>.zip`, laid out as the game folder:
the DAOLib loader, the address files, this plugin, `README-INSTALL.txt` and
`daocoop.ini.example`.

For a player: extract the zip over the game folder (with the game closed), copy
`daocoop.ini.example` to `bin_ship\daolib\plugins\daocoop\daocoop.ini`, then in
that file set `role = host` (one player) or `role = guest` plus the host's
address in `peer` (the other). Host and guest need the same game build and the
same mod version. The install notes in `tools\README-INSTALL.txt` list the
firewall ports and what does and does not work yet.

## Build

```powershell
git submodule update --init --recursive
.\tools\build.ps1 -Preset debug
```

Output: `E:\dao-coop-build\debug\` (the tree's build is kept off OneDrive).

## Test install

Never deploy into your real game. The isolated copy used for testing is
`E:\DAO-Coop-Test`:

```powershell
.\tools\deploy.ps1 -Preset debug
```

Logs: `Documents\BioWare\Dragon Age\daolib\loader.log` and `daocoop.log`.

## Building the in-game screen

The event log panel in the HUD is a plugin-owned SWF 8 movie built from
ActionScript 2 (`ui/eventlog/frame_1/DoAction.as`) with FFDec, which imports
the game's own fonts. Build it before the plugin (the plugin copies the movie
beside its DLL through CMake's `FILES`):

```powershell
python tools\movie.py --ffdec C:\Modding\ffdec_26.3.0 coop_eventlog
```

`docs\UI.md` has the pipeline, the tests and the screenshots.

## The coop-anything hub

The game's write-up and directory entry live in
[cranebeaker/coop-anything](https://github.com/cranebeaker/coop-anything):
`games/dragon-age-origins/README.md` and `data/directory.yaml`. The first
contribution ([PR #81](https://github.com/cranebeaker/coop-anything/pull/81))
was merged; the current update (combat arbitration, death/revive, guest
rejoin, in-game Event Log, directory entry to `alpha`) is
[PR #141](https://github.com/cranebeaker/coop-anything/pull/141), left as a
draft for the owner's review.

## Licence

GPLv3, as DAOLib (the plugin compiles the library in).

