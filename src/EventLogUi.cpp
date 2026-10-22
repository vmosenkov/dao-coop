// The event log screen: adds ui/coop_eventlog.swf as a HUD screen once the GUI
// system is up, then pushes EventLog's last events and the view size into it.
#include "EventLogUi.h"

#include "Events.h"

#include <Windows.h>

#include "dao/ClientExoApp.h"
#include "dao/Frame.h"
#include "dao/GameModes.h"
#include "dao/Gui.h"
#include "dao/World.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <span>
#include <vector>

namespace coopui
{
namespace
{

constexpr std::wstring_view kScreen = L"CoopEventLog";
constexpr std::wstring_view kMovie = L"coop_eventlog";
constexpr std::size_t kShownEvents = 8;

bool g_added = false;
bool g_shown = false;
std::uint64_t g_nextAddFrame = 0;
std::uint64_t g_nextTickFrame = 0;
std::uint32_t g_lastRevision = std::uint32_t(-1);
int g_lastViewWidth = -1;
int g_lastViewHeight = -1;
std::vector<std::wstring> g_wideLines;
std::vector<std::int32_t> g_categories;

// True only in a running game session: the main menu, the pregame and loading
// screens have no explore or combat mode, and the HUD is not there to own.
bool InGameSession()
{
    const std::optional<dao::GameMode> mode = dao::CClientExoApp::CurrentMode();
    return mode && (*mode == dao::GameMode::Explore || *mode == dao::GameMode::Combat);
}

// Event text is UTF-8 (creature names may be localised); AS2 strings are wide.
std::wstring WideFromUtf8(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
    {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

} // namespace

void Tick()
{
    const std::uint64_t frame = dao::FrameCount();

    if (!g_added)
    {
        if (frame < g_nextAddFrame)
        {
            return;
        }
        g_nextAddFrame = frame + 120; // the GUI system is up from the first main-menu frame
        if (dao::gui::AddScreen(kScreen, kMovie, dao::gui::kLayerHud))
        {
            g_added = true;
            spdlog::info("ui: event log screen added (movie coop_eventlog)");
        }
        return;
    }

    // The log belongs to a running session: show it when the world is up,
    // hide it in the main menu, the pregame and the loading screens.
    const bool inSession = InGameSession();
    if (inSession && !g_shown)
    {
        dao::gui::ShowScreen(kScreen);
        g_shown = true;
        spdlog::info("ui: event log screen shown");
    }
    else if (!inSession && g_shown)
    {
        dao::gui::HideScreen(kScreen);
        g_shown = false;
        spdlog::info("ui: event log screen hidden (no game session)");
    }
    if (!inSession)
    {
        return;
    }

    if (frame < g_nextTickFrame)
    {
        return;
    }
    g_nextTickFrame = frame + 15; // about four times a second

    std::optional<dao::gui::Movie> movie = dao::gui::ScreenMovie(kScreen);
    if (!movie)
    {
        return;
    }

    if (const std::optional<dao::world::ViewSize> view = dao::world::ScreenSize())
    {
        if (view->width != g_lastViewWidth || view->height != g_lastViewHeight)
        {
            movie->SetVariable("_root.viewWidth", dao::gui::Value::Number(view->width));
            movie->SetVariable("_root.viewHeight", dao::gui::Value::Number(view->height));
            g_lastViewWidth = view->width;
            g_lastViewHeight = view->height;
            spdlog::info("ui: event log view {}x{}", view->width, view->height);
        }
    }

    const coop::EventLog &log = coop::EventLog::Get();
    const std::uint32_t revision = log.Revision();
    if (revision == g_lastRevision)
    {
        return;
    }
    g_lastRevision = revision;

    const std::deque<coop::GameEvent> &recent = log.Recent();
    const std::size_t start = recent.size() > kShownEvents ? recent.size() - kShownEvents : 0;
    g_wideLines.clear();
    g_categories.clear();
    for (std::size_t i = start; i < recent.size(); ++i)
    {
        g_wideLines.push_back(WideFromUtf8(recent[i].text));
        g_categories.push_back(static_cast<std::int32_t>(recent[i].category));
    }
    std::vector<const wchar_t *> pointers;
    pointers.reserve(g_wideLines.size());
    for (const std::wstring &line : g_wideLines)
    {
        pointers.push_back(line.c_str());
    }
    if (!pointers.empty())
    {
        movie->SetArray("_root.lines", std::span<const wchar_t *const>(pointers.data(), pointers.size()));
        movie->SetArray("_root.categories", std::span<const std::int32_t>(g_categories.data(), g_categories.size()));
    }
    movie->SetVariable("_root.count", dao::gui::Value::Number(static_cast<double>(pointers.size())));
    movie->SetVariable("_root.revision", dao::gui::Value::Number(static_cast<double>(revision)));
    spdlog::info("ui: event log revision {} ({} line(s))", revision, pointers.size());
}

} // namespace coopui

