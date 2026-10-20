# dao-coop — status

Last updated: 2026-10-20. Everything here was run or read on the owner's PC;
**[observed]** = a command's output or a file read, **[derived]** = our
inference, **[unknown]** = not established. This file is the honest log; the
plan is `DESIGN.md`.

## Game identity (coop-anything AGENTS.md rule 5)

| | |
|---|---|
| Title | Dragon Age: Origins – Ultimate Edition |
| Store | Steam, app id **47810** (depot 47811; `sku.ini` says `SKU=Steam`), Steam build id **21245176** |
| Installed at | `<game dir>` = `%USERPROFILE%`-independent path on E: (owner's choice) |
| Main exe | `bin_ship\daorigins.exe`, FileVersion **1.05.13263.0**, 9,674,752 bytes, SHA-256 `A79621C2D2020C6F838096150E30F8E51FA263DBC9121E04E3D8C633975892F8` |
| Exe provenance | **[observed]** the exe in place has **no `.bind` section** (no SteamStub) and link stamp **0x4eb19d53** = the **GOG build** of 1.05.13263.0; the stock Steam exe sits beside it as `DAOrigins.exe.backup` (10,594,144 bytes, `.bind` section, entry 0x009ED2EE). So the owner (or an earlier setup) replaced Steam's wrapped exe with GOG's to run DAOLib. |
| Loader installed | **DAOLib 0.5.0** (`bin_ship\dinput8.dll` + `dinput8.pdb`, GPLv3, github.com/Whocares66-Dev/DAOLib) + DAOAddressLibrary 0.5.0 (`bin_ship\daolib\addresses\*.bin`) |
| Engine | **[observed]** Eclipse (BioWare, Aurora/NWN lineage), 32-bit, RTTI intact, no netcode |
| OS | Windows 11 Home, 10.0.26300, 64-bit |

### Evidence that the loader works on this machine

`Documents\BioWare\Dragon Age\daolib\loader.log`, session of 2026-10-20
19:33–19:38 (owner's real install, untouched by us):

- `address library: 103293 IDs for this build, from ...\daoaddresses-4eb19d53-0096a000-004e3024-00400000.bin`
- `entry point detoured; plugins load when the game starts`
- engine heap, all processors, fast load enabled; `no plugin folder at ...\bin_ship\daolib\plugins`
- the game reached 19:38 and exited cleanly (`ExitProcess(0)`).

**[derived]** the chain loader → library → address IDs is verified on this
exact install. What is missing is a plugin folder and a plugin.

## Isolated test install (never touch the owner's install/saves)

| | |
|---|---|
| Path | `E:\DAO-Coop-Test` (full copy; the owner approved this) |
| Created | 2026-10-20 by `robocopy /E /COPY:DAT /DCOPY:DAT /MT:8`; exit 1 = files copied (success) |
| Size | 27,933,375,816 bytes (26.0 GB), 22,658 files; `bin_ship\daorigins.exe` present |
| Second instances | **DAOLib named instances**: start the game with `DAOLIB_INSTANCE=<name>`; the loader then uses its own mutex and a separate Documents profile at `%LOCALAPPDATA%\DAOLib\instances\<name>` (settings, add-ins, packages, saves, screenshots, logs; seeded once from the default profile's saves). This is how two copies run side by side **without touching the owner's profile**. **[observed]** in DAOLib `src/loader/Instance.cpp`. |
| Test profile | **[observed] 2026-10-20:** started `E:\DAO-Coop-Test\bin_ship\daorigins.exe` twice, with `DAOLIB_INSTANCE=host` then `=guest`; both processes ran side by side for 40 s each; each log says `instance "…": its profile was given the default profile's Settings, addins, Offers, packages, Characters, in 47 ms`; instance profiles exist under `%LOCALAPPDATA%\DAOLib\instances\{host,guest}` with `seeded` marker. The owner's real profile was only read (the seeding copies it). A single-instance run reached the main menu (window title `Dragon Age`). |

## Toolchain

| Piece | Status |
|---|---|
| Python 3.14.4 | **[observed]** present |
| Git 2.45.2 | **[observed]** present; `gh` CLI logged in (owner approved use) |
| vcpkg | **[observed]** cloned + bootstrapped at `E:\vcpkg` (2026-09-26 tool, `vcpkg.exe version` OK) |
| Visual Studio Build Tools 2022 | **[observed 2026-10-20]** installed with the C++ workload (elevated run, owner accepted the UAC prompt) at `E:\VS2022BuildTools`: `VC\Tools\MSVC\14.44.35207\bin\Hostx64\x86\cl.exe`, `VC\Auxiliary\Build\vcvarsall.bat`, CMake and Ninja under `Common7\IDE\CommonExtensions\Microsoft\CMake\`. The old system-wide Build Tools 2022 (E:) has no C++ workload and is unused. |
| Windows SDK | shipped with the new VS install (the pre-existing `C:\Program Files (x86)\Windows Kits\10` had no version directory) |

## Build and milestone log (verified 2026-10-20)

The whole chain was built and then **run in the game** the same evening.

| Step | Evidence |
|---|---|
| Build | `tools\build.ps1 -Preset debug` → `E:\dao-coop-build\debug\dinput8.dll` (3,742,208 bytes) and `daolib\plugins\daocoop\daocoop.dll` (2,191,872 bytes). vcpkg installed detours/spdlog/directxtex for `x86-windows-static-md`; DAOLib core, daolib, the address library and the loader all compiled with MSVC 14.44.35207. |
| Deploy | `tools\deploy.ps1 -Preset debug` copied loader, address files and plugin into `E:\DAO-Coop-Test` (test copy only). |
| M0 — plugin loads, reads live state | One host instance with `DAOCOOP_AUTOLOAD=1`: `loader.log` says `daocoop.dll: loaded` / `1 plugin(s)`; `daocoop.log` says `dao-coop loaded (plugin API 2), instance "host"`, `autoload on`, `autoload: asking to load the newest save…`, `game loaded: slot 1 kind auto`, then `mode explore` with `party bar: 1 selected` and `controlled 0x00000082: area 0x000000ca pos (-37.80, -9.54, 0.02) health 140.0 / 140.0`. |
| M1 — two isolated instances, both in the world | Two `daorigins` processes ran side by side (941 MB and 940 MB working set), each as its own named instance, both loading the same seeded save: host `game loaded: slot 1 kind auto` at 21:11:29, guest at 21:12:04; both reached `mode explore`; both reported controlled **0x00000082 in area 0x000000ca** with health 140/140. |
| ID alignment (M3 precondition) | After both loaded the same save, the two independent processes agreed on the controlled creature's object id and the area id. Their positions diverged slightly (−37.86/−10.00 vs −37.80/−9.54) because each simulates on its own — the exact divergence a sync layer has to remove. |
| M2 — loopback state sync (puppet) | `tools\sync-test.ps1`: the host waited for its world, then the guest started, then 90 s of sync. Host issued **34** drive move orders; both sides exchanged packets (**host**: 3,404 sent / 2,185 received; **guest**: 2,169 sent / 3,346 received; **0 dropped**); the guest applied **830** positions with the engine's `SetPosition`; last reported host→guest distance **0.00 m**, both in area `0x000000ca`. Script verdict: **PASS**. |

| M3 — in-game join over the network | `tools\join-test.ps1`: host reached its world, wrote a manual save to a private slot (91) and served that slot's folder over TCP; the guest, at its main menu, received it (`join: received the host's slot over TCP (4 files)`), wrote it into a manual slot of its own profile (`join: wrote 4 file(s)`), asked the game to load it (`join: asked the game to load the host's save (slot 91)`) and reached the host's world (`game loaded: slot 91 kind manual`); then state sync continued (569 positions applied, last distance 0.15 m, both in area `0x000000ca`). Script verdict: **PASS**. |

| M4 — two separate avatars | `tools\two-avatar-test.ps1`: the host created a mabari beside the player and added it to the party with the engine's `AddToParty` (command 396) **before** the handoff save — `companion: created 0x00000363 … party list 2 member(s)`; the guest joined by save transfer, then switched control to party index 1 with `SelectPartyMemberIndexMessage` (`select: asking for a click on index 1`, control changed to `0x00000363` — the click-style replace, field 0; field 1 only adds to the selection and leaves control on the first entry). Both players drove their own creature; each game mirrored the other's: **host** controlled `0x00000082`, puppet `0x00000363`, puppet applied **4,815**, puppet distance **0.26 m**; **guest** controlled `0x00000363`, puppet `0x00000082`, puppet applied **5,274**, puppet distance **0.21 m**; both in area `0x000000ca`. Script verdict: **PASS**. |

Nothing here is a claim about full co-op yet: milestone 0/1 prove the loader,
the plugin, live engine reads and the two-instance rig; milestone 2 proves a
host-authoritative state channel inside two running games; milestone 3 proves a
network join that needs no manual file copying; milestone 4 proves **two
players controlling two different party members in the same world**, each
game mirroring the other's avatar. Combat, area transitions, a player-facing
UI and internet transport are still ahead.

## Our project

`dao-coop` (this repository) is a separate GPLv3 plugin repository that brings
DAOLib in as a submodule (`extern/DAOLib`, at `v2.5.0`-era master
`c2b59fe` + its address-library submodule).

- **[observed]** `CMakeLists.txt` uses `add_subdirectory(extern/DAOLib
  EXCLUDE_FROM_ALL)` + `add_daolib_plugin(daocoop SOURCES src/Plugin.cpp)`;
  `vcpkg.json` lists detours, spdlog, directxtex (DAOLib's own dependencies,
  as its `dev/PLUGINS.md` requires); build tree is off OneDrive at
  `E:\dao-coop-build\<preset>`.
- `src/Plugin.cpp` (milestone 0): logs to `daocoop.log`, once a second, the
  game mode, pause state, party-bar selection and controlled creature's id,
  area, position and health. It changes nothing in the game. With
  `DAOCOOP_AUTOLOAD=1` it also asks the game to load the newest save of the
  character offered at the main menu, so the two-instance rig runs without a
  mouse; the save lives in the named instance's own profile.
- **Built, deployed and run:** see the milestone log above.

### Safety evidence

- The owner's real `Documents\BioWare\Dragon Age` newest file is
  `daolib\loader.log` at **19:38:14** (the owner's own last session). Nothing
  in it was written by our runs; the instance test wrote only under
  `%LOCALAPPDATA%\DAOLib\instances\{host,guest}` (20:07–20:09).
- The test copy's `daorigins.exe` has the same SHA-256 as the owner's
  (`A79621C2…`); its `dinput8.dll` is the owner's DAOLib 0.5.0 release loader
  (`B2CDE35B…`), which our deploy will replace in the **test copy only**.

## Claim

Opened 2026-10-20 (owner approved the account): **"Claim a game: Dragon Age:
Origins (co-op on DAOLib, from zero)"**,
https://github.com/cranebeaker/coop-anything/issues/79 .

Project repository (published after milestone 2, owner's decision):
**https://github.com/vmosenkov/dao-coop** (public, GPL-3.0).

Contribution: **draft PR https://github.com/cranebeaker/coop-anything/pull/81**
— replaces the DAO gap entry with this project's entry, adds the game write-up
(`games/dragon-age-origins/README.md`) and these notes, QA level `automated`.
Draft pending the owner's review.

## Research (phase 0/1, done)

- No public co-op/multiplayer mod for DAO has ever shipped. DAO is listed as an
  open gap in coop-anything's `DIRECTORY.md`; BioWare confirmed in 2009 there
  were no plans, and 2021 interviews say a multiplayer mode was cut during
  development.
- **The one live attempt is `DAOKadanMP`** by `Whocares66-Dev` — **private**
  repository (GitHub 404 unauthenticated), no release, no license. It is
  evidenced only by public issue text in DAOLib/DAOEluvianMCP: two game
  processes on one save, clipboard-address joining, host/guest object-id
  matching (partial; counter drift when play continues after a save), shared
  chest contents, in-session saves loading (KadanMP issue 11); known gaps:
  kill bodybags/loot sync (DAOLib #103), attacks >60 m from the host's camera
  not registering (DAOLib #88).
- **DAOLib** is the enabler: SKSE-style native plugin loader + library, 100k-ID
  address library for GOG/EA/Steam 1.05.13263.0, all 827 script commands
  callable/interceptable, engine messages, per-frame callback on the script
  thread, creature state read/write, orders, input, Flash UI, saves.
- **DAOEluvianMCP** is a public MCP test bench plugin (drives a live game, two
  instances, own ports) — our intended automated QA rig.
- **2026 decompilation project** (Reddit `Shot_Let_1344`, ~60% after 3 years,
  release at ~90%, targeted 2027) **plans** net support/co-op but has no public
  artifact. Not a dependency.

## Next actions

1. ~~Install the C++ workload → build `daocoop.dll` + loader.~~ Done.
2. ~~Deploy to `E:\DAO-Coop-Test`, run with `DAOLIB_INSTANCE=host`, confirm
   `daocoop.log` shows live state (milestone 0).~~ Done.
3. ~~Start `DAOLIB_INSTANCE=guest` beside it; confirm two instances run with
   separate profiles (milestone 1).~~ Done.
4. Milestone 2: a loopback transport between the two instances and a puppet —
   each instance's controlled creature's state sent to the other and applied
   to its copy of the same id. Needs: a listener thread + queue drained in
   `OnFrame`, a versioned packet, and a write path for position (orders first,
   memory write as fallback).
5. Milestone 3: in-game join (host save → transport → guest `dao::saves::Load`)
   instead of the manual/autoload path used for M1.
6. Decide with the owner: publish `dao-coop` after M2; contact the DAOLib
   author about the private `DAOKadanMP` after M2.
