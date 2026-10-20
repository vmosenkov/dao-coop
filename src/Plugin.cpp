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
#include "dao/Messages.h"
#include "dao/Objects.h"
#include "dao/Orders.h"
#include "dao/Party.h"
#include "dao/Plugin.h"
#include "dao/Saves.h"
#include "dao/generated/ScriptApi.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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
std::string g_peerIp = "127.0.0.1";
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

// Milestone 4: two avatars. The host creates a companion and adds it to the
// party before the handoff save; the guest loads that save, so both games have
// the same second party member with the same id. The guest then selects party
// index 1 and each side mirrors the other's controlled creature.
bool g_addCompanion = false;
bool g_companionDone = false;
int g_selectParty = -1;
bool g_selected = false;
std::uint64_t g_worldStartFrame = 0;
dao::ObjectId g_puppetId = dao::kInvalidObject;
std::uint64_t g_puppetApplied = 0;
float g_puppetDistance = 0.0F;

// Milestone 5: the hostile the host keeps in sync. It is spawned before the
// handoff save, so the guest loads a world with the same creature and id.
bool g_combatTest = false;
bool g_hostileSpawned = false;
dao::ObjectId g_hostileId = dao::kInvalidObject;
int g_hostileTicks = 0;
std::uint64_t g_hostileNextFrame = 0;
std::uint64_t g_hostileApplied = 0;
float g_hostileDistance = 0.0F;

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

// ---------------------------------------------------------------------------
// Configuration: daocoop.ini beside the plugin, for players; environment
// variables, for the test rig. An environment variable, when set, wins over
// the file. Keys are flat ("role", "peer", "join", ...); [sections] and
// '#'/';' comments are accepted and ignored as structure.
// ---------------------------------------------------------------------------

std::map<std::string, std::string> g_ini;

