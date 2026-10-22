// The event log screen: adds ui/coop_eventlog.swf as a HUD screen once the GUI
// system is up, then pushes EventLog's last events and the view size into it.
#include "EventLogUi.h"

#include "Events.h"

#include "dao/Frame.h"
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
std::uint64_t g_nextAddFrame = 0;
std::uint64_t g_nextTickFrame = 0;
std::uint32_t g_lastRevision = std::uint32_t(-1);
int g_lastViewWidth = -1;
int g_lastViewHeight = -1;
std::vector<std::wstring> g_wideLines;

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
            dao::gui::ShowScreen(kScreen);
            spdlog::info("ui: event log screen added (movie coop_eventlog)");
        }
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
    for (std::size_t i = start; i < recent.size(); ++i)
    {
        g_wideLines.emplace_back(recent[i].text.begin(), recent[i].text.end());
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
    }
    movie->SetVariable("_root.count", dao::gui::Value::Number(static_cast<double>(pointers.size())));
    movie->SetVariable("_root.revision", dao::gui::Value::Number(static_cast<double>(revision)));
    spdlog::info("ui: event log revision {} ({} line(s))", revision, pointers.size());
}

} // namespace coopui

