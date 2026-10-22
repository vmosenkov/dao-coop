# The in-game event log screen (SWF UI)

The co-op mod shows its session event log in a HUD screen of its own. The
screen is a plain SWF 8 movie with an ActionScript 2 frame script, built from
text — no Flash editor is involved. This is the same pipeline DAOTurnBased uses
(`tools/movie.py` there, GPLv3, by the author of DAOLib); our copy is adapted
to one movie and documented here.

## Files

| File | What it is |
|---|---|
| `ui/eventlog/frame_1/DoAction.as` | The panel's ActionScript 2 source. |
| `tools/movie.py` | Builds `ui/coop_eventlog.swf`: writes a one-frame SWF 8 shell, compiles the source into its frame script with FFDec, decompiles the result and refuses a script the compiler dropped. |
| `ui/coop_eventlog.swf` | The built movie (checked in; regenerate with the command below). |
| `src/EventLogUi.cpp` | `dao::gui::AddScreen(L"CoopEventLog", L"coop_eventlog", kLayerHud)`, `ShowScreen`, then pushes `EventLog` lines and the view size into the movie. |
| `tools/ui-shot.ps1` | One-instance smoke test: starts the game, waits for the world, screenshots the window, prints the `ui:` log lines. |
| `tools/ui-join-shot.ps1` | Two-instance test: host + guest join, screenshots the host window, checks that real events reached the movie. |

## Build the movie

FFDec (JPEXS, https://github.com/jindrapetrik/jpexs-decompiler) is the AS2
compiler; it is installed at `C:\Modding\ffdec_26.3.0` (Java on PATH):

```powershell
python tools\movie.py --ffdec C:\Modding\ffdec_26.3.0 coop_eventlog
```

The shell is `FWS`, SWF version 8, a 1280x720 stage, 30 fps, one frame. It
imports the game's two fonts the way the game's own movies do
(`ImportAssets2`: `gfxfontlib.swf`, `$TitleFont`, `$BodyFont`), so the panel is
drawn with DAO's own typefaces. FFDec's `-importScript` puts the compiled
`DoAction.as` into the shell's frame; a second FFDec pass (`-export script`)
proves the functions survived.

The movie is copied beside `daocoop.dll` by `add_daolib_plugin(... FILES
ui/coop_eventlog.swf)`, which is where `dao::gui::AddScreen`/`HasMovie` look
for a plugin's movies. `tools\deploy.ps1` copies the whole plugin folder, and
`tools\package.ps1` copies the SWF into the release package.

## Runtime bridge

`EventLogUi.cpp` runs from the plugin's frame callback:

1. Retries `AddScreen` until the GUI system is up (it exists from the first
   main-menu frame), then `ShowScreen` once.
2. About four times a second, takes `ScreenMovie(L"CoopEventLog")` and, when
   `EventLog::Revision()` changed, writes:
   - `_root.lines` via `Movie::SetArray` (the last 8 event texts, wide strings),
   - `_root.count`, `_root.revision` via `Movie::SetVariable`,
   - `_root.viewWidth` / `_root.viewHeight` from `dao::world::ScreenSize()`
     when the view size changes.
3. `EventLog` knows nothing about the movie; the movie knows nothing about the
   network. The SWF redraws only when `_root.revision` differs from the value
   it last drew.

`DoAction.as` lays the panel out in screen pixels (bottom-right, 440 px wide,
title with `$TitleFont`, rows with `$BodyFont`) and converts them to stage units
with the view size, because the movie's stage is scaled to the view while the
game's own interface is drawn at its pixel size. With no plugin (`_root.count`
unset, e.g. a Flash player) it shows sample lines beginning with
"Co-op Event Log initialized.". The panel clip has `hitTestDisable = true`, so
it never takes a world click.

## Tests actually run (2026-10-23)

- `tools\ui-shot.ps1`: the world loaded, log said `ui: event log screen added
  (movie coop_eventlog)`, `ui: event log view 960x540`; the screenshot showed
  the panel with "Co-op Event Log initialized." and the sample lines.
- `tools\ui-join-shot.ps1`: guest joined; host log `event: [multiplayer]
  Another player joined the game`, `ui: event log revision 1 (1 line(s))`;
  guest revision 2 (2 lines). The screenshot showed "Another player joined the
  game." in the panel — a real `EventLog::Recent()` event rendered by the SWF.

Screenshots from those runs: `E:\Temp_files\ui-shot.png`,
`E:\Temp_files\ui-join-shot.png` (not in the repository).

## Limitations

- The panel is always visible; no collapse, hide, filters or scrolling yet (the
  next UI step).
- Layout was checked at 960x540 and 1024x768; other aspect ratios can place it
  differently.
- The list is the last eight events of all categories; technical messages are
  kept out of the player log and only go to the file log.
- The movie needs the game's `gfxfontlib.swf`; a standalone Flash player needs
  an empty stub beside the movie for frame 1 to run (the builder of DAOTurnBased
  writes one; ours is tuned for the game).
- Mouse: the panel disables hit testing; there are no clickable controls yet.

## Reference

DAOTurnBased's `tools/movie.py`, `ui/turnorder/frame_1/DoAction.as` and
`dev/UI.md` document the same pipeline; our `tools/movie.py` is an adaptation
of that file under the same GPLv3.

## Combat events and folding (2026-10-23)

The log also carries combat lines from the arbitration path: "Combat started"
and "Combat ended" from the alive-hostile count, "Your creature attacks the
enemy" / "The other player's creature attacks the enemy" / "The enemy attacks
your creature" from the attack commands (throttled to one line per creature per
1.5 s), and "The hostile died". Technical command lines stay in the file log.
The panel folds to its last line when clicked; clicking it again unfolds it.
See `docs/images/event-log-combat.png` for a run with the fight in the panel.

## Filters and creature names (2026-10-23)

The panel has four filter tabs — **ALL**, **COMBAT**, **MULTI**, **SYSTEM** —
drawn as small clips along the top; a click sets the filter and the list shows
only the events of that category (the movie reads `_root.categories`, a second
array the plugin fills through `SetArray`). A click on the panel body still
folds it to its latest line. Combat lines now carry the engine's creature names
through `dao::script::api::GetName`: "Айдан attacks Огр", "Морриган attacks
Огр", "Огр died" (UTF-8 in the log, widened for the movie). See
`docs/images/event-log-filters.png`.

## Lifecycle: the log belongs to a game session (2026-10-23)

The screen is registered once through `AddScreen`, but it is shown only while
the game is in explore or combat mode (`dao::CClientExoApp::CurrentMode`), and
`HideScreen` runs when the main menu, the pregame or a loading screen comes
back. `tools\ui-menu-test.ps1` starts one instance without autoload, waits at
the main menu and checks that the screen was never shown; the window screenshot
is `docs/images/event-log-no-menu.png`. `tools\ui-shot.ps1` checks the other
half: after the world loads the log says `ui: event log screen shown`, and the
panel is in the screenshot (`docs/images/event-log-in-world.png`). Both
screenshots are taken with `PrintWindow`, so they work even when the game is not
the foreground window.
