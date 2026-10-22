// The UI bridge: shows the co-op event log in a HUD screen of its own.
//
// It reads EventLog and the game's screen size and writes them into the movie
// that dao::gui::AddScreen installed (ui/coop_eventlog.swf). Nothing here
// knows about the network, and EventLog knows nothing about the movie.
//
// Call Tick() from the frame callback, on the game thread.
#pragma once

namespace coopui
{

void Tick();

} // namespace coopui
