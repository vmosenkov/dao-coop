# dao-coop — design

Co-op for **Dragon Age: Origins – Ultimate Edition** on top of
[DAOLib](https://github.com/Whocares66-Dev/DAOLib). This document is the plan;
what has actually been run is in `STATUS.md`. Facts marked **[observed]** come
from a source or a run recorded in `STATUS.md`; **[derived]** are our
inferences; **[unknown]** is open.

## 1. What the game gives us, and does not

- **[observed]** DAO uses BioWare's Eclipse engine (an upgraded Aurora/NWN
  engine). The retail PC build has **no netcode** and a multiplayer mode was
  cut during development (2021 developer interviews). `DIRECTORY.md` lists it
  as an open gap; no public co-op mod has ever shipped.
- **[observed]** The game is strictly single-player in its model: one
  controlled creature at a time (the player may switch between party members),
  one party inventory, one global pause, one dialogue/cutscene camera.
- **[observed]** Game logic is 827 numbered script commands run by an in-engine
  VM on one thread; 314 engine message handlers; native classes with RTTI.
- **[observed]** DAOLib 0.5.0 + DAOAddressLibrary (~103k stable IDs for
  GOG/EA/Steam builds of 1.05.13263.0) is an SKSE-style loader and C++ library.
  It exposes the script command table (call/intercept/add), engine messages,
  per-frame callback on the script thread, object/creature state read (and
  creature properties write), orders that are indistinguishable from player
  clicks, input injection/withholding, the Flash UI (custom screens/chat),
  save hooks and per-plugin co-save files, and the game's modes/pause.
- **[observed]** Two game processes can run on one machine when each is a named
  DAOLib instance (`DAOLIB_INSTANCE=<name>`): separate process mutex,
  separate Documents profile (settings, saves, screenshots, logs), seeded once
  from the default profile's saves.
- **[derived]** Therefore co-op = **two full game processes** + a replication
  layer in a plugin on each. There is no client/server split to reuse and no
  determinism guarantee, so **host-authoritative state sync** is the model
  (not lockstep).

## 2. Prior art we build on

| Who | What | Use to us |
|---|---|---|
| DAOLib (`Whocares66-Dev/DAOLib`) | loader + library; public GPLv3 | our hook layer and half the engine map |
| `DAOKadanMP` (same author, **private**) | in-development multiplayer mod; evidence only in public DAOLib/Eluvian issue text | proves the approach: clipboard-address join, two instances on one save, save-transfer join, object-id matching, guest AI suppression. Not available to us; a lead to contact, not a dependency. |
| `DAOTurnBased` (Nexus 7053) | turn-based mode; paused-clock and command-queue work | patterns for pausing/serialising commands |
| `DAOEluvianMCP` (public) | MCP test bench plugin driving a live game over HTTP/MCP | our automated two-instance QA rig |
| 2026 decompilation project (private, ~2027) | planned net support/co-op | not a current artifact |

## 3. Architecture

### 3.1 Session model: host authority + save-transfer join

1. **Host** plays normally. On a guest joining, the host writes a save
   (`dao::saves::Save`) to a slot of its own and sends the slot's folder —
   `.das`, `.das.met`, `screen.dds` — to the guest.
2. **Guest** writes that folder into its own profile (a character folder it
   already has or makes) and loads it (`dao::saves::Load`), from the main menu
   at first. **[observed by the private mod]** a guest is in the host's world
   9–19 s later; a load numbers its new objects from the save's own counter, so
   after the load both games agree on every object id.
3. From then on the host owns the simulation; the guest mirrors it and sends
   its player's input/orders.

The alternative (each side transitions levels independently) needs identical
object numbering; DAOLib gives `dao::objects::OnAreasBuild` (number a build's
new objects from a plugin-given id) and `NumberNewFrom` to keep the counters
aligned. We start with save-transfer join because it doesn't depend on that
alignment.

### 3.2 Two players in one party

Both games run the same save, so both have the same four party members with
the same ids. Each instance's "player" is one of those creatures:

- the host controls the Warden (as normal);
- the guest controls one party member (its own selection in the party bar).

For the *other* game, that creature is a remote player: its AI/scripted
following is suppressed and its state is driven by the network. DAOLib's
`docs/AI.md` records the scripts' own switch for a creature's AI
(`CREATURE_RULES_FLAG0` bit `0x00000008`, `CREATURE_RULES_FLAG_AI_OFF`) and
the private mod's session used exactly that to make a guest's copies of
creatures choose nothing.
- **[derived]** No puppet spawning is needed for party members: both worlds
  hold the same objects. This is the single biggest simplification of the
  design and the reason the save-transfer join is worth its cost.

### 3.3 Sync channels (v1)

Ticked from the plugin's `dao::OnFrame` on the script thread, 10–20 Hz,
unreliable for state and reliable for events:

