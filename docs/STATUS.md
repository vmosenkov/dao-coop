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

| M5 — hostile health/death channel | `tools\combat-test.ps1`: the host spawned an ogre (`ogre_default.utc`) before the handoff save with the scripts' AI-off switch (`CREATURE_RULES_FLAG0` bit 8) and inactive, so neither game entered combat and the save was allowed; after the guest joined, the host damaged it on a schedule (12 ticks, health `220 → 0`) and carried its id/position/health in every state packet. The guest received the same hostile (`0x00000363`), took health `0.0` (the death state) and applied it **13** times with the engine's property setter. Script verdict: **PASS**. |

Nothing here is a claim about full co-op yet: M0/1 prove the loader, plugin and
two-instance rig; M2 a host-authoritative state channel; M3 a network join with
no manual file copying; M4 two players controlling two different party members
in one world; M5 the hostile health/death channel (with plugin-applied damage
standing in for the engine's combat, AI and death animations still unsynced).
A player-facing UI, real combat arbitration, area transitions and an internet
transport are still ahead.

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

## Round 4 — release package, config file, LAN transport (verified 2026-10-20)

| Step | Evidence |
|---|---|
| Config file | `daocoop.ini` beside the plugin, env vars override it. One host instance started with only `DAOLIB_INSTANCE=host` and **no** `DAOCOOP_*` variables: log said `config: 8 value(s) from …\daocoop.ini`, `autoload on`, `net: role host listening on 0.0.0.0:45830 sending to 127.0.0.1:45831`, `handoff on`, then reached the world and loaded a save. So a player edits one file; nothing else is needed. |
| Release build | `tools\build.ps1 -Preset release -Offline` (reuses the packages the debug build installed; vcpkg's own release configure was hanging on a GitHub download) → `dinput8.dll` 868,352 bytes, `daocoop.dll` 555,008 bytes (release runtime — on a player's PC the debug build would need VS debug runtimes). |
| Package | `tools\package.ps1` → `E:\dao-coop-build\dist\dao-coop-0.6.0.zip` (6.1 MB): loader + address files + plugin + `README-INSTALL.txt` + `daocoop.ini.example`, laid out as the game folder. |
| Release build tested | `tools\join-test.ps1` on the release build: **PASS** — host served slot 91, guest loaded it, 58 positions applied, last distance 0.01 m, same area. |
| LAN transport | `DAOCOOP_PEERIP`/`HOSTIP` set to this machine's non-loopback address `192.168.56.1`: both instances bound `0.0.0.0` and logged `sending to 192.168.56.1:…`; the join + sync test **PASSED** (host served the save, guest joined, same area). The code path a LAN of two PCs uses is therefore exercised; a second physical PC has not been tried. |

## Round 5 — nearby-hostile table and combat arbitration (verified 2026-10-22)

| Step | Evidence |
|---|---|
| Windowed test rig | `tools\windowed.ps1` puts the two named instances into 960×540 windows (`FullScreen=0`); `-Arrange` moves the running copies side by side (0,0 and 960,0). The owner's real profile is untouched. |
| Nearby hostiles (v3) | `StatePacket` v3 (140 bytes) carries up to four hostiles (`id, position, health, dead flag`). The host collects them with `script::api::GetNearestObjectByHostility` (creatures with max health > 2, within 40 m) and the guest applies position and health. `tools\combat-test.ps1` **PASS**: both sides report the same lowest ogre health 18 (host 172 samples, guest 177), 66 health applications at the guest, id 0x363 on both. The ogre regenerates out of combat, so the test compares the minimum both sides saw, not the final value. |
| Visible fight | `tools\demo-fight.ps1`: the ogre spawns six metres away and is woken after the handoff save is written (it must stay inactive before that, or the engine refuses to save in combat); both creatures are ordered to attack and DAO resolves every blow. The owner watched the fight in two windows; screenshots showed the ogre being hit and the dog moving. |
| Combat arbitration | The guest turns its own attack order (`commands::OnStart` on its controlled creature) into an `AttackRequestPacket` (request id, attacker, target). The host validates (known guest, not a repeat, attacker and target exist, target alive, same area, hostile) and runs `dao::orders::Attack` so DAO resolves the blow; an `AttackResultPacket` goes back. `tools\attack-test.ps1` **PASS**: guest sent request 1 (attacker 0x366, target 0x363), the host accepted it, the guest logged `accepted` for the same id. A request that arrives before the host has seen the guest's first state packet is refused as `bad-attacker` and the guest retries the same id (seen in the first run's log). |
| Known limitations (open) | Animations and damage numbers are computed locally in each game and are not replicated; only position and health agree. When the host dies (or its world stops), the host stops sending state, so the guest keeps playing while its NPCs run in place — there is no pause/death handshake yet. The guest's own local blow still resolves in its own world; the host's health overwrites the result, so the authoritative fight is the host's. |

