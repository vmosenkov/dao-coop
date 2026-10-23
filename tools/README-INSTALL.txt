dao-coop for Dragon Age: Origins - Ultimate Edition
===================================================

What it is: a co-op mod for Dragon Age: Origins, built as a DAOLib plugin.
Two players, two game processes: the guest joins by receiving the host's save,
and each game mirrors the other player's character and the hostile the host
keeps in sync. This is a work in progress: see "What works" below before
expecting a finished co-op game.

Requirements
------------
- Dragon Age: Origins - Ultimate Edition, version 1.05.13263.0. The Steam, GOG
  and EA builds all work through DAOLib's address library. Both players need
  the SAME store build? No: the address library maps all three, but both
  players must run the same game build *and* the same dao-coop version.
- DAOLib 0.6.0 from https://github.com/Whocares66-Dev/DAOLib/releases
  (the mod builds on it; its loader and address files are included in this zip
  as well, so the DAOLib install step is already covered by copying this zip).
- The latest Microsoft Visual C++ Redistributable, x86 version: DAOLib 0.6.0's
  documentation names it as a requirement, and older redistributables stop the
  game at start with 0xc0000142.

Install (both players)
----------------------
1. Extract the zip over the game folder, the one that holds bin_ship. Do it
   with the game closed. That installs:
     bin_ship\dinput8.dll               the loader
     bin_ship\daolib\addresses\*.bin    the address library
     bin_ship\daolib\plugins\daocoop\   this mod
2. Copy daocoop.ini.example to
     bin_ship\daolib\plugins\daocoop\daocoop.ini
   and edit [net] role and peer.

Host
----
In daocoop.ini:
  role = host
  autoload = 1      (or load a save by hand at the main menu)
  handoff = 1
  add_companion = 1
  peer = <the guest's LAN address>     (optional; the host can leave it)
Start the game, load a save, play. After a few seconds a companion is hired
for the guest, the game saves to a private slot and serves it to the guest.
Open UDP 45830 and TCP 45832 on the host PC's firewall if the guest cannot
join.

Guest
-----
In daocoop.ini:
  role = guest
  peer = <the host's LAN address>
  join = 1
  select_party = 1
Start the game and wait at the main menu. The mod receives the host's save,
writes it into a manual slot of the character it finds in your profile, loads
it, and puts you in the host's world controlling the companion. Open UDP
45831 on the guest PC's firewall.

Testing both copies on one PC
-----------------------------
Two game processes only run side by side as DAOLib named instances: start each
with the environment variable DAOLIB_INSTANCE set to a different name
(host/guest). Each instance gets its own Documents profile under
%LOCALAPPDATA%\DAOLib\instances\<name>, so the player's saves are untouched.
For the same-PC test the test rig sets DAOCOOP_* environment variables, which
override the ini; a human on one PC can do the same.

What works today (verified 2026-10-23)
--------------------------------------
- The plugin loads through DAOLib and reads the live game state.
- Two named instances run side by side, and after loading the same save they
  agree on object ids.
- A guest joins over the network by save transfer; no file copying.
- Two avatars: the host controls one party member, the guest another, and each
  game mirrors the other player's creature.
- The host's hostile creature's position, health and death state reach the
  guest.
- Combat arbitration: the guest's attack orders are validated and resolved in
  the host's game, so both players really fight the same enemy.
- A fallen player does not end the world while another party member is alive;
  after the fight the fallen party is revived and control returns.
- When the host loads another save mid-session, the guest reloads it and
  rejoins by itself (a loading screen, no restart).
- In-game event log: a panel in the HUD (our own SWF) shows the session's
  events (multiplayer, combat with creature names, system) with filter tabs;
  click it to fold or unfold.

What does not work yet
----------------------
- Combat animations and effects, loot and bodybags are not synced; the two
  games can show a swing at different moments.
- Area transitions: loading another save on the host makes the guest rejoin,
  but walking through a door in the world does not move the guest yet.
- Quests, inventories, dialogue and pause are not synced (pause agreement is
  tested but not a full pause UI).
- An in-game host/join menu: the ini file is the interface.
- Internet play (LAN only for now; the Steam relay is not wired up).
- Only the developer tested builds: no player reports yet.

Logs: Documents\BioWare\Dragon Age\daolib\loader.log and daocoop.log (a named
instance writes them under its own profile).

Source and issues: https://github.com/vmosenkov/dao-coop (GPL-3.0). It builds
on DAOLib by Whocares66-Dev, GPL-3.0.


