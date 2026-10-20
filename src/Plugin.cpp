// dao-coop, milestones 0-2.
//
// Milestone 0 (proven 2026-10-20): the plugin loads through DAOLib and reads
// the live game state from the game thread. Milestone 1 (proven): two named
// instances run side by side, load the same save and agree on object ids.
//
// Milestone 2 (this revision): a host and a guest exchange their controlled
// creature's state over UDP loopback 20 times a second. The guest treats the
// host's word as law and applies the received position to its copy of the same
// creature with the engine's own SetPosition command, so the two games show
// the same creature in the same place. The host has a test-only drive mode
// (DAOCOOP_DRIVE=1) that walks its character between waypoints, so a test can
// run without a person at the keyboard.
//
// What this is not yet: a join handshake (the two games are both started on
// the same save by the test rig), movement intent sync, combat, or area
// transitions. Those are milestones 3+.
//
// Environment variables (all optional; the test rig sets them):
//   DAOCOOP_AUTOLOAD=1     load the newest save at the main menu
//   DAOCOOP_ROLE=host      host: sends its state, may drive its character
//   DAOCOOP_ROLE=guest     guest: sends its state and applies the host's
//   DAOCOOP_DRIVE=1        host only: walk between waypoints for testing
//   DAOCOOP_HOST_PORT      default 45830
//   DAOCOOP_GUEST_PORT     default 45831
#include "SaveTransfer.h"
#include "Transport.h"

#include "dao/ClientExoApp.h"
#include "dao/Frame.h"
#include "dao/Log.h"
#include "dao/Objects.h"
#include "dao/Orders.h"
#include "dao/Party.h"
#include "dao/Plugin.h"
#include "dao/Saves.h"
#include "dao/generated/ScriptApi.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr std::uint64_t kFramesBetweenLogs = 60;     // about a second
constexpr std::uint64_t kFramesBetweenLoadTries = 120;
constexpr std::uint64_t kFramesBetweenSends = 3;     // about 20 Hz
constexpr std::uint64_t kFramesBetweenMoves = 300;   // about five seconds
constexpr float kApplyTolerance = 0.15F;             // metres
constexpr float kDriveRadius = 6.0F;                 // metres around the anchor

enum class Role
{
    None,
    Host,
    Guest,
};

bool g_autoLoad = false;
bool g_loadRequested = false;
Role g_role = Role::None;
bool g_drive = false;
coop::Transport g_net;
std::uint16_t g_hostPort = 45830;
std::uint16_t g_guestPort = 45831;
std::uint32_t g_sequence = 0;
coop::StatePacket g_remote{};
bool g_haveRemote = false;
std::uint64_t g_applied = 0;
bool g_anchorSet = false;
dao::Vector g_anchor{};

// Milestone 3: the save-transfer join.
bool g_handoff = false;          // host: save and serve the slot
bool g_join = false;             // guest: receive the slot and load it
std::uint16_t g_tcpPort = 45832;
std::string g_hostIp = "127.0.0.1";
std::uint64_t g_handoffWorldFrame = 0;
bool g_handoffRequested = false;
bool g_handoffSent = false;
std::uint64_t g_nextJoinTryFrame = 0;
std::atomic<bool> g_receiveInFlight{false};
std::atomic<bool> g_filesReady{false};
std::vector<coop::FileBlob> g_receivedFiles;
std::mutex g_receivedMutex;
bool g_joinFilesWritten = false;
bool g_joinLoadRequested = false;
std::uint64_t g_nextLoadTryFrame = 0;
// The host save uses a slot of its own, well away from the player's.
constexpr std::int32_t kHandoffSlot = 91;
constexpr std::uint64_t kHandoffDelayFrames = 480; // about eight seconds in the world

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

std::uint16_t EnvironmentPort(const char *name, std::uint16_t fallback)
{
    const std::string text = Environment(name);
    if (text.empty())
    {
        return fallback;
    }
    const long value = std::strtol(text.c_str(), nullptr, 10);
    return value > 0 && value < 65536 ? static_cast<std::uint16_t>(value) : fallback;
}