std::string Trim(const std::string &text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string Lower(std::string text)
{
    for (char &c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

void LoadIni(const std::filesystem::path &path)
{
    std::ifstream file(path);
    if (!file)
    {
        spdlog::warn("config: no {} - using defaults and environment variables", path.string());
        return;
    }
    std::string line;
    int count = 0;
    while (std::getline(file, line))
    {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '[')
        {
            continue;
        }
        const auto equals = trimmed.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }
        const std::string key = Lower(Trim(trimmed.substr(0, equals)));
        if (!key.empty())
        {
            g_ini[key] = Trim(trimmed.substr(equals + 1));
            ++count;
        }
    }
    spdlog::info("config: {} value(s) from {}", count, path.string());
}

// A setting: the environment variable if set, else the ini value, else "".
std::string Setting(const char *key, const char *envName)
{
    const std::string fromEnv = Environment(envName);
    if (!fromEnv.empty())
    {
        return fromEnv;
    }
    const auto found = g_ini.find(key);
    return found != g_ini.end() ? found->second : std::string{};
}

bool Flag(const char *key, const char *envName)
{
    const std::string value = Lower(Setting(key, envName));
    return !value.empty() && value != "0" && value != "false" && value != "no" && value != "off";
}

std::uint16_t PortSetting(const char *key, const char *envName, std::uint16_t fallback)
{
    const std::string text = Setting(key, envName);
    if (text.empty())
    {
        return fallback;
    }
    const long value = std::strtol(text.c_str(), nullptr, 10);
    return value > 0 && value < 65536 ? static_cast<std::uint16_t>(value) : fallback;
}

// A setting: the environment variable if set, else the ini value, else "".
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

// Milestone 4 host: create a mabari next to the player and add it to the
// party before the handoff save, so the guest loads a world with two party
// members (and the same ids) and can control the second one.
void AddCompanion()
{
    if (!g_addCompanion || g_companionDone)
    {
        return;
    }
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    if (app == nullptr)
    {
        return;
    }
    const dao::ObjectId player = dao::party::Controlled();
    if (player == dao::kInvalidObject)
    {
        return;
    }
    const dao::CCObject *creature = app->GetObjectById(player);
    if (creature == nullptr)
    {
        return;
    }
    const dao::Vector position = creature->Position();
    dao::CScriptLocation location{};
    location.position.x = position.x + 1.5F;
    location.position.y = position.y;
    location.position.z = position.z;
    location.area = creature->AreaId();
    const dao::Resource resource{L"gen00fl_dog.utc"};
    const dao::ObjectId dog =
        dao::script::api::CreateObject(dao::script::constants::OBJECT_TYPE_CREATURE, resource, location);
    g_companionDone = true;
    if (dog == dao::kInvalidObject)
    {
        spdlog::warn("companion: the game made no creature from gen00fl_dog.utc");
        return;
    }
    // AddToParty(creature, player): pops two objects, ignores the second.
    const bool hired = dao::script::Invoke<void>(396, dog, dao::kInvalidObject);
    const dao::script::Array party = dao::script::api::GetPartyList(dao::kInvalidObject);
    spdlog::info("companion: created 0x{:08x} beside the player, AddToParty {}, party list {} member(s)", dog, hired,
                 party.Size());
}

// Milestone 4 guest: take control of another party member (index n) through the
// same message the party bar sends. Its second field is how the key behaves:
// with the add-to-selection key held the message adds to the selection, and
// control follows the selection's first entry. So first try a plain click
// (second field 0, replacing the selection); if that leaves control where it
// was, fall back to toggling the old first member off and adding the new one.
int g_selectPhase = 0;
std::uint64_t g_selectPhaseFrame = 0;

void SendPartyIndex(const char *why, int index, int add)
{
    const std::wstring text =
        L"SelectPartyMemberIndexMessage " + std::to_wstring(index) + L" " + std::to_wstring(add);
    const bool sent = dao::messages::SendBinding(text);
    spdlog::info("select: {} index {} field {} ({})", why, index, add, sent ? "message sent" : "message not sent");
}

void SelectPartyMember()
{
    if (g_selectParty < 0 || g_selected)
    {
        return;
    }
    const dao::script::Array party = dao::script::api::GetPartyList(dao::kInvalidObject);
    if (party.Size() <= static_cast<std::size_t>(g_selectParty))
    {
        return;
    }
    const std::uint64_t frame = dao::FrameCount();
    if (g_selectPhase == 0)
    {
        SendPartyIndex("asking for a click on", g_selectParty, 0);
        g_selectPhase = 1;
        g_selectPhaseFrame = frame;
        return;
    }
    if (g_selectPhase == 1 && frame - g_selectPhaseFrame > 120)
    {
        const dao::ObjectId wanted = party.Object(static_cast<std::size_t>(g_selectParty));
        const dao::ObjectId now = dao::party::Controlled();
        spdlog::info("select: after the click, controlled 0x{:08x} (wanted 0x{:08x})", now, wanted);
        if (now == wanted)
        {
            g_selected = true;
            return;
        }
        SendPartyIndex("toggling off", 0, 1);
        SendPartyIndex("adding", g_selectParty, 1);
        g_selectPhase = 2;
        g_selectPhaseFrame = frame;
        return;
    }
    if (g_selectPhase == 2 && frame - g_selectPhaseFrame > 120)
    {
        const dao::ObjectId wanted = party.Object(static_cast<std::size_t>(g_selectParty));
        const dao::ObjectId now = dao::party::Controlled();
        spdlog::info("select: after the toggle, controlled 0x{:08x} (wanted 0x{:08x})", now, wanted);
        g_selected = true;
    }
}

// Milestone 5 host: put a hostile creature into the world before the handoff
// save, far enough away not to start a fight in either game. The guest gets it
// with the save, and the host's copy is the authority for its health.
void SpawnHostile()
{
    if (!g_combatTest || g_hostileSpawned)
    {
        return;
    }
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    if (app == nullptr)
    {
        return;
    }
    const dao::ObjectId player = dao::party::Controlled();
    if (player == dao::kInvalidObject)
    {
        return;
    }
    const dao::CCObject *creature = app->GetObjectById(player);
    if (creature == nullptr)
    {
        return;
    }
    const dao::Vector position = creature->Position();
    dao::CScriptLocation location{};
    location.position.x = position.x + 25.0F;
    location.position.y = position.y;
    location.position.z = position.z;
    location.area = creature->AreaId();
    const dao::Resource resource{L"ogre_default.utc"};
    const dao::ObjectId hostile =
        dao::script::api::CreateObject(dao::script::constants::OBJECT_TYPE_CREATURE, resource, location);
    g_hostileSpawned = true;
    if (hostile == dao::kInvalidObject)
    {
        spdlog::warn("combat: the game made no creature from ogre_default.utc");
        return;
    }
    g_hostileId = hostile;
    // Keep the spawned ogre out of the fight: the scripts' own AI-off switch
    // (CREATURE_RULES_FLAG0 bit 8) and inactive, so neither game enters combat
    // and the host's save is allowed. Its health and position still sync, which
    // is what milestone 5 is testing.
    dao::script::api::SetLocalInt(hostile, L"CREATURE_RULES_FLAG0", 8);
    dao::script::api::SetObjectActive(hostile, 0);
    spdlog::info("combat: hostile spawned 0x{:08x} from ogre_default.utc (AI off, inactive)", hostile);
}

// Milestone 5 host: damage the hostile on a schedule once the guest is
// connected, so the health channel has something to carry. This is a stand-in
// for a real fight: the host's game is the authority, the channel is what is
// being tested.
void DamageHostileTick()
{
    if (!g_combatTest || g_hostileId == dao::kInvalidObject || g_hostileTicks >= 14)
    {
        return;
    }
    const std::uint64_t frame = dao::FrameCount();
    if (g_net.Received() < 100 || frame < g_hostileNextFrame)
    {
        return;
    }
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    if (app == nullptr)
    {
        return;
    }
    const dao::CCCreature *hostile = app->GetCreatureById(g_hostileId);
    if (hostile == nullptr)
    {
        return;
    }
    const std::optional<float> health = hostile->Health();
    if (!health || *health <= 1.0F)
    {
        g_hostileTicks = 14;
        return;
    }
    const float next = *health - 18.0F;
    hostile->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, next,
                         dao::script::PropertyValue::Current);
    ++g_hostileTicks;
    g_hostileNextFrame = frame + 120;
    spdlog::info("combat: hostile 0x{:08x} health {:.1f} -> {:.1f}", g_hostileId, *health, next);
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
            if (g_role == Role::Host && g_hostileId != dao::kInvalidObject)
            {
                const dao::CCCreature *hostile = app->GetCreatureById(g_hostileId);
                if (hostile != nullptr)
                {
                    const dao::Vector hostilePosition = hostile->Position();
                    packet.hostileId = g_hostileId;
                    packet.hostileX = hostilePosition.x;
                    packet.hostileY = hostilePosition.y;
                    packet.hostileZ = hostilePosition.z;
                    packet.hostileHealth = hostile->Health().value_or(0.0F);
                }
            }
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

    // Apply the other player's creature. With two avatars the remote id is a
    // different party member, and each side mirrors the other player's puppet;
    // in the one-avatar tests both sides control the same creature, and only
    // the guest follows the host's word.
    if (g_haveRemote && g_remote.areaId == local->AreaId() && g_remote.objectId != dao::kInvalidObject)
    {
        if (g_remote.objectId != controlled)
        {
            const dao::CCCreature *puppet = app->GetCreatureById(g_remote.objectId);
            if (puppet != nullptr)
            {
                const dao::Vector puppetPosition = puppet->Position();
                const float dx = g_remote.x - puppetPosition.x;
                const float dy = g_remote.y - puppetPosition.y;
                const float dz = g_remote.z - puppetPosition.z;
                const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
                g_puppetId = g_remote.objectId;
                g_puppetDistance = distance;
                if (distance > kApplyTolerance)
                {
                    dao::Vector target{};
                    target.x = g_remote.x;
                    target.y = g_remote.y;
                    target.z = g_remote.z;
                    dao::script::api::SetPosition(g_remote.objectId, target);
                    ++g_puppetApplied;
                }
            }
        }
        else if (g_role == Role::Guest)
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
    }

    // Guest: mirror the host's hostile creature (health and position). The
    // host is the authority for it; the guest's game displays the result.
    if (g_role == Role::Guest && g_haveRemote && g_remote.hostileId != dao::kInvalidObject)
    {
        const dao::CCCreature *hostile = app->GetCreatureById(g_remote.hostileId);
        if (hostile != nullptr)
        {
            const dao::Vector hostilePosition = hostile->Position();
            const float hx = g_remote.hostileX - hostilePosition.x;
            const float hy = g_remote.hostileY - hostilePosition.y;
            const float hz = g_remote.hostileZ - hostilePosition.z;
            const float hostileDistance = std::sqrt((hx * hx) + (hy * hy) + (hz * hz));
            g_hostileDistance = hostileDistance;
            if (hostileDistance > kApplyTolerance)
            {
                dao::Vector target{};
                target.x = g_remote.hostileX;
                target.y = g_remote.hostileY;
                target.z = g_remote.hostileZ;
                dao::script::api::SetPosition(g_remote.hostileId, target);
            }
            const std::optional<float> localHealth = hostile->Health();
            if (localHealth && std::fabs(*localHealth - g_remote.hostileHealth) > 1.0F)
            {
                hostile->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, g_remote.hostileHealth,
                                     dao::script::PropertyValue::Current);
                ++g_hostileApplied;
            }
        }
    }

    // Either player can drive their own creature for tests (host by default,
    // the guest once it has selected its own party member).
    if (g_drive && dao::FrameCount() % kFramesBetweenMoves == 0)
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
        char line[640] = {};
        if (g_haveRemote)
        {
            const float dx = g_remote.x - localPosition.x;
            const float dy = g_remote.y - localPosition.y;
            const float dz = g_remote.z - localPosition.z;
            const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
            float localHostileHealth = 0.0F;
            dao::ObjectId localHostile = dao::kInvalidObject;
            if (g_hostileId != dao::kInvalidObject)
            {
                if (const dao::CCCreature *hostile = app->GetCreatureById(g_hostileId))
                {
                    localHostile = g_hostileId;
                    localHostileHealth = hostile->Health().value_or(0.0F);
                }
            }
            std::snprintf(line, sizeof line,
                          "net role=%s local=0x%08x remote=0x%08x puppet=0x%08x local (%.2f, %.2f, %.2f) area 0x%08x "
                          "remote (%.2f, %.2f, %.2f) area 0x%08x distance %.2f applied %llu puppet_applied %llu "
                          "puppet_distance %.2f hostile=0x%08x hostile_health %.1f local_hostile=0x%08x "
                          "local_hostile_health %.1f hostile_applied %llu hostile_distance %.2f sent %llu recv %llu "
                          "dropped %llu",
                          g_role == Role::Host ? "host" : "guest", controlled, g_remote.objectId, g_puppetId,
                          localPosition.x, localPosition.y, localPosition.z, local->AreaId(), g_remote.x, g_remote.y,
                          g_remote.z, g_remote.areaId, distance, static_cast<unsigned long long>(g_applied),
                          static_cast<unsigned long long>(g_puppetApplied), g_puppetDistance, g_remote.hostileId,
                          g_remote.hostileHealth, localHostile, localHostileHealth,
                          static_cast<unsigned long long>(g_hostileApplied), g_hostileDistance,
                          static_cast<unsigned long long>(g_net.Sent()), static_cast<unsigned long long>(g_net.Received()),
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

    // Milestone 4: once in the world, the host adds a companion (before the
    // handoff save) and the guest takes control of its own party member.
    if (InWorld())
    {
        if (g_worldStartFrame == 0)
        {
            g_worldStartFrame = frame;
        }
        if (frame - g_worldStartFrame > 60)
        {
            SpawnHostile();
        }
        if (frame - g_worldStartFrame > 120)
        {
            AddCompanion();
            SelectPartyMember();
            DamageHostileTick();
        }
    }
    else
    {
        g_worldStartFrame = 0;
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

    const std::filesystem::path ini = std::filesystem::path(info->pluginPath).parent_path() / L"daocoop.ini";
    LoadIni(ini);

    const std::string instance = Environment("DAOLIB_INSTANCE");
    spdlog::info("dao-coop loaded (plugin API {}), instance \"{}\"", dao::kPluginApiVersion,
                 instance.empty() ? "default" : instance);

    g_autoLoad = Flag("autoload", "DAOCOOP_AUTOLOAD");
    spdlog::info("autoload {}", g_autoLoad ? "on" : "off");

    const std::string role = Lower(Setting("role", "DAOCOOP_ROLE"));
    if (role == "host")
    {
        g_role = Role::Host;
    }
    else if (role == "guest")
    {
        g_role = Role::Guest;
    }
    g_drive = Flag("drive", "DAOCOOP_DRIVE");
    g_hostPort = PortSetting("host_port", "DAOCOOP_HOST_PORT", 45830);
    g_guestPort = PortSetting("guest_port", "DAOCOOP_GUEST_PORT", 45831);
    const std::string peerSetting = Setting("peer", "DAOCOOP_PEERIP");
    g_peerIp = peerSetting.empty() ? "127.0.0.1" : peerSetting;

    if (g_role != Role::None)
    {
        const std::uint16_t listenPort = g_role == Role::Host ? g_hostPort : g_guestPort;
        const std::uint16_t peerPort = g_role == Role::Host ? g_guestPort : g_hostPort;
        if (g_net.Open(listenPort, peerPort, g_peerIp.c_str()))
        {
            spdlog::info("net: role {} listening on 0.0.0.0:{} sending to {}:{}", role, listenPort,
                         g_net.PeerAddress(), peerPort);
        }
        else
        {
            spdlog::error("net: could not open UDP socket on port {} (is the port taken?)", listenPort);
            g_role = Role::None;
        }
    }
    spdlog::info("drive {}", g_drive ? "on" : "off");

    g_addCompanion = g_role == Role::Host && Flag("add_companion", "DAOCOOP_ADD_COMPANION");
    const std::string selectParty = Setting("select_party", "DAOCOOP_SELECT_PARTY");
    g_selectParty = selectParty.empty() ? -1 : std::atoi(selectParty.c_str());
    if (g_addCompanion)
    {
        spdlog::info("companion on: the host creates and hires a mabari before the handoff save");
    }
    if (g_selectParty >= 0)
    {
        spdlog::info("select on: this player takes party index {}", g_selectParty);
    }

    g_combatTest = g_role == Role::Host && Flag("combat_test", "DAOCOOP_COMBAT_TEST");
    if (g_combatTest)
    {
        spdlog::info("combat on: the host spawns a hostile before the handoff save and keeps its health in sync");
    }
    dao::party::OnControlChanged([](dao::ObjectId before, dao::ObjectId now) {
        spdlog::info("control: 0x{:08x} -> 0x{:08x}", before, now);
    });

    g_handoff = g_role == Role::Host && Flag("handoff", "DAOCOOP_HANDOFF");
    g_join = g_role == Role::Guest && Flag("join", "DAOCOOP_JOIN");
    g_tcpPort = PortSetting("tcp_port", "DAOCOOP_TCP_PORT", 45832);
    // The save transfer talks to the host: its own "host" setting, or the peer.
    const std::string hostSetting = Setting("host", "DAOCOOP_HOSTIP");
    g_hostIp = hostSetting.empty() ? g_peerIp : hostSetting;
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
