// dao-coop, milestone 0: prove the chain.
//
// What this is: a DAOLib plugin that loads into Dragon Age: Origins and, once
// a game is loaded, logs the player's controlled creature, the party bar's
// selection, and each party member's area, position and health about once a
// second. It changes nothing in the game. It exists so that the rest of the
// co-op work starts from a verified base: the loader loads us, we can read the
// engine's live state from the game thread, and two instances can both do it.
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

#include <spdlog/spdlog.h>

#include <cstdint>
#include <vector>

namespace
{

// About once a second at the game's frame rate. Per-frame lines would drown
// the log; per-second lines are enough to see whether the state moves.
constexpr std::uint64_t kFramesBetweenLogs = 60;

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
    default:
        return "other";
    }
}

// One line for a creature: its id, area, position and health. Lookups are by
// id, the same ones the scripts use, so an id the engine no longer knows
// answers null and is skipped.
void LogCreature(dao::CClientExoApp *app, const char *what, dao::ObjectId id)
{
    const dao::CCreature *creature = app->GetCreatureById(id);
    if (creature == nullptr)
    {
        return;
    }
    const dao::Vector position = creature->Position();
    const std::optional<float> health = creature->Health();
    const std::optional<float> maxHealth = creature->MaxHealth();
    spdlog::info("{} 0x{:x}: area 0x{:x} pos ({:.2f}, {:.2f}, {:.2f}) health {} / {} team {}", what, id,
                 creature->AreaId(), position.x, position.y, position.z, health.value_or(-1.0F),
                 maxHealth.value_or(-1.0F), creature->TeamId());
}

void Frame()
{
    static std::uint64_t lastLogged = 0;
    const std::uint64_t frame = dao::FrameCount();
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

    spdlog::info("dao-coop loaded (plugin API {})", dao::kPluginApiVersion);
    dao::OnFrame(&Frame);
    return true;
}