// The mode's number and, when it is one the engine names, its name. The
// number matters in the logs: an "other" mode hid a stuck intro movie once
// (the host waited three minutes because its window was never foreground).
std::string ModeText(std::optional<dao::GameMode> mode)
{
    if (!mode)
    {
        return "none";
    }
    const char *name = "other";
    switch (*mode)
    {
    case dao::GameMode::Explore:
        name = "explore";
        break;
    case dao::GameMode::Combat:
        name = "combat";
        break;
    case dao::GameMode::Gui:
        name = "gui";
        break;
    case dao::GameMode::Dialog:
        name = "dialog";
        break;
    case dao::GameMode::Cutscene:
        name = "cutscene";
        break;
    case dao::GameMode::Pregame:
        name = "pregame";
        break;
    case dao::GameMode::Loading:
        name = "loading";
        break;
    case dao::GameMode::Movie:
        name = "movie";
        break;
    case dao::GameMode::CharGen:
        name = "chargen";
        break;
    default:
        break;
    }
    char text[64] = {};
    std::snprintf(text, sizeof text, "%d (%s)", static_cast<int>(*mode), name);
    return text;
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

bool InWorld()
{
    const std::optional<dao::GameMode> mode = dao::CClientExoApp::CurrentMode();
    return mode && (*mode == dao::GameMode::Explore || *mode == dao::GameMode::Combat);
}

// The local player's creature as this game sees it.
bool LocalState(coop::StatePacket &packet)
{
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    if (app == nullptr)
    {
        return false;
    }
    const dao::ObjectId controlled = dao::party::Controlled();
    if (controlled == dao::kInvalidObject)
    {
        return false;
    }
    const dao::CCCreature *creature = app->GetCreatureById(controlled);
    if (creature == nullptr)
    {
        return false;
    }
    const dao::Vector position = creature->Position();
    packet.sequence = ++g_sequence;
    packet.objectId = controlled;
    packet.areaId = creature->AreaId();
    packet.x = position.x;
    packet.y = position.y;
    packet.z = position.z;
    packet.health = creature->Health().value_or(-1.0F);
    return true;
}

// Test-only: at a menu, ask the game to load the newest save of the character
// it offers. Used by the two-instance rig so a run needs no mouse. The game
// refuses cleanly when it is not ready, so the mode is not a gate; only a
// successful request stops the retries. The save lives in the named
// instance's own profile, never the player's.
void TryAutoLoad()
{
    if (!g_autoLoad || g_loadRequested)
    {
        return;
    }
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    if (app == nullptr)
    {
        return;
    }
    const std::wstring character = dao::saves::Character();
    if (character.empty())
    {
        return;
    }
    const std::vector<dao::saves::Slot> slots = dao::saves::List(character);
    if (slots.empty())
    {
        spdlog::warn("autoload: character has no saves");
        return;
    }
    const dao::saves::Slot &newest = slots.back();
    g_loadRequested = true;
    spdlog::info("autoload: asking to load the newest save of the character offered (slot {} kind {}), mode {}",
                 newest.number, dao::saves::Name(newest.kind),
                 ModeText(dao::CClientExoApp::CurrentMode()));
    if (dao::saves::Load(newest))
    {
        spdlog::info("autoload: the game took the load request");
        return;
    }
    static int failures = 0;
    ++failures;
    if (failures <= 3 || failures % 10 == 0)
    {
        spdlog::info("autoload: the game refused the load request ({} time(s)); will retry, mode {}", failures,
                     ModeText(dao::CClientExoApp::CurrentMode()));
    }
    g_loadRequested = false;
}

// Host side of the join: send the slot folder that was just written. The
// waiting and the bytes live on a worker thread; the game keeps playing.
void StartHandoffSend(const std::filesystem::path &folder)
{
    std::thread([folder]() {
        std::string summary;
        std::string error;
        if (coop::SendFolder(folder, g_tcpPort, summary, error))
        {
            spdlog::info("handoff: sent the slot folder ({}): {}", summary, folder.string());
        }
        else
        {
            spdlog::warn("handoff: the transfer failed: {}", error);
        }
    }).detach();
}

// Guest side of the join: connect to the host and receive the slot's files.
void StartGuestReceive()
{
    if (g_receiveInFlight.exchange(true))
    {
        return;
    }
    std::thread([]() {
        std::vector<coop::FileBlob> files;
        std::string error;
        if (coop::ReceiveFolder(g_hostIp, g_tcpPort, files, error))
        {
            {
                const std::lock_guard<std::mutex> lock(g_receivedMutex);
                g_receivedFiles = std::move(files);
            }
            g_filesReady = true;
            spdlog::info("join: received the host's slot over TCP ({} files)", g_receivedFiles.size());
        }
        else
        {
            spdlog::warn("join: could not receive the slot yet: {}", error);
        }
        g_receiveInFlight = false;
    }).detach();
}

std::wstring FirstCharacter()
{
    const std::wstring chosen = dao::saves::Character();
    if (!chosen.empty())
    {
        return chosen;
    }
    const std::vector<std::wstring> characters = dao::saves::Characters();
    return characters.empty() ? std::wstring{} : characters.front();
}

// Guest side: once the files are in memory, write them into a slot folder of
// the guest's own profile and ask the game to load it. The folder is the game's
// own path for a manual slot (Slot::Folder), so nothing here hard-codes the
// player's profile layout.
void TryGuestJoin()
{
    if (!g_filesReady)
    {
        return;
    }
    if (!g_joinFilesWritten)
    {
        std::vector<coop::FileBlob> files;
        {
            const std::lock_guard<std::mutex> lock(g_receivedMutex);
            files = std::move(g_receivedFiles);
            g_receivedFiles.clear();
        }
        if (files.empty())
        {
            g_filesReady = false;
            return;
        }
        const std::wstring character = FirstCharacter();
        if (character.empty())
        {
            spdlog::warn("join: the game knows no character to put the save under yet; will retry");
            g_filesReady = false;
            const std::lock_guard<std::mutex> lock(g_receivedMutex);
            g_receivedFiles = std::move(files);
            return;
        }
        const dao::saves::Slot slot{character, kHandoffSlot, dao::saves::Kind::Manual};
        const std::filesystem::path folder = slot.Folder();
        std::error_code code;
        std::filesystem::create_directories(folder, code);
        int written = 0;
        for (const coop::FileBlob &file : files)
        {
            std::ofstream out(folder / file.name, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                spdlog::warn("join: could not write {} in {}", file.name, folder.string());
                continue;
            }
            out.write(reinterpret_cast<const char *>(file.data.data()), static_cast<std::streamsize>(file.data.size()));
            ++written;
        }
        spdlog::info("join: wrote {} file(s) into {}", written, folder.string());
        g_joinFilesWritten = true;
    }

    if (g_joinLoadRequested || dao::FrameCount() < g_nextLoadTryFrame)
    {
        return;
    }
    g_nextLoadTryFrame = dao::FrameCount() + 120;
    const std::wstring character = FirstCharacter();
    if (character.empty())
    {
        return;
    }
    const dao::saves::Slot slot{character, kHandoffSlot, dao::saves::Kind::Manual};
    if (!slot.Exists())
    {
        spdlog::warn("join: the written slot is not there yet ({})", slot.Folder().string());
        return;
    }
    if (dao::saves::Load(slot))
    {
        g_joinLoadRequested = true;
        spdlog::info("join: asked the game to load the host's save (slot {})", kHandoffSlot);
    }
    else
    {
        spdlog::info("join: the game refused the load for now; will retry");
    }
}

// Milestone 2: one frame's worth of co-op.
//  - both roles send their controlled creature's state about 20 times a second
//  - both drain the socket every frame
//  - the guest applies the host's position to its copy of the same creature
//  - the host, when asked, walks its character between waypoints so there is
//    movement to sync without a human at the controls
void NetFrame()
{
    if (g_role == Role::None || !g_net.IsOpen() || !InWorld())
    {
        return;
    }
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    if (app == nullptr)
    {
        return;
    }
    const dao::ObjectId controlled = dao::party::Controlled();
    if (controlled == dao::kInvalidObject)
    {
        return;
    }
    const dao::CCCreature *local = app->GetCreatureById(controlled);
    if (local == nullptr)
    {
        return;
    }

    // Send.
    if (dao::FrameCount() % kFramesBetweenSends == 0)
    {
        coop::StatePacket packet{};
        if (LocalState(packet))
        {
            g_net.Send(packet);
        }
    }

    // Receive everything waiting; keep the newest.
    coop::StatePacket incoming{};
    for (int i = 0; i < 32 && g_net.Receive(incoming); ++i)
    {
        if (!g_haveRemote || incoming.sequence > g_remote.sequence)
        {
            g_remote = incoming;
            g_haveRemote = true;
        }
    }

    const dao::Vector localPosition = local->Position();
    if (!g_anchorSet)
    {
        g_anchor = localPosition;
        g_anchorSet = true;
    }

    // Guest: apply the host's word.
    if (g_role == Role::Guest && g_haveRemote && g_remote.objectId == controlled &&
        g_remote.areaId == local->AreaId())
    {
        const float dx = g_remote.x - localPosition.x;
        const float dy = g_remote.y - localPosition.y;
        const float dz = g_remote.z - localPosition.z;
        if (std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) > kApplyTolerance)
        {
            dao::Vector target{};
            target.x = g_remote.x;
            target.y = g_remote.y;
            target.z = g_remote.z;
            dao::script::api::SetPosition(controlled, target);
            ++g_applied;
        }
    }

    // Host: walk between waypoints on a circle around the anchor, for tests.
    if (g_role == Role::Host && g_drive && dao::FrameCount() % kFramesBetweenMoves == 0)
    {
        const float angle = static_cast<float>(dao::FrameCount() / kFramesBetweenMoves) * 1.1F;
        dao::Vector target{};
        target.x = g_anchor.x + (std::cos(angle) * kDriveRadius);
        target.y = g_anchor.y + (std::sin(angle) * kDriveRadius);
        target.z = g_anchor.z;
        if (dao::orders::MoveTo(controlled, target))
        {
            spdlog::info("drive: move ordered to ({:.2f}, {:.2f}, {:.2f})", target.x, target.y, target.z);
        }
    }

    // Once a second: the numbers a test reads.
    if (dao::FrameCount() % kFramesBetweenLogs == 0)
    {
        char line[512] = {};
        if (g_haveRemote)
        {
            const float dx = g_remote.x - localPosition.x;
            const float dy = g_remote.y - localPosition.y;
            const float dz = g_remote.z - localPosition.z;
            const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
            std::snprintf(line, sizeof line,
                          "net role=%s local (%.2f, %.2f, %.2f) area 0x%08x remote (%.2f, %.2f, %.2f) area 0x%08x "
                          "distance %.2f applied %llu sent %llu recv %llu dropped %llu",
                          g_role == Role::Host ? "host" : "guest", localPosition.x, localPosition.y, localPosition.z,
                          local->AreaId(), g_remote.x, g_remote.y, g_remote.z, g_remote.areaId, distance,
                          static_cast<unsigned long long>(g_applied), static_cast<unsigned long long>(g_net.Sent()),
                          static_cast<unsigned long long>(g_net.Received()),
                          static_cast<unsigned long long>(g_net.Dropped()));
        }
        else
        {
            std::snprintf(line, sizeof line,
                          "net role=%s local (%.2f, %.2f, %.2f) area 0x%08x no remote packet yet sent %llu recv %llu",
                          g_role == Role::Host ? "host" : "guest", localPosition.x, localPosition.y, localPosition.z,
                          local->AreaId(), static_cast<unsigned long long>(g_net.Sent()),
                          static_cast<unsigned long long>(g_net.Received()));
        }
        spdlog::info("{}", line);
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

    // Milestone 3 host: after a few seconds in the world, write a save for the
    // guest and serve its slot folder.
    if (g_role == Role::Host && g_handoff && !g_handoffSent && !g_handoffRequested && InWorld())
    {
        if (g_handoffWorldFrame == 0)
        {
            g_handoffWorldFrame = frame;
        }
        if (frame - g_handoffWorldFrame > kHandoffDelayFrames && frame % 60 == 0)
        {
            if (dao::saves::Save(dao::saves::Kind::Manual, kHandoffSlot, L"coop-handoff"))
            {
                g_handoffRequested = true;
                spdlog::info("handoff: save requested for the guest (slot {})", kHandoffSlot);
            }
        }
    }

    // Milestone 3 guest: receive the host's slot, write it into this profile,
    // then load it.
    if (g_role == Role::Guest && g_join)
    {
        if (!g_filesReady && !g_receiveInFlight && frame >= g_nextJoinTryFrame)
        {
            g_nextJoinTryFrame = frame + 300; // retry about every five seconds
            StartGuestReceive();
        }
        TryGuestJoin();
    }

    NetFrame();

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
    spdlog::info("---- frame {} mode {} paused {} ----", frame, ModeText(dao::CClientExoApp::CurrentMode()),
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

    const std::string role = Environment("DAOCOOP_ROLE");
    if (role == "host")
    {
        g_role = Role::Host;
    }
    else if (role == "guest")
    {
        g_role = Role::Guest;
    }
    g_drive = g_role == Role::Host && !Environment("DAOCOOP_DRIVE").empty() && Environment("DAOCOOP_DRIVE") != "0";
    g_hostPort = EnvironmentPort("DAOCOOP_HOST_PORT", 45830);
    g_guestPort = EnvironmentPort("DAOCOOP_GUEST_PORT", 45831);

    if (g_role != Role::None)
    {
        const std::uint16_t listenPort = g_role == Role::Host ? g_hostPort : g_guestPort;
        const std::uint16_t peerPort = g_role == Role::Host ? g_guestPort : g_hostPort;
        if (g_net.Open(listenPort, peerPort))
        {
            spdlog::info("net: role {} listening on 127.0.0.1:{} sending to 127.0.0.1:{}", role, listenPort, peerPort);
        }
        else
        {
            spdlog::error("net: could not open UDP socket on 127.0.0.1:{} (is the port taken?)", listenPort);
            g_role = Role::None;
        }
    }
    spdlog::info("drive {}", g_drive ? "on (DAOCOOP_DRIVE)" : "off");

    g_handoff = g_role == Role::Host && !Environment("DAOCOOP_HANDOFF").empty() && Environment("DAOCOOP_HANDOFF") != "0";
    g_join = g_role == Role::Guest && !Environment("DAOCOOP_JOIN").empty() && Environment("DAOCOOP_JOIN") != "0";
    g_tcpPort = EnvironmentPort("DAOCOOP_TCP_PORT", 45832);
    const std::string hostIp = Environment("DAOCOOP_HOSTIP");
    g_hostIp = hostIp.empty() ? "127.0.0.1" : hostIp;
    if (g_handoff)
    {
        spdlog::info("handoff on: after a few seconds in the world, save slot {} and serve its folder on TCP {}",
                     kHandoffSlot, g_tcpPort);
    }
    if (g_join)
    {
        spdlog::info("join on: look for a host's save at {}:{}", g_hostIp, g_tcpPort);
    }

    // The host's save just written: hand its slot folder to the waiting guest.
    dao::saves::OnSaved([](const dao::saves::Slot &saved) {
        if (g_role == Role::Host && g_handoff && !g_handoffSent && saved.kind == dao::saves::Kind::Manual &&
            saved.number == kHandoffSlot)
        {
            g_handoffSent = true;
            spdlog::info("handoff: slot {} written; serving its folder", saved.number);
            StartHandoffSend(saved.Folder());
        }
    });

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