## Round 6 — death, control and pause (2026-10-22, partly verified)

| Item | Evidence |
|---|---|
| Packet v4 | `StatePacket` is 144 bytes: the player's current and **maximum** health, so a mirrored creature keeps its proper maximum. Before this the pet on the host sat at `health 1.0 / 1.0` (its template's maximum), which is why guest-attack orders were accepted but did almost no damage. |
| Avatar fixed | Each game remembers the creature it owns (`g_myAvatar`) and keeps naming it on the network even when the game moves control to another party member. The host's creature is fixed at the first control event; the guest's after its party-member click. |
| Control protection | A control change away from the avatar no longer hands over the other player's creature: the client asks for its own avatar back (`control: not taking 0x…; asking for the player's creature`). This is the host-not-stealing-the-guest's-character fix. |
| Pause sync | `StatePacket::flags` bit 0 is the sender's tactical pause; the other game follows it with the engine's own `PauseMessage`/`UnpauseMessage` (45-frame cooldown). `tools\pause-test.ps1` run: guest pressed pause, both logs show `paused true`; the "following" log did not appear because the host was already paused (DAO pauses a window that lost focus while the guest was started), so the states already agreed. |
| Death test | `tools\death-test.ps1`: host party not raised, guest raised, ogre 300 HP. Host player fell; **the host world kept running** (415-434 state lines after the fall) and the guest kept sending attack requests (7-135, all accepted). The ogre did not die in those runs because the mirrored pet was clamped to 1 HP; that is fixed by v4 but the death+revive chain needs a fresh run. |
| Revive (unverified) | After the fight, fallen party members are set to their maximum health (the documented zero-to-positive health transition rebuilds placement and clears the dead timer, `dev/COMBAT.md:403`) and the host's control is moved back to its avatar. Not yet observed in play. |
| Still local | Animations, damage numbers, effects and the enemies' own actions are computed by each game separately; only position and health are replicated. The guest's ogre stands still while the host's fights. This is the next large step (combat event replication). |
## Round 7 — session event log and guest rejoin (verified 2026-10-23)

| Item | Evidence |
|---|---|
| Event log foundation | `src/Events.h`: categories multiplayer / world / combat / system, a 200-event ring the future UI can read. `Notify()` writes `event: [category] …` to the file log and shows short events on screen. |
| On-screen output | `DEBUG_PrintToScreen` is a no-op in the shipped game and `PrintString` consumes its arguments (Codex checked the disassembly notes); the verified path is `DisplayStatusMessage` (script command 747, FloatyLayer handler), now used with a 40-frame throttle. Visibility in combat is inferred, not yet confirmed by eye. |
| Events | "Another player joined the game", "Joined the host's game", "Connection to the other player lost" / "Reconnected", "The hostile died" (combat), "Host loaded another save; saving and serving the new world" (world), "Rejoined the host's new world". Seen in `daocoop.log` of both instances. |
| Guest rejoin | The host publishes `kWorldReloaded` in the state flags when it loads another save, then re-saves slot 91 and serves it again. The guest resets its join state (files, selection, avatar, mirrored state) on that flag and goes through receive → write → load again; the game shows its loading screen and the guest re-selects the second party member. `tools\reload-test.ps1` **PASS**: host loaded manual slot 2, served the world a second time; guest saw the reload, received the new save, loaded it, logged "Rejoined the host's new world". |
| Level test save | The owner's new save is used for tests: `Hero1/QuickSave_1` (health 200, Fort Drakon second floor, 4 party members). No extra dog is created when the save already has a party; the demo keeps high-level health; the demo ogre spawns at the player's feet (offsets landed outside navigation in that level) and its health is raised for the demo (values above 9999 are clamped by the game). |
## Round 8 — death/revive verified; event panel investigated (2026-10-23)

| Item | Evidence |
|---|---|
| Death + revive | `tools\death-test.ps1` **PASS**: the test kills the host player on purpose three seconds after the fight starts (`DAOCOOP_TEST_KILL_PLAYER=1`), the host's world keeps running while the guest is alive (state lines after the fall), the hostile is then defeated through the test hook, the revive path sets fallen party members back to their maximum health (the documented zero-to-positive health transition) and the host's control returns to its avatar; the avatar is above zero health afterwards. Note: the ogre did not finish the host by itself in the level save, so the *combat* death is staged with a test hook; the revive path itself is what the test verifies. |
| Revive fix | A corpse left in the hostile table no longer counts as an ongoing fight: `RevivePartyAfterFight` now looks only at entries that are alive (`!dead` and health > 0), on the host and through the mirrored table on the guest. Before this the dead ogre kept the fight "running" and revive never fired. |
| In-game log window | Investigated with Codex: `PrintToLogWindow` (82) pops its arguments and does nothing, and `PrintToLog` (5) writes only to the engine's Script file-log channel (off by default); the game has no message-log movie. A persistent panel therefore needs the plugin's own SWF loaded into a HUD movie (`dao::gui::ScreenMovie`, `LoadInto`, `Movie::SetVariable`/`SetArray` in `Gui.h`), which needs a `.swf` asset that does not exist yet. The current on-screen output stays `DisplayStatusMessage` (throttled). |
| Test hooks | `DAOCOOP_TEST_RELOAD` (load another save, guest rejoins), `DAOCOOP_TEST_VICTORY` (defeat the hostile after the fall), `DAOCOOP_TEST_KILL_PLAYER` (kill the host player on purpose). |
## Round 9 — in-game Event Log SWF (verified in DAO, 2026-10-23)

| Item | Evidence |
|---|---|
| Pipeline | `ui/eventlog/frame_1/DoAction.as` (ActionScript 2) -> `python tools\movie.py --ffdec C:\Modding\ffdec_26.3.0 coop_eventlog` -> `ui/coop_eventlog.swf` (SWF 8, 4057 bytes), packaged by `add_daolib_plugin(... FILES ui/coop_eventlog.swf)` beside `daocoop.dll`. The generator is adapted from DAOTurnBased's `tools/movie.py` (GPLv3); the shell imports the game's `$TitleFont`/`$BodyFont` from `gfxfontlib.swf`, and FFDec compiles and then re-exports the script to prove it survived. See `docs/UI.md`. |
| Integration | `src/EventLogUi.cpp` retries `dao::gui::AddScreen(L"CoopEventLog", L"coop_eventlog", kLayerHud)` until the GUI system is up, shows it, then four times a second pushes the view size and, when `EventLog::Revision()` changed, the last eight event texts through `Movie::SetArray` + `_root.count`/`_root.revision` through `Movie::SetVariable`. |
| Smoke test 1 | `tools\ui-shot.ps1`: host reached the world; log `ui: event log screen added (movie coop_eventlog)`, `ui: event log view 960x540`; the window screenshot shows the panel with "Co-op Event Log initialized." and the sample lines. |
| Smoke test 2 | `tools\ui-join-shot.ps1`: host + guest join; host log `event: [multiplayer] Another player joined the game` then `ui: event log revision 1 (1 line(s))`; guest revision 2. The host screenshot shows "Another player joined the game." — a real `EventLog::Recent()` event drawn by our SWF. |
| Verified UI API | `dao::gui::AddScreen/ShowScreen/ScreenMovie`, `Movie::SetVariable/SetArray`, `Value::Number/WString`, `HasMovie`, `dao::world::ScreenSize` — all cited in `Gui.h`/`World.h`; no invented calls. |
| Limits | Panel always visible (no collapse/filters/scroll yet); layout checked at 960x540/1024x768; last eight events of all categories; the movie needs the game's font library (a standalone player needs a stub). |
## Round 10 — combat events in the in-game log (verified 2026-10-23)

| Item | Evidence |
|---|---|
| Combat events | `event: [combat] Combat started`, "Your creature attacks the enemy", "The other player's creature attacks the enemy", "The enemy attacks your creature", "Combat ended", "The hostile died" appear in the host/guest logs during `tools\demo-fight.ps1`; the screen shows them (see `docs/images/event-log-combat.png`). Attack lines are throttled per creature (1.5 s). |
| Folding | A click on the panel folds it to its latest line and another click unfolds it (`panel.onPress` in `DoAction.as`); the text fields have `hitTestDisable`, so the panel takes the click. Not yet exercised by hand. |
| Still missing | The attack lines name sides, not creatures (no creature names in the protocol yet); no filters, scrolling or event details; combat is only the attack-start/death events the arbitration sees (miss/damage happen inside DAO's own resolution). |
## Round 11 — log filters and creature names (verified 2026-10-23)

| Item | Evidence |
|---|---|
| Filters | The movie received `_root.categories` from `EventLogUi.cpp` (`SetArray` of `int32`), and draws ALL/COMBAT/MULTI/SYSTEM tabs; a click sets the filter. Shown on screen in `docs/images/event-log-filters.png`; the filter click itself was not exercised by hand yet. |
| Creature names | `dao::script::api::GetName` (command 275) feeds the combat lines: the demo fight logged "Морриган attacks Огр", "Айдан attacks Огр", "Dusty attacks Огр", "Огр died". Log text is UTF-8; `EventLogUi` widens it for AS2 (`MultiByteToWideChar`). |
| Still missing | Scrolling history and expanded event details; player names for multiplayer lines (the protocol carries only ids); the filtered tab click is unverified by hand. |
## Round 12 — coop-anything contribution updated (2026-10-23)

| Item | Evidence |
|---|---|
| Update PR | Draft [PR #141](https://github.com/cranebeaker/coop-anything/pull/141) in cranebeaker/coop-anything (branch `game/dragon-age-origins-update` in the vmosenkov fork): the Dragon Age: Origins write-up now describes the verified combat arbitration, death/revival, guest rejoin and the in-game Event Log (milestones M7-M10), the QA section lists the new tests, the known issues and next steps are current; `DIRECTORY.md` was regenerated from `data/directory.yaml` (entry `prototype` → `alpha`, `players_visible: yes`, notes and `checked: 2026-10-23`). The PR is a draft so the owner can review it, per the hub's rules. |
| Earlier PR | [PR #81](https://github.com/cranebeaker/coop-anything/pull/81) (write-up + directory entry + prototype) was merged earlier. |
| Filters and names | Runtime-verified: the panel shows ALL/COMBAT/MULTI/SYSTEM tabs and combat lines with the engine's creature names ("Морриган attacks Огр", "Айдан attacks Огр", "Огр died"); see `docs/images/event-log-filters.png`. Clicking a tab is not yet exercised by hand. |
## Round 13 — settling the mirrored creature (2026-10-23)

| Item | Evidence |
|---|---|
| "Runs in place" | The mirrored creature could keep the animation of its last command after a fight. `ClearStalePuppetCommands` (called from the frame callback on both roles) now checks, once per 2.5 s, that no fight is near (host: the alive-hostile count; guest: the mirrored hostile table) and that the copy still has a current command (`dao::commands::HeldFor`), and then drops it with `dao::script::api::ClearAllCommands(copy, 1)` (script command 538, a hard clear). A previous attempt with `RemoveCommandByIndex` did not clear it and logged every 2.5 s; the hard clear does, and the demo fight needs no clearing (its commands end naturally), so this is a safety net for a real play session. `attack-test` still PASSes after the change. Visual check of the behaviour in a live play session is still to do. |
## Round 14 — release package 0.7.0 verified (2026-10-23)

| Item | Evidence |
|---|---|
| Package | `tools\package.ps1 -Version 0.7.0` built `E:\dao-coop-build\dist\dao-coop-0.7.0.zip` (7.0 MB, release preset): DAOLib loader, address files, the plugin with `coop_eventlog.swf` beside it, `README-INSTALL.txt` and `daocoop.ini.example`. |
| Release tests | `tools\deploy.ps1 -Preset release`, then **`attack-test` PASS** (guest attack request accepted and resolved by the host) and **`reload-test` PASS** (host loaded another save, served it, guest reloaded and rejoined) on the release build. |
| Install notes | `tools\README-INSTALL.txt` now lists combat arbitration, death/revive, guest rejoin and the in-game event log under "What works today (verified 2026-10-23)", and the missing animation/effect/loot sync, world-door transitions and the ini-only menu under "What does not work yet". |
## Round 15 — Event Log lifecycle fixed (2026-10-23)

| Item | Evidence |
|---|---|
| Bug | The event log panel showed in the main menu, before any save was loaded: `AddScreen` was followed by `ShowScreen` unconditionally from the first frames. |
| Fix | `EventLogUi.cpp` now checks `dao::CClientExoApp::CurrentMode()`: the screen is shown only in explore or combat mode, hidden (`HideScreen`) in the main menu, the pregame and the loading screens, and no events/view size are pushed outside a session. |
| Test | New `tools\ui-menu-test.ps1`: one instance with no autoload stays at the main menu; the log has `ui: event log screen added` but no `screen shown`, the screenshot has no panel — **PASS**. Then `tools\ui-shot.ps1`: after the world loads, `ui: event log screen shown` appears and the panel is in the screenshot. Both screenshots use `PrintWindow` (works with a background game window). See `docs/images/event-log-no-menu.png` and `docs/images/event-log-in-world.png`. The release package `dao-coop-0.7.0.zip` was rebuilt with the fix and both tests pass on the release build (`ui-menu-test` PASS, `ui-shot` shows the panel only after the world loads). |
## Round 16 — owner's two bugs: duplicate announcement and the run animation (2026-10-23)

| Item | Evidence |
|---|---|
| Duplicate "Another player joined the game" | `Notify` no longer calls `DisplayStatusMessage`; the in-game event log is now the only on-screen place for session events (the file log keeps everything). On the host, a join shows the line only in the panel: no large message is in `docs/images/walk-host.png` / `docs/images/walk-guest.png`, and the screenshots from the join test show the panel only. |
| Mirrored creature's run animation | A teleport never plays a run animation, so a gap under 3 m is now walked with the game's own move order: `dao::orders::MoveTo(puppet, target)` about three times a second, and the creature's movement controller plays walk/run. A teleport remains for gaps over 3 m, for a walk with no progress for a second (a wall), and it clears the queue then; the first frames of an attack still skip all movement. The net line has a new `puppet_walks` counter. New `tools\walk-test.ps1` (host + guest join, both drive their creature for 90 s): **PASS** — host 33 drive orders, 248 puppet move orders, puppet distance 0.56 m; guest 15 drive orders, 209 puppet move orders, 0.77 m; both in the same area. |
## Next actions

1. Owner playtest: two PCs on the LAN, then an internet transport (Steam relay is the planned one).
2. Check the run animation and the notification fix by hand in a real session.
3. Verify the filter tabs and folding by hand; add scrolling through history and event details.
4. Review and undraft PR #141 in coop-anything.
5. Replicate combat animations/effects between the games; then loot and bodybags.
6. World-door area transitions; an in-game host/join menu to replace the ini file.
7. Rebuild and re-test the 0.7.0 package after this round.

