// A small event log for the co-op session: multiplayer, world, combat and
// system events in one place. The plugin writes them to the file log
// (spdlog) and shows the important ones on screen; a real UI panel can read
// Recent() later instead of the network code touching any UI.
//
// Foundation only: a bounded ring of the last events, no filters, no layout.
#pragma once

#include <cstdint>
#include <ctime>
#include <deque>
#include <string>

namespace coop
{

enum class EventCategory : std::uint8_t
{
    Multiplayer = 0,
    World = 1,
    Combat = 2,
    System = 3,
};

inline const char *EventCategoryName(EventCategory category)
{
    switch (category)
    {
    case EventCategory::Multiplayer:
        return "multiplayer";
    case EventCategory::World:
        return "world";
    case EventCategory::Combat:
        return "combat";
    case EventCategory::System:
        return "system";
    }
    return "?";
}

struct GameEvent
{
    EventCategory category = EventCategory::System;
    std::string text;
    std::uint32_t atMs = 0; // session clock, for the future UI
};

class EventLog
{
  public:
    static constexpr std::size_t kMaxEvents = 200;

    static EventLog &Get()
    {
        static EventLog log;
        return log;
    }

    void Add(EventCategory category, std::string text)
    {
        events_.push_back(GameEvent{category, std::move(text), static_cast<std::uint32_t>(events_.size())});
        if (events_.size() > kMaxEvents)
        {
            events_.pop_front();
        }
        ++revision_;
    }

    const std::deque<GameEvent> &Recent() const { return events_; }
    // Rises on every event; a UI redraws only when it changed.
    std::uint32_t Revision() const { return revision_; }

  private:
    std::deque<GameEvent> events_;
    std::uint32_t revision_ = 0;
};

} // namespace coop