| Channel | Source → destination | Notes |
|---|---|---|
| Player state | each instance's own controlled creature → the other | area id, position, facing, health; the receiving game sets it for its copy of the creature |
| Orders | each instance's input → the other | attack/use/move as `dao::orders::*` (they look like clicks); command queue events (`dao::commands::OnStart/OnEnd`) carry target and ability ids |
| Hostile state | host → guest | creature position/health/combat state/death for hostiles near the players; guest's copies have AI off |
| Damage claims | guest → host | the guest's parties' blows are resolved host-side; the host applies and broadcasts the result (the private mod's sessions show why: a guest's attack far from the host's camera must still count) |
| Effects/timers | host → guest | effect ids and remaining time; the guest ages them on its own clock |
| Area transitions | host → guest | guest starts the same transition at the same moment; ids aligned by the save counter / `OnAreasBuild` |
| Save | host → guest | canonical save is the host's; guest never overwrites its own real saves (named-instance profile or a dedicated character folder) |

Movement: prefer the game's own movement machinery (move orders / path
requests) over writing positions, so animation, collision and pathfinding stay
consistent; writing an object's position memory every frame is the fallback
(DAOLib reads position at `CCObject+0x120`).

### 3.4 Who may pause / talk / change areas (v1 policy)

- Pause: each side may pause its own game; a pause is **not** shared in v1
  (real-time-with-pause means the other player keeps playing; sharing it would
  need a pause vote).
- Dialogue, cutscenes, level transitions, saving: **host only**. The guest is
  told and follows; the guest's copies of the creatures are frozen where the
  host is in a conversation. **[unknown]** what the guest's screen should show
  (we start by mirroring the host's game mode message).
- Death/party-wipe: host's state wins; guest respawns as the host does.

### 3.5 Transport

- **v0** loopback UDP, two instances on the host PC (this is also the
  automated-test rig).
- **v1** LAN UDP (owner and friend are on the same network) — plain
  unreliable+reliable channels, ~20 Hz, with latency/loss simulation for
  tests.
- **v2** Steam relay through Spacewar (coop-anything's
  `playbook/networking-steam-spacewar.md`) so the mod also works over the
  internet without port forwarding; the friend has the Steam edition.

### 3.6 Identity and build checks

- Plugin refuses to load unless `LoaderInfo::knownExecutable` and plugin API
  version match. Host and guest must run the same game build
  (1.05.13263.0, GOG/EA/Steam all mapped by the address library) and the same
  mod build; the protocol carries a version string and refuses mismatches.
- Session ids: a stable per-save "session id" in the plugin co-save
  (`dao::saves::Slot::Read/Write`), so a rejoin can be matched to the world.

## 4. Milestones

| # | Milestone | Exit test |
|---|---|---|
| M0 | plugin loads; reads live state | `daocoop.log` shows controlled creature id/area/position/health once a second in the test install |
| M1 | two isolated instances | `DAOLIB_INSTANCE=host` and `guest` run together, separate profiles, both log state |
| M2 | loopback replication: puppets | guest's controlled creature moves in the host's game and vice versa |
| M3 | in-game join | host clicks "host", saves; guest clicks "join", receives save, loads, appears in host's world — no manual file copying |
| M4 | combat sync | both players damage the same hostile; both see the same health/death; loot appears once |
| M5 | transitions + saves | 15 consecutive area transitions with identical object-id checks; host save/load with guest following |
| M6 | polish + LAN + Steam relay | friend joins over the network; options screen has host/join/settings |
| M7 | automated QA + write-up + PR | two-instance suite drives every scenario above; coop-anything directory/write-up updated |

## 5. Risks and unknowns

1. **Dual simulation conflicts.** Both games simulate the same world; the
   non-authority must suppress AI/scripts without breaking quest triggers.
   Mitigation: suppress only what is mirrored; host runs story.
2. **Save-transfer join cost.** 8–20 s per join and heavy for transitions;
   acceptable to start, later replace with `OnAreasBuild` alignment.
3. **Combat resolution across two processes.** The private mod had a bug here
   (blows far from host camera not counted); design damage as host-resolved
   intents from the start.
4. **Engine thread rules.** Script commands must run on the script thread;
   network I/O must not block it (a listener thread + a queue drained in
   `OnFrame`).
5. **32-bit memory ceiling** with two games on one PC for testing (each a
   process, so OS handles it; per-process address space is the usual DAO
   problem, DAOLib's heap fix helps).
6. **Licensing.** DAOLib is GPLv3; our plugin is GPLv3 and we publish source.
   Release = loader + address library + our plugin; never game files.
7. **No access to the private mod.** We reimplement from the public DAOLib
   notes; we should try to contact its author (owner decision) before
   duplicating effort.
8. **Version drift.** DAOLib is moving fast (0.1.0→0.5.0 in nine days);
   pin a release and upgrade deliberately.
9. **Unknown: does a guest's game tolerate its characters being driven
   remotely** (pathing, animations, command queue)? M2 answers it.
10. **Unknown: pause/dialogue arbitration** beyond v1 policy.
