// dao-coop, milestone 0: prove the chain.
//
// What this is: a DAOLib plugin that loads into Dragon Age: Origins and, once
// a game is loaded, logs the player's controlled creature, the party bar's
// selection, and each party member's area, position and health about once a
// second. It changes nothing in the game. It exists so that the rest of the
// co-op work starts from a verified base: the loader loads us, we can read the
// engine's live state from the game thread, and two instances can both do it.
//
// For automated runs, DAOCOOP_AUTOLOAD=1 makes the plugin load the newest save
// of the character the game offers at the main menu, instead of waiting for a
// mouse. That is test-only behaviour: it is off unless the variable is set, and
// it only ever runs inside the named-instance profile the test rig uses.
//
// What we build toward (see the design notes in the project repo): host
// authority with two game processes, a save-transfer join, puppet players,
// and shared combat. This plugin is the first rung, not the mod.
//
// Log: Documents\BioWare\Dragon Age\daolib\daocoop.log (a named instance has
// its own log folder; a second game in the same folder writes
// daocoop.<pid>.log).
#include "dao/ClientExoApp.h"
#include "dao/Frame.h"
#include "dao/Log.h"
#include "dao/Objects.h"
#include "dao/Party.h"
#include "dao/Plugin.h"
#include "dao/Saves.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace
{

// About once a second at the game's frame rate. Per-frame lines would drown
// the log; per-second lines are enough to see whether the state moves.
constexpr std::uint64_t kFramesBetweenLogs = 60;
constexpr std::uint64_t kFramesBetweenLoadTries = 120;

bool g_autoLoad = false;   // DAOCOOP_AUTOLOAD=1: load the newest save at the menu
bool g_loadRequested = false;

// Reads an environment variable without the deprecated getenv (/W4 /WX at
// DAOLib's settings treats its warning as an error).
std::string Environment(const char *name)
{
    char *value = nullptr;
    std::size_t length = 0;
    std::string result;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr)
    {
        result = value;
        std::free(value);
    }
    return result;
}

const char *ModeName(std::optional<dao::GameMode> mode)
{
    if (!mode)
    {
        return "none";
    }
    switch (*mode)
    {
    case dao::GameMode::Explore:
        return "explore";
    case dao::GameMode::Combat:
        return "combat";
    case dao::GameMode::Loading:
        return "loading";
    case dao::GameMode::Pregame:
        return "pregame";
    case dao::GameMode::CharGen:
        return "chargen";
    default:
        return "other";
    }
}

// One line for a creature: its id, area, position and health. Lookups are by
// id, the same ones the scripts use, so an id the engine no longer knows
// answers null and is skipped.
void LogCreature(dao::CClientExoApp *app, const char *what, dao::ObjectId id)
{
    const dao::CCCreature *creature = app->GetCreatureById(id);
    if (creature == nullptr)
    {
        return;
    }
    const dao::Vector position = creature->Position();
    const std::optional<float> health = creature->Health();
    const std::optional<float> maxHealth = creature->MaxHealth();
    char line[512] = {};
    // spdlog's fmt is checked at compile time; building the line first keeps
    // this readable and avoids the check tripping over the std::optional use.
    std::snprintf(line, sizeof line, "%s 0x%08x: area 0x%08x pos (%.2f, %.2f, %.2f) health %.1f / %.1f team %d", what, id,
                  creature->AreaId(), position.x, position.y, position.z, health.value_or(-1.0F),
                  maxHealth.value_or(-1.0F), creature->TeamId());
    spdlog::info("{}", line);
}

// Test-only: at the main menu, ask the game to load the newest save of the
// character it offers. Used by the two-instance rig so a run needs no mouse.
// The save lives in the named instance's own profile, never the player's.
void TryAutoLoad()
{
    if (!g_autoLoad || g_loadRequested)
    {
        return;
    }
    const std::optional<dao::GameMode> mode = dao::CClientExoApp::CurrentMode();
    if (!mode || (*mode != dao::GameMode::Pregame && *mode != dao::GameMode::CharGen))
    {
        return; // not at a menu
    }
    const std::wstring character = dao::saves::Character();
    if (character.empty())
    {
        return; // the game has not listed the saves yet
    }
    const std::vector<dao::saves::Slot> slots = dao::saves::List(character);
    if (slots.empty())
    {
        spdlog::warn("autoload: character has no saves");
        return;
    }
    const dao::saves::Slot &newest = slots.back();
    g_loadRequested = true;
    spdlog::info("autoload: asking to load the newest save of the character offered (slot {} kind {})", newest.number,
                 dao::saves::Name(newest.kind));
    if (!dao::saves::Load(newest))
    {
        spdlog::warn("autoload: the game refused the load request (a menu may still be up)");
        g_loadRequested = false; // try again later
    }
}

void Frame()
{
    // Test-only autoload: look again every couple of seconds until the game is
    // at a menu that will take a load request.
    static std::uint64_t lastLoadTry = 0;
    const std::uint64_t frame = dao::FrameCount();
    if (frame - lastLoadTry >= kFramesBetweenLoadTries)
    {
        lastLoadTry = frame;
        TryAutoLoad();
    }

    static std::uint64_t lastLogged = 0;
    if (frame - lastLogged < kFramesBetweenLogs)
    {
        return;
    }
    lastLogged = frame;

    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    if (app == nullptr)
    {
        return;
    }
    spdlog::info("---- frame {} mode {} paused {} ----", frame, ModeName(dao::CClientExoApp::CurrentMode()),
                 app->IsPaused());

    const std::vector<dao::ObjectId> selected = dao::party::Selected();
    spdlog::info("party bar: {} selected", selected.size());
    for (const dao::ObjectId id : selected)
    {
        LogCreature(app, "selected", id);
    }
    LogCreature(app, "controlled", dao::party::Controlled());
}

} // namespace

extern "C" __declspec(dllexport) bool __cdecl DAOLib_Load(const dao::LoaderInfo *info)
{
    dao::OpenLog("daocoop");
    if (info == nullptr || info->apiVersion != dao::kPluginApiVersion || !info->knownExecutable)
    {
        spdlog::error("not the loader or the game this was built for: api {} (built for {}), known executable {}",
                      info != nullptr ? info->apiVersion : 0, dao::kPluginApiVersion,
                      info != nullptr ? info->knownExecutable : false);
        return false;
    }

    const std::string instance = Environment("DAOLIB_INSTANCE");
    spdlog::info("dao-coop loaded (plugin API {}), instance \"{}\"", dao::kPluginApiVersion,
                 instance.empty() ? "default" : instance);

    g_autoLoad = !Environment("DAOCOOP_AUTOLOAD").empty() && Environment("DAOCOOP_AUTOLOAD") != "0";
    spdlog::info("autoload {}", g_autoLoad ? "on (DAOCOOP_AUTOLOAD)" : "off");

    dao::saves::OnGameLoaded([](const std::optional<dao::saves::Slot> &loaded) {
        if (loaded)
        {
            spdlog::info("game loaded: slot {} kind {}", loaded->number, dao::saves::Name(loaded->kind));
        }
        else
        {
            spdlog::info("game loaded: a new game");
        }
    });
    dao::OnFrame(&Frame);
    return true;
}
