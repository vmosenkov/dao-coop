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

## Licence

GPLv3, as DAOLib (the plugin compiles the library in).
