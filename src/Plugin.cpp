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
#include "EventLogUi.h"
#include "Events.h"
#include "SaveTransfer.h"
#include "Transport.h"

#include <Windows.h>

#include "dao/Animation.h"
#include "dao/Audio.h"
#include "dao/Clock.h"
#include "dao/ClientExoApp.h"
#include "dao/CommandEvents.h"
#include "dao/Frame.h"
#include "dao/Log.h"
#include "dao/Messages.h"
#include "dao/Objects.h"
#include "dao/Orders.h"
#include "dao/Party.h"
#include "dao/Plugin.h"
#include "dao/Saves.h"
#include "dao/SubActions.h"
#include "dao/generated/ScriptApi.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstring>
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
// A gap this small is walked with a real move order, so the mirrored creature's
// own movement controller plays its run animation; longer gaps are teleported.
constexpr float kPuppetWalkRange = 3.0F;             // metres
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
bool g_driveAway = false; // test: host and guest walk in opposite directions
coop::Transport g_net;
std::uint16_t g_hostPort = 45830;
std::uint16_t g_guestPort = 45831;
std::uint32_t g_sequence = 0;
std::uint32_t g_eventSequence = 0; // ids of the events sent to the other player
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
std::uint64_t g_puppetMoveFrame = 0;
std::uint64_t g_puppetMoveStart = 0;
int g_puppetMoves = 0;            // move orders sent in this walk (0 = not walking)
int g_puppetWalks = 0;            // move orders sent in total, for the net log
float g_puppetWalkBest = 1.0e9F;  // closest the walk has come, to spot a wall

// Milestone 5: the hostile the host keeps in sync. It is spawned before the
// handoff save, so the guest loads a world with the same creature and id.
bool g_combatTest = false;
bool g_hostileSpawned = false;
dao::ObjectId g_hostileId = dao::kInvalidObject;
int g_hostileTicks = 0;
std::uint64_t g_hostileNextFrame = 0;
std::uint64_t g_hostileApplied = 0;
float g_hostileDistance = 0.0F;

// A visible demonstration fight: the ogre spawns close, and once both games
// are in the world its AI is turned back on and both players' creatures are
// ordered to attack. DAO's own combat code resolves the fight.
bool g_demoFight = false;
bool g_demoStarted = false;
bool g_demoOrdered = false;
// How much health the demonstration party gets; 0 or less leaves it as the
// game made it, so a death test can let the host fall while the guest lives.
float g_demoPartyHealth = 5000.0F;
float g_demoOgreHealth = 1500.0F;
// When to tell the host player's creature to attack again (it gets no input in
// the demonstration, so a single order is not enough).
std::uint64_t g_demoNextAttackOrder = 0;

// Session event log: everything the player should see goes to the log and to
// the in-game panel; the plugin shows no separate floating message.
void Notify(coop::EventCategory category, const std::string &text)
{
    coop::EventLog::Get().Add(category, text);
    spdlog::info("event: [{}] {}", coop::EventCategoryName(category), text);
    // Send the line to the other game: the two players should read the same
    // session log. Fire and forget; a lost event is only a missing line.
    if (g_net.IsOpen() && g_role != Role::None)
    {
        coop::EventPacket packet{};
        packet.category = static_cast<std::uint8_t>(category);
        packet.sequence = ++g_eventSequence;
        std::snprintf(packet.text, sizeof packet.text, "%s", text.c_str());
        g_net.Send(packet);
    }
}

// The engine's name for a creature ("Aedan", "Dog", ...), as UTF-8 for the log.
std::string CreatureName(dao::ObjectId id)
{
    if (id == dao::kInvalidObject)
    {
        return {};
    }
    const std::wstring wide = dao::script::api::GetName(id);
    if (wide.empty())
    {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

// Session bookkeeping for those events.
bool g_sawRemote = false;
bool g_connectionLost = false;
bool g_sharedCreatureWarned = false; // the guest ended up on the host's own creature
std::uint64_t g_lastRemoteFrame = 0;
std::uint64_t g_worldReloadFrames = 0;
bool g_worldReloadNoticed = false;
bool g_firstLoadSeen = false;
float g_lastHostileHealth = -1.0F;
bool g_hostileDeathAnnounced = false;
// Combat events: one "attacks" line per creature per 1.5 s, and one
// "combat started"/"combat ended" transition for the alive-hostile count.
std::map<dao::ObjectId, std::uint64_t> g_lastAttackLog;
bool g_combatSeen = false;
std::uint64_t g_lastFightFrame = 0; // the last frame a living hostile was seen
// Diagnostics off by default (0.6.0 callbacks): log the base-layer animations and
// sound events the game starts for the creatures this session mirrors.
bool g_traceAnim = false;
bool g_traceSound = false;
std::uint64_t g_animEvents = 0;
std::uint64_t g_soundEvents = 0;
// Test hook: after the guest has joined, load another save on the host to
// exercise the guest's automatic rejoin.
bool g_testReload = false;
bool g_testReloaded = false;
std::uint64_t g_testReloadAt = 0;
// Test hook: with the host's avatar down, defeat the hostile so the
// revive-after-the-fight path can be exercised without a real winner.
bool g_testVictory = false;
bool g_testVictoryDone = false;
std::uint64_t g_testVictoryAt = 0;
// Test hook: kill the host player on purpose when the fight starts, so the
// revive path can be checked without waiting for the ogre to win.
bool g_testKillPlayer = false;
bool g_testKillPlayerDone = false;
std::uint64_t g_testKillPlayerAt = 0;
// Party-member selection phases (guest), reset when the world is replaced.
int g_selectPhase = 0;
std::uint64_t g_selectPhaseFrame = 0;
// The player's own creature, fixed when the world starts: if the game moves
// control to another party member (the avatar fell), the network keeps naming
// the avatar, not whoever the game selected.
dao::ObjectId g_myAvatar = dao::kInvalidObject;
// A fight is in progress once a hostile was mirrored; after it ends the fallen
// party members stand up and the host's player gets their creature back.
bool g_fightSeen = false;
// The host mirrors what each hostile is attacking, so the guest's copy can
// fight the same fight instead of standing still.
std::map<dao::ObjectId, std::pair<dao::ObjectId, std::uint64_t>> g_hostileTargets;
// Every attack start, counted per creature on the host, and the last one the
// guest has mirrored.
std::map<dao::ObjectId, std::uint32_t> g_hostileAttackSeq;
std::map<dao::ObjectId, std::uint32_t> g_seenAttackSeq;
// Commands a creature is running: a creature in a fight must not be teleported
// by the position mirror every frame, or its blow never lands. The skip is
// short (the swing window) so the two games do not drift apart.
std::map<dao::ObjectId, int> g_busyCreatures;
std::map<dao::ObjectId, std::uint64_t> g_lastAttackStart;
// Pause synchronisation: the two games follow each other's tactical pause.
std::uint64_t g_pauseNextFrame = 0;
bool g_testPause = false;
bool g_testPauseDone = false;
bool g_handoffWritten = false; // the handoff save is on disk

// Combat arbitration: the guest turns its own attack order into a request and
// sends it to the host, which validates it and executes the attack in its
// world; the host's answer is logged (the outcome arrives as hostile state).
struct PendingAttack
{
    bool valid = false;
    std::uint32_t requestId = 0;
    dao::ObjectId attacker = dao::kInvalidObject;
    dao::ObjectId target = dao::kInvalidObject;
};

PendingAttack g_pendingAttack;
std::uint32_t g_attackRequestSeq = 0;
std::uint32_t g_lastGuestRequest = 0;
std::uint64_t g_attackRequestsAccepted = 0;
std::uint64_t g_attackRequestsRefused = 0;
// A request that arrives before the host has seen the guest's first state
// packet is refused as bad-attacker; the guest repeats it a couple of times
// instead of losing the player's action.
bool g_attackAwaiting = false;
int g_attackResends = 0;
std::uint32_t g_attackLastRequest = 0;
dao::ObjectId g_attackLastAttacker = dao::kInvalidObject;
dao::ObjectId g_attackLastTarget = dao::kInvalidObject;

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
    const dao::ObjectId avatar = g_myAvatar != dao::kInvalidObject ? g_myAvatar : dao::party::Controlled();
    if (avatar == dao::kInvalidObject)
    {
        return false;
    }
    const dao::CCCreature *creature = app->GetCreatureById(avatar);
    if (creature == nullptr)
    {
        return false;
    }
    const dao::Vector position = creature->Position();
    packet.sequence = ++g_sequence;
    packet.objectId = avatar;
    packet.areaId = creature->AreaId();
    packet.x = position.x;
    packet.y = position.y;
    packet.z = position.z;
    packet.health = creature->Health().value_or(-1.0F);
    packet.maxHealth = creature->MaxHealth().value_or(-1.0F);
    packet.flags = app->IsPaused() ? coop::kPaused : 0;
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

// The host loaded another save during a session: save the new world again and
// serve it, so the guest can reconnect to it.
void ResetHandoffForResync()
{
    g_handoffSent = false;
    g_handoffWritten = false;
    g_handoffRequested = false;
    g_handoffWorldFrame = 0;
    g_myAvatar = dao::kInvalidObject;
    g_hostileId = dao::kInvalidObject;
    g_hostileSpawned = false;
    g_hostileTicks = 0;
    g_demoStarted = false;
    g_demoOrdered = false;
    g_fightSeen = false;
    spdlog::info("handoff: the host loaded another save; saving and serving the new world for the guest");
}

// The guest saw that flag: forget the old world and go through the join again.
void ResetGuestJoinForResync()
{
    g_filesReady = false;
    g_joinFilesWritten = false;
    g_joinLoadRequested = false;
    g_nextLoadTryFrame = 0;
    g_nextJoinTryFrame = 0;
    g_myAvatar = dao::kInvalidObject;
    g_selected = false;
    g_selectPhase = 0;
    g_selectPhaseFrame = 0;
    g_pendingAttack.valid = false;
    g_seenAttackSeq.clear();
    g_hostileTargets.clear();
    g_fightSeen = false;
    {
        const std::lock_guard<std::mutex> lock(g_receivedMutex);
        g_receivedFiles.clear();
    }
    spdlog::info("join: the host loaded another save; reconnecting to the new world");
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
    // A saved game that already has a party needs no extra dog; the guest
    // takes the second party member.
    const dao::script::Array existing = dao::script::api::GetPartyList(dao::kInvalidObject);
    if (existing.Size() >= 2)
    {
        g_companionDone = true;
        spdlog::info("companion: the save already has {} party member(s); not adding a dog", existing.Size());
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
            g_myAvatar = wanted;
            // Before this the guest was still on the host's Warden; the mirror
            // counted those repositions, which would look like the old binding.
            g_applied = 0;
            g_puppetApplied = 0;
            spdlog::info("avatar: this player's creature is 0x{:08x}", wanted);
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
        g_myAvatar = wanted;
        g_applied = 0;
        g_puppetApplied = 0;
        spdlog::info("avatar: this player's creature is 0x{:08x}", wanted);
    }
}

// Milestone 5 host: put a hostile creature into the world before the handoff
// save, far enough away not to start a fight in either game. The guest gets it
// with the save, and the host's copy is the authority for its health.
void SpawnHostile()
{
    if ((!g_combatTest && !g_demoFight) || g_hostileSpawned)
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
    // The demonstration fight spawns the ogre at the player's feet: any offset
    // in the level test save landed outside navigation (COMMAND_FAILED_INVALID_PATH)
    // and the ogre fell. The automated combat test keeps it far away so no fight
    // starts before the handoff save.
    location.position.x = position.x + (g_demoFight ? 0.5F : 25.0F);
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
    // The ogre is created inactive and with the scripts' AI-off switch so that
    // neither game enters combat before the handoff save is written (the
    // engine refuses saves in combat). The demonstration fight turns it back on
    // afterwards. It stays active=false so the save carries it without a fight.
    dao::script::api::SetLocalInt(hostile, L"CREATURE_RULES_FLAG0", 8);
    dao::script::api::SetObjectActive(hostile, 0);
    // A creature made by CreateObject does not always land on a hostile team
    // (seen in the Fort Drakon save): give it a team of its own and make that
    // team hostile to the player's, or GetNearestObjectByHostility never
    // returns it and the health channel has nothing to carry.
    {
        const dao::ObjectId playerId = g_myAvatar != dao::kInvalidObject ? g_myAvatar : dao::party::Controlled();
        const std::int32_t playerTeam = dao::script::api::GetTeamId(playerId);
        const std::int32_t hostileTeam = playerTeam + 1;
        dao::script::api::SetTeamId(hostile, hostileTeam);
        dao::script::api::SetGroupHostility(playerTeam, hostileTeam, 1);
        spdlog::info("combat: hostile team {} (player team {}), groups made hostile", hostileTeam, playerTeam);
    }
    spdlog::info("combat: hostile spawned 0x{:08x} from ogre_default.utc (AI off, inactive)", hostile);
}

// Milestone 5 host: damage the hostile on a schedule once the guest is
// connected, so the health channel has something to carry. This is a stand-in
// for a real fight: the host's game is the authority, the channel is what is
// being tested.
void DamageHostileTick()
{
    if (!g_combatTest || g_demoFight || g_hostileId == dao::kInvalidObject || g_hostileTicks >= 14)
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

// The host side of the hostile table: the hostiles around the player as the
// host's game sees them. Bounded by count and radius; a creature the lookup no
// longer answers is simply absent from the table (the guest then leaves it
// alone rather than guessing it died).
constexpr float kHostileRadius = 40.0F;

struct NearbyHostile
{
    dao::ObjectId id = dao::kInvalidObject;
    dao::Vector position{};
    float health = 0.0F;
    bool dead = false;
    dao::ObjectId target = dao::kInvalidObject;
    std::uint32_t attackSeq = 0;
};

int CollectNearbyHostiles(dao::CClientExoApp *app, dao::ObjectId player, NearbyHostile out[coop::kMaxHostiles])
{
    const dao::script::Array found = dao::script::api::GetNearestObjectByHostility(
        player, 1, dao::script::constants::OBJECT_TYPE_CREATURE, 8, 0, 0, 0);
    const dao::CCObject *self = app->GetObjectById(player);
    if (self == nullptr)
    {
        return 0;
    }
    const dao::Vector center = self->Position();
    int count = 0;
    for (std::size_t i = 0; i < found.Size() && count < static_cast<int>(coop::kMaxHostiles); ++i)
    {
        const dao::ObjectId id = found.Object(i);
        if (id == dao::kInvalidObject || id == player)
        {
            continue;
        }
        const dao::CCCreature *creature = app->GetCreatureById(id);
        if (creature == nullptr)
        {
            continue;
        }
        // Rats and other one-hit props are hostile too; they are not worth a
        // packet slot next to a real enemy.
        if (creature->MaxHealth().value_or(0.0F) <= 2.0F)
        {
            continue;
        }
        const dao::Vector position = creature->Position();
        const float dx = position.x - center.x;
        const float dy = position.y - center.y;
        const float dz = position.z - center.z;
        if (std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) > kHostileRadius)
        {
            continue;
        }
        out[count].id = id;
        out[count].position = position;
        out[count].health = creature->Health().value_or(0.0F);
        out[count].dead = dao::script::api::IsDead(id) != 0;
        const auto known = g_hostileTargets.find(id);
        constexpr std::uint64_t kTargetRememberFrames = 600; // about ten seconds
        if (known != g_hostileTargets.end() && dao::FrameCount() - known->second.second < kTargetRememberFrames)
        {
            out[count].target = known->second.first;
        }
        const auto attackSeq = g_hostileAttackSeq.find(id);
        if (attackSeq != g_hostileAttackSeq.end())
        {
            out[count].attackSeq = attackSeq->second;
        }
        ++count;
    }
    return count;
}

// Demonstration only: make the party survive long enough to watch the fight
// (the prologue Warden has 140 HP, an ogre has 220, and the Warden dies in a
// couple of minutes otherwise). DAO still resolves every blow.
void BuffPartyForDemo()
{
    if (g_demoPartyHealth <= 0.0F)
    {
        spdlog::info("demo: party health left as the game made it");
        return;
    }
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    if (app == nullptr)
    {
        return;
    }
    const dao::script::Array party = dao::script::api::GetPartyList(dao::kInvalidObject);
    for (std::size_t i = 0; i < party.Size(); ++i)
    {
        const dao::ObjectId id = party.Object(i);
        if (id == dao::kInvalidObject)
        {
            continue;
        }
        if (const dao::CCCreature *creature = app->GetCreatureById(id))
        {
            // A high-level saved party may already have more health than the
            // demo floor; never lower it.
            const float existingMax = creature->MaxHealth().value_or(0.0F);
            const float target = existingMax > g_demoPartyHealth ? existingMax : g_demoPartyHealth;
            creature->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, target,
                                  dao::script::PropertyValue::Base);
            creature->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, target,
                                  dao::script::PropertyValue::Current);
        }
    }
    spdlog::info("demo: party health set to at least {:.0f} for the demonstration", g_demoPartyHealth);
}

void DemoFightTick()
{
    if (!g_demoFight || !InWorld())
    {
        return;
    }
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    if (app == nullptr)
    {
        return;
    }
    if (g_role == Role::Host && g_handoff && g_handoffWritten && g_haveRemote && !g_demoStarted &&
        g_hostileId != dao::kInvalidObject)
    {
        // The save is on disk and the guest is in the world, so a fight may
        // start now: wake the ogre, turn its AI back on and order the host's
        // creature to attack it.
        g_demoStarted = true;
        g_fightSeen = true;
        BuffPartyForDemo();
        dao::script::api::SetObjectActive(g_hostileId, 1);
        dao::script::api::SetLocalInt(g_hostileId, L"CREATURE_RULES_FLAG0", 0);
        if (const dao::CCCreature *ogre = app->GetCreatureById(g_hostileId))
        {
            ogre->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, g_demoOgreHealth,
                              dao::script::PropertyValue::Base);
            ogre->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, g_demoOgreHealth,
                              dao::script::PropertyValue::Current);
        }
        const dao::ObjectId fighter = g_myAvatar != dao::kInvalidObject ? g_myAvatar : dao::party::Controlled();
        const bool ordered = dao::orders::Attack(fighter, g_hostileId);
        g_demoNextAttackOrder = dao::FrameCount() + 120;
        spdlog::info("demo: fight started at 0x{:08x} (active, AI on, attack order {})", g_hostileId, ordered);
        return;
    }
    // The host player's creature does not get input in the demonstration; keep
    // ordering it to attack while the foe lives, or it just stands there.
    if (g_role == Role::Host && g_demoStarted && g_hostileId != dao::kInvalidObject &&
        dao::FrameCount() > g_demoNextAttackOrder)
    {
        g_demoNextAttackOrder = dao::FrameCount() + 120;
        if (const dao::CCCreature *ogre = app->GetCreatureById(g_hostileId))
        {
            if (ogre->Health().value_or(0.0F) > 0.0F)
            {
                const dao::ObjectId fighter =
                    g_myAvatar != dao::kInvalidObject ? g_myAvatar : dao::party::Controlled();
                dao::orders::Attack(fighter, g_hostileId);
            }
        }
    }
    if (g_role == Role::Guest && !g_demoOrdered && g_haveRemote && g_remote.hostileCount > 0)
    {
        // Prefer the real enemy over any one-hit prop that slipped in.
        const coop::HostileEntry *chosen = nullptr;
        for (std::uint32_t i = 0; i < g_remote.hostileCount && i < coop::kMaxHostiles; ++i)
        {
            const coop::HostileEntry &entry = g_remote.hostiles[i];
            if (entry.id != dao::kInvalidObject && entry.health > 10.0F &&
                app->GetCreatureById(entry.id) != nullptr)
            {
                chosen = &entry;
                break;
            }
        }
        if (chosen != nullptr)
        {
            // The guest's copy of the ogre was saved inactive with its AI off;
            // wake it so the demonstration fight is visible on both screens.
            g_fightSeen = true;
            BuffPartyForDemo();
            dao::script::api::SetObjectActive(chosen->id, 1);
            dao::script::api::SetLocalInt(chosen->id, L"CREATURE_RULES_FLAG0", 0);
            const dao::ObjectId fighter = g_myAvatar != dao::kInvalidObject ? g_myAvatar : dao::party::Controlled();
            const bool ordered = dao::orders::Attack(fighter, chosen->id);
            g_demoOrdered = ordered;
            g_demoNextAttackOrder = dao::FrameCount() + 120;
            spdlog::info("demo: guest woke 0x{:08x} and ordered its creature to attack it ({})", chosen->id, ordered);
        }
    }
    // The guest's creature gets no input either; keep ordering it while the foe
    // lives, the same way the host does, or it stands after one order.
    if (g_role == Role::Guest && g_demoOrdered && g_haveRemote && g_remote.hostileCount > 0 &&
        dao::FrameCount() > g_demoNextAttackOrder)
    {
        g_demoNextAttackOrder = dao::FrameCount() + 120;
        const coop::HostileEntry &entry = g_remote.hostiles[0];
        if (entry.id != dao::kInvalidObject && entry.health > 10.0F && app->GetCreatureById(entry.id) != nullptr)
        {
            const dao::ObjectId fighter = g_myAvatar != dao::kInvalidObject ? g_myAvatar : dao::party::Controlled();
            dao::orders::Attack(fighter, entry.id);
        }
    }
}

// After the fight: fallen party members stand up (a health change from zero to
// positive is the documented native path that rebuilds placement and clears the
// dead timer, dev/COMBAT.md:403) and the host's player gets the avatar back if
// the game had moved control to a companion. First runtime test of this in co-op.
void RevivePartyAfterFight()
{
    if (!InWorld() || !g_haveRemote)
    {
        return;
    }
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    if (app == nullptr)
    {
        return;
    }
    const dao::ObjectId avatar = g_myAvatar != dao::kInvalidObject ? g_myAvatar : dao::party::Controlled();
    bool hostilesNear = false;
    if (g_role == Role::Host)
    {
        NearbyHostile found[coop::kMaxHostiles];
        const int count = CollectNearbyHostiles(app, avatar, found);
        for (int i = 0; i < count; ++i)
        {
            // A corpse left in the table is not a fight.
            if (!found[i].dead && found[i].health > 0.0F)
            {
                hostilesNear = true;
            }
        }
    }
    else
    {
        for (std::uint32_t i = 0; i < g_remote.hostileCount && i < coop::kMaxHostiles; ++i)
        {
            const coop::HostileEntry &entry = g_remote.hostiles[i];
            if ((entry.flags & coop::kHostileDead) == 0 && entry.health > 0.0F)
            {
                hostilesNear = true;
            }
        }
    }
    if (hostilesNear)
    {
        g_fightSeen = true;
        return;
    }
    if (!g_fightSeen)
    {
        return;
    }
    g_fightSeen = false;

    const dao::script::Array party = dao::script::api::GetPartyList(dao::kInvalidObject);
    int revived = 0;
    for (std::size_t i = 0; i < party.Size(); ++i)
    {
        const dao::ObjectId id = party.Object(i);
        if (id == dao::kInvalidObject)
        {
            continue;
        }
        const dao::CCCreature *creature = app->GetCreatureById(id);
        if (creature == nullptr)
        {
            continue;
        }
        const std::optional<float> health = creature->Health();
        if (!health || *health > 0.0F)
        {
            continue;
        }
        const float maxHealth = creature->MaxHealth().value_or(0.0F);
        if (maxHealth <= 0.0F)
        {
            continue;
        }
        creature->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, maxHealth,
                              dao::script::PropertyValue::Base);
        creature->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, maxHealth,
                              dao::script::PropertyValue::Current);
        ++revived;
        spdlog::info("revive: 0x{:08x} stands up with {:.0f} health", id, maxHealth);
    }
    if (g_role == Role::Host && g_myAvatar != dao::kInvalidObject && dao::party::Controlled() != g_myAvatar)
    {
        for (std::size_t i = 0; i < party.Size(); ++i)
        {
            if (party.Object(i) == g_myAvatar)
            {
                SendPartyIndex("control back to the player's creature at", static_cast<int>(i), 0);
                break;
            }
        }
    }
    if (revived > 0)
    {
        spdlog::info("revive: {} party member(s) back after the fight", revived);
    }
}

// Combat arbitration, host side: validate a guest's attack intent and, when it
// passes, order the attack in the host's own world. DAO resolves the fight.
const char *AttackResultName(coop::AttackResult result)
{
    switch (result)
    {
    case coop::AttackResult::Accepted:
        return "accepted";
    case coop::AttackResult::StaleRequest:
        return "stale-request";
    case coop::AttackResult::BadAttacker:
        return "bad-attacker";
    case coop::AttackResult::BadTarget:
        return "bad-target";
    case coop::AttackResult::NotHostile:
        return "not-hostile";
    case coop::AttackResult::WrongArea:
        return "wrong-area";
    case coop::AttackResult::OrderRefused:
        return "order-refused";
    }
    return "unknown";
}

void HandleGuestAttack(dao::CClientExoApp *app, const coop::AttackRequestPacket &request)
{
    if (g_role != Role::Host)
    {
        return;
    }
    coop::AttackResult result = coop::AttackResult::Accepted;
    const dao::CCCreature *attacker = app->GetCreatureById(request.attacker);
    const dao::CCCreature *target = app->GetCreatureById(request.target);
    const dao::CCObject *local = app->GetObjectById(dao::party::Controlled());
    if (!g_haveRemote || request.attacker != g_remote.objectId)
    {
        result = coop::AttackResult::BadAttacker;
    }
    else if (request.requestId <= g_lastGuestRequest)
    {
        result = coop::AttackResult::StaleRequest;
    }
    else if (attacker == nullptr)
    {
        result = coop::AttackResult::BadAttacker;
    }
    else if (target == nullptr || dao::script::api::IsDead(request.target) != 0)
    {
        result = coop::AttackResult::BadTarget;
    }
    else if (local != nullptr && target->AreaId() != local->AreaId())
    {
        result = coop::AttackResult::WrongArea;
    }
    else if (dao::script::api::IsObjectHostile(request.attacker, request.target) == 0)
    {
        result = coop::AttackResult::NotHostile;
    }
    else
    {
        g_lastGuestRequest = request.requestId;
        if (dao::orders::Attack(request.attacker, request.target))
        {
            ++g_attackRequestsAccepted;
        }
        else
        {
            result = coop::AttackResult::OrderRefused;
            ++g_attackRequestsRefused;
        }
    }

    coop::AttackResultPacket answer{};
    answer.flags = static_cast<std::uint8_t>(result);
    answer.requestId = request.requestId;
    answer.attacker = request.attacker;
    answer.target = request.target;
    g_net.Send(answer);
    spdlog::info("attack: guest request {} attacker 0x{:08x} target 0x{:08x} -> {}", request.requestId,
                 request.attacker, request.target, AttackResultName(result));
}

// Milestone 2: one frame's worth of co-op.
//  - both roles send their controlled creature's state about 20 times a second
//  - both drain the socket every frame
//  - the guest applies the host's position to its copy of the same creature
//  - the host, when asked, walks its character between waypoints so there is
//    movement to sync without a human at the controls
// See docs: the mirrored creature kept playing the animation of its last
// command after the fight ("runs in place"). When no fight is near, drop its
// queued command so it settles into its idle animation. Script command 534;
// commands cannot be cleared from inside a command observer, so this runs once
// per two and a half seconds from the frame callback instead.
bool FightNear()
{
    if (g_role == Role::Host)
    {
        return g_combatSeen;
    }
    for (std::uint32_t i = 0; i < g_remote.hostileCount && i < coop::kMaxHostiles; ++i)
    {
        const coop::HostileEntry &entry = g_remote.hostiles[i];
        if ((entry.flags & coop::kHostileDead) == 0 && entry.health > 0.0F)
        {
            return true;
        }
    }
    return false;
}

void ClearStalePuppetCommands(std::uint64_t frame)
{
    static std::uint64_t next = 0;
    if (!InWorld() || g_remote.objectId == dao::kInvalidObject || frame < next)
    {
        return;
    }
    next = frame + 150;
    if (FightNear())
    {
        return;
    }
    // Nothing queued means the creature is already idle: leave it alone.
    if (!dao::commands::HeldFor(g_remote.objectId))
    {
        return;
    }
    dao::script::api::ClearAllCommands(g_remote.objectId, 1);
    spdlog::info("puppet: dropped a stale command while no fight is near");
}

// The host's answer to a guest attack request. A bad-attacker answer is retried
// with the same id: the host may simply not have seen the guest yet.
void HandleAttackResult(const coop::AttackResultPacket &answer)
{
    const auto result = static_cast<coop::AttackResult>(answer.flags);
    if (result == coop::AttackResult::Accepted)
    {
        ++g_attackRequestsAccepted;
        g_attackAwaiting = false;
        g_attackResends = 0;
    }
    else
    {
        ++g_attackRequestsRefused;
        if (result == coop::AttackResult::BadAttacker && g_attackAwaiting && g_attackResends < 2)
        {
            ++g_attackResends;
            g_pendingAttack.valid = true;
            g_pendingAttack.requestId = g_attackLastRequest;
            g_pendingAttack.attacker = g_attackLastAttacker;
            g_pendingAttack.target = g_attackLastTarget;
            spdlog::info("attack: retrying request {} (attempt {})", g_attackLastRequest, g_attackResends + 1);
        }
        else
        {
            g_attackAwaiting = false;
        }
    }
    spdlog::info("attack: result for request {} (attacker 0x{:08x}, target 0x{:08x}): {}", answer.requestId,
                 answer.attacker, answer.target, AttackResultName(result));
}

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
    // The creature this player's game calls its own; the mirroring and the
    // state packet use it even if the game moved control to a companion.
    const dao::ObjectId avatar = g_myAvatar != dao::kInvalidObject ? g_myAvatar : controlled;

    // Send.
    if (dao::FrameCount() % kFramesBetweenSends == 0)
    {
        coop::StatePacket packet{};
        if (LocalState(packet))
        {
            if (g_role == Role::Host)
            {
                NearbyHostile found[coop::kMaxHostiles];
                const int count = CollectNearbyHostiles(app, avatar, found);
                packet.hostileCount = static_cast<std::uint8_t>(count);
                for (int i = 0; i < count; ++i)
                {
                    packet.hostiles[i].id = found[i].id;
                    packet.hostiles[i].x = found[i].position.x;
                    packet.hostiles[i].y = found[i].position.y;
                    packet.hostiles[i].z = found[i].position.z;
                    packet.hostiles[i].health = found[i].health;
                    packet.hostiles[i].flags = found[i].dead ? coop::kHostileDead : 0;
                    packet.hostiles[i].target =
                        found[i].target != dao::kInvalidObject ? found[i].target : 0;
                    packet.hostiles[i].attackSeq = found[i].attackSeq;
                }
                // Combat events for the log: the fight starting/ending and the
                // first mirrored hostile dying.
                int aliveHostiles = 0;
                for (int i = 0; i < count; ++i)
                {
                    if (!found[i].dead && found[i].health > 0.0F)
                    {
                        ++aliveHostiles;
                    }
                }
                // One fight is one pair of lines: "ended" only after five
                // seconds without a living hostile, so a hostile that steps out
                // of sight for a moment does not spam the log.
                if (aliveHostiles > 0)
                {
                    g_lastFightFrame = dao::FrameCount();
                    if (!g_combatSeen)
                    {
                        Notify(coop::EventCategory::Combat, "Combat started");
                        g_combatSeen = true;
                    }
                }
                else if (g_combatSeen && dao::FrameCount() - g_lastFightFrame > 150)
                {
                    Notify(coop::EventCategory::Combat, "Combat ended");
                    g_combatSeen = false;
                }
                if (count > 0 && found[0].health <= 0.0F && g_lastHostileHealth > 0.0F && !g_hostileDeathAnnounced)
                {
                    const std::string deadName = CreatureName(found[0].id);
                    Notify(coop::EventCategory::Combat, deadName.empty() ? "The hostile died" : deadName + " died");
                    g_hostileDeathAnnounced = true;
                }
                if (count > 0)
                {
                    g_lastHostileHealth = found[0].health;
                }
                // Tell the guest its world is out of date after a save load.
                if (g_worldReloadFrames > 0)
                {
                    packet.flags |= coop::kWorldReloaded;
                    --g_worldReloadFrames;
                }
            }
            g_net.Send(packet);
        }
    }

    // Receive everything waiting in one pass and dispatch by kind: separate
    // Receive calls per kind each scan the socket and a busy kind starves the
    // others.
    coop::StatePacket incoming{};
    bool gotPacket = false;
    coop::Transport::ReceivedPacket packet{};
    for (int i = 0; i < 32 && g_net.ReceiveAny(packet); ++i)
    {
        switch (packet.kind)
        {
        case coop::PacketKind::State:
        {
            std::memcpy(&incoming, packet.data, sizeof incoming);
            gotPacket = true;
            if (!g_haveRemote || incoming.sequence > g_remote.sequence)
            {
                g_remote = incoming;
                g_haveRemote = true;
            }
            break;
        }
        case coop::PacketKind::AttackRequest:
        {
            if (g_role == Role::Host)
            {
                coop::AttackRequestPacket request{};
                std::memcpy(&request, packet.data, sizeof request);
                HandleGuestAttack(app, request);
            }
            break;
        }
        case coop::PacketKind::AttackResult:
        {
            if (g_role == Role::Guest)
            {
                coop::AttackResultPacket answer{};
                std::memcpy(&answer, packet.data, sizeof answer);
                HandleAttackResult(answer);
            }
            break;
        }
        case coop::PacketKind::Event:
        {
            coop::EventPacket event{};
            std::memcpy(&event, packet.data, sizeof event);
            const std::string text(event.text);
            const auto category = static_cast<coop::EventCategory>(event.category);
            coop::EventLog::Get().Add(category, text);
            spdlog::info("event(remote): [{}] {}", coop::EventCategoryName(category), text);
            // The other player's creature swings in their own game; play the
            // same swing on this game's copy of it, so the two screens show the
            // attack. The combat line names the creature that attacked.
            if (g_role == Role::Guest && category == coop::EventCategory::Combat &&
                g_remote.objectId != dao::kInvalidObject)
            {
                const std::string puppetName = CreatureName(g_remote.objectId);
                if (!puppetName.empty() && text.rfind(puppetName + " attacks", 0) == 0)
                {
                    // Ahead of the copy's queued walk order: the swing plays now
                    // and the walk continues after it.
                    dao::script::OwnedCommand swing =
                        dao::script::api::CommandPlayAnimation(dao::animation::kAttack);
                    if (swing && dao::script::api::AddCommand(g_remote.objectId, *swing.get(), 1, 0) >= 0)
                    {
                        spdlog::info("puppet: attack animation for {}", puppetName);
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }
    if (gotPacket)
    {
        g_lastRemoteFrame = dao::FrameCount();
        if (g_connectionLost)
        {
            Notify(coop::EventCategory::Multiplayer, "Reconnected to the other player");
            g_connectionLost = false;
        }
        if (!g_sawRemote)
        {
            // The guest announces itself from its join path ("Joined the
            // host's game"), and both lines reach both logs through the event
            // packet, so only the host writes this one here.
            if (g_role == Role::Host)
            {
                Notify(coop::EventCategory::Multiplayer, "Another player joined the game");
            }
            g_sawRemote = true;
        }
    }
    else if (g_sawRemote && !g_connectionLost && dao::FrameCount() - g_lastRemoteFrame > 300)
    {
        Notify(coop::EventCategory::Multiplayer, "Connection to the other player lost");
        g_connectionLost = true;
    }

    if (g_haveRemote)
    {
        const bool reload = (g_remote.flags & coop::kWorldReloaded) != 0;
        if (reload && !g_worldReloadNoticed)
        {
            Notify(coop::EventCategory::World, "Host loaded another save; reconnecting to the new world");
            g_worldReloadNoticed = true;
            ResetGuestJoinForResync();
        }
        else if (!reload)
        {
            g_worldReloadNoticed = false;
        }
    }

    // Combat arbitration. The guest sends the intent its own game just saw (an
    // attack order), the host validates it, executes it in its own world and
    // answers; the guest logs the answer. The authoritative outcome arrives as
    // the hostile table's health, which the host owns.
    if (g_role == Role::Guest && g_pendingAttack.valid && g_haveRemote)
    {
        coop::AttackRequestPacket request{};
        request.requestId = g_pendingAttack.requestId;
        request.attacker = g_pendingAttack.attacker;
        request.target = g_pendingAttack.target;
        if (g_net.Send(request))
        {
            spdlog::info("attack: request {} sent (attacker 0x{:08x}, target 0x{:08x})", request.requestId,
                         request.attacker, request.target);
            g_attackAwaiting = true;
            g_attackLastRequest = request.requestId;
            g_attackLastAttacker = request.attacker;
            g_attackLastTarget = request.target;
            g_pendingAttack.valid = false;
        }
    }

    // Tactical pause: each game follows the other's pause through the same
    // engine message the pause key sends. A short cooldown keeps the two from
    // toggling at each other.
    if (g_haveRemote && dao::FrameCount() > g_pauseNextFrame && dao::FrameCount() > 60)
    {
        const bool remotePaused = (g_remote.flags & coop::kPaused) != 0;
        const bool localPaused = app->IsPaused();
        if (remotePaused != localPaused)
        {
            dao::GameModeController *modes = app->GameModes();
            const bool handled =
                remotePaused ? dao::messages::Send(modes, dao::messages::Make<dao::messages::PauseMessage>())
                             : dao::messages::Send(modes, dao::messages::Make<dao::messages::UnpauseMessage>());
            g_pauseNextFrame = dao::FrameCount() + 45;
            spdlog::info("pause: following the other player ({}), handled {}", remotePaused ? "paused" : "unpaused",
                         handled);
        }
    }
    // Test hook: the guest presses pause once so the two-instance rig can see
    // whether the host follows.
    if (g_testPause && !g_testPauseDone && g_role == Role::Guest && g_haveRemote && dao::FrameCount() > 600)
    {
        g_testPauseDone = true;
        dao::GameModeController *modes = app->GameModes();
        const bool handled = dao::messages::Send(modes, dao::messages::Make<dao::messages::PauseMessage>());
        spdlog::info("pause: test pressed pause on the guest, handled {}", handled);
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
    const bool waitingForOwnCreature = g_role == Role::Guest && !g_selected && g_selectParty >= 0;
      if (!waitingForOwnCreature && g_haveRemote && g_remote.areaId == local->AreaId() &&
          g_remote.objectId != dao::kInvalidObject)
    {
        if (g_remote.objectId != avatar)
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
                // A creature must not be teleported back to the other game's
                // position during its swing, or its blow never lands; the skip
                // lasts only the first frames of the attack, so the two games
                // still track each other and do not drift apart.
                const auto busyIt = g_busyCreatures.find(g_remote.objectId);
                const auto startedIt = g_lastAttackStart.find(g_remote.objectId);
                const std::uint64_t now = dao::FrameCount();
                const bool inSwing = busyIt != g_busyCreatures.end() && busyIt->second > 0 &&
                                     startedIt != g_lastAttackStart.end() &&
                                     now - startedIt->second < 30;
                const bool attackRecent =
                    startedIt != g_lastAttackStart.end() && now - startedIt->second < 120;
                if (!inSwing && distance > kApplyTolerance)
                {
                    dao::Vector target{};
                    target.x = g_remote.x;
                    target.y = g_remote.y;
                    target.z = g_remote.z;
                    if (!attackRecent && distance < kPuppetWalkRange)
                    {
                        // Walk there with the game's own move order: the
                        // creature's movement controller then plays its run
                        // animation (a teleport never does).
                        if (g_puppetMoves == 0)
                        {
                            g_puppetMoveStart = now;
                            g_puppetWalkBest = distance;
                        }
                        if (now >= g_puppetMoveFrame)
                        {
                            g_puppetMoveFrame = now + 20;
                            dao::orders::MoveTo(g_remote.objectId, target);
                            ++g_puppetMoves;
                            ++g_puppetWalks;
                        }
                        if (distance < g_puppetWalkBest - 0.05F)
                        {
                            g_puppetWalkBest = distance;
                        }
                        // Walking into a wall: after a moment with no progress,
                        // put the creature where it belongs instead.
                        if (g_puppetMoves > 0 && now - g_puppetMoveStart > 60 &&
                            distance >= g_puppetWalkBest - 0.05F)
                        {
                            dao::script::api::SetPosition(g_remote.objectId, target);
                            dao::script::api::ClearAllCommands(g_remote.objectId, 1);
                            ++g_puppetApplied;
                            g_puppetMoves = 0;
                        }
                    }
                    else
                    {
                        dao::script::api::SetPosition(g_remote.objectId, target);
                        if (!attackRecent)
                        {
                            dao::script::api::ClearAllCommands(g_remote.objectId, 1);
                        }
                        ++g_puppetApplied;
                        g_puppetMoves = 0;
                    }
                }
                else if (distance <= kApplyTolerance)
                {
                    g_puppetMoves = 0;
                }
                // Keep the remote creature's health in step too. While the
                // other player is alive their creature counts as a living party
                // member on this side, so the party is not "all dead" and the
                // game-over screen stays away until both players are down. The
                // maximum is set first, or the current value is clamped to the
                // template's small one (this is what left the pet at 1/1).
                const std::optional<float> puppetMax = puppet->MaxHealth();
                if (g_remote.maxHealth > 0.0F &&
                    (!puppetMax || std::fabs(*puppetMax - g_remote.maxHealth) > 1.0F))
                {
                    puppet->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, g_remote.maxHealth,
                                        dao::script::PropertyValue::Base);
                    ++g_puppetApplied;
                }
                const std::optional<float> puppetHealth = puppet->Health();
                if (puppetHealth && std::fabs(*puppetHealth - g_remote.health) > 1.0F)
                {
                    puppet->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, g_remote.health,
                                        dao::script::PropertyValue::Current);
                    ++g_puppetApplied;
                }
            }
        }
        else if (g_role == Role::Guest)
        {
            // Both games control the same creature, which means the guest did not
            // take a party member of its own. Teleporting its creature to the
            // host's position here is exactly the "stuck together / teleported
            // back" behaviour the owner reported: leave the guest's own creature
            // alone and say so once.
            if (!g_sharedCreatureWarned)
            {
                g_sharedCreatureWarned = true;
                Notify(coop::EventCategory::System,
                       "Both players control the same creature; the guest should select another party member");
            }
        }
    }

    // Guest: mirror the host's nearby-hostile table (position and health). The
    // host is the authority; a creature the table does not name is left alone.
    if (g_role == Role::Guest && g_haveRemote && g_remote.areaId == local->AreaId())
    {
        int appliedHere = 0;
        for (std::uint32_t i = 0; i < g_remote.hostileCount && i < coop::kMaxHostiles; ++i)
        {
            const coop::HostileEntry &entry = g_remote.hostiles[i];
            if (entry.id == dao::kInvalidObject)
            {
                continue;
            }
            const dao::CCCreature *hostile = app->GetCreatureById(entry.id);
            if (hostile == nullptr)
            {
                continue;
            }
            const dao::Vector position = hostile->Position();
            const float hx = entry.x - position.x;
            const float hy = entry.y - position.y;
            const float hz = entry.z - position.z;
            const float distance = std::sqrt((hx * hx) + (hy * hy) + (hz * hz));
            g_hostileDistance = distance;
            if (distance > kApplyTolerance)
            {
                dao::Vector target{};
                target.x = entry.x;
                target.y = entry.y;
                target.z = entry.z;
                dao::script::api::SetPosition(entry.id, target);
            }
            const std::optional<float> localHealth = hostile->Health();
            if (localHealth && std::fabs(*localHealth - entry.health) > 0.05F)
            {
                hostile->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, entry.health,
                                     dao::script::PropertyValue::Current);
                ++g_hostileApplied;
                ++appliedHere;
            }
            // The host mirrors every attack it starts (a rising sequence number
            // per creature), so the guest orders its copy exactly when the
            // host's copy swings, instead of on a timer.
            if (entry.target != 0 && entry.attackSeq != 0)
            {
                const auto seen = g_seenAttackSeq.find(entry.id);
                if (seen == g_seenAttackSeq.end() || seen->second != entry.attackSeq)
                {
                    if (app->GetCreatureById(entry.target) != nullptr &&
                        dao::script::api::IsObjectHostile(entry.id, entry.target) != 0)
                    {
                        const bool ordered = dao::orders::Attack(entry.id, entry.target);
                        g_seenAttackSeq[entry.id] = entry.attackSeq;
                        spdlog::info("mirror: hostile 0x{:08x} attack {} -> 0x{:08x} ({})", entry.id,
                                     entry.attackSeq, entry.target, ordered);
                    }
                }
            }
        }
        if (appliedHere > 0)
        {
            spdlog::info("hostile-table: applied health to {} of {} mirrored entries", appliedHere,
                         g_remote.hostileCount);
        }
    }

    // Either player can drive their own creature for tests (host by default,
    // the guest once it has selected its own party member).
    if (g_drive && dao::FrameCount() % kFramesBetweenMoves == 0)
    {
        dao::Vector target{};
        if (g_driveAway)
        {
            // Test hook: the host walks one way, the guest the other, so the two
            // avatars must part instead of staying together.
            const float direction = g_role == Role::Host ? -1.0F : 1.0F;
            target.x = g_anchor.x + direction * kDriveRadius;
            target.y = g_anchor.y;
            target.z = g_anchor.z;
        }
        else
        {
            const float angle = static_cast<float>(dao::FrameCount() / kFramesBetweenMoves) * 1.1F;
            target.x = g_anchor.x + (std::cos(angle) * kDriveRadius);
            target.y = g_anchor.y + (std::sin(angle) * kDriveRadius);
            target.z = g_anchor.z;
        }
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
            if (g_role == Role::Host)
            {
                NearbyHostile found[coop::kMaxHostiles];
                if (CollectNearbyHostiles(app, controlled, found) > 0)
                {
                    localHostile = found[0].id;
                    localHostileHealth = found[0].health;
                }
            }
            std::snprintf(line, sizeof line,
                          "net role=%s local=0x%08x remote=0x%08x puppet=0x%08x local (%.2f, %.2f, %.2f) area 0x%08x "
                          "remote (%.2f, %.2f, %.2f) area 0x%08x distance %.2f applied %llu puppet_applied %llu "
                          "puppet_distance %.2f puppet_walks %d hostiles %u hostile0=0x%08x hostile0_health %.1f local_hostile=0x%08x "
                          "local_hostile_health %.1f hostile_applied %llu hostile_distance %.2f sent %llu recv %llu "
                          "dropped %llu",
                          g_role == Role::Host ? "host" : "guest", controlled, g_remote.objectId, g_puppetId,
                          localPosition.x, localPosition.y, localPosition.z, local->AreaId(), g_remote.x, g_remote.y,
                          g_remote.z, g_remote.areaId, distance, static_cast<unsigned long long>(g_applied),
                          static_cast<unsigned long long>(g_puppetApplied), g_puppetDistance,
                          g_puppetWalks,
                          static_cast<unsigned>(g_remote.hostileCount), g_remote.hostiles[0].id,
                          g_remote.hostiles[0].health, localHostile, localHostileHealth,
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
            DemoFightTick();
            RevivePartyAfterFight();
            DamageHostileTick();
            // Test hook: kill the host player on purpose a few seconds into the
            // fight, so the revive path can be checked without waiting for the
            // hostile to win.
            if (g_role == Role::Host && g_testKillPlayer && g_testVictory && !g_testKillPlayerDone &&
                g_demoStarted)
            {
                if (g_testKillPlayerAt == 0)
                {
                    g_testKillPlayerAt = frame + 300;
                }
                if (frame >= g_testKillPlayerAt)
                {
                    g_testKillPlayerDone = true;
                    dao::CClientExoApp *exo = dao::CClientExoApp::Get();
                    const dao::ObjectId fightAvatar =
                        g_myAvatar != dao::kInvalidObject ? g_myAvatar : dao::party::Controlled();
                    if (exo != nullptr)
                    {
                        if (const dao::CCCreature *avatarCreature = exo->GetCreatureById(fightAvatar))
                        {
                            avatarCreature->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, 0.0F,
                                                        dao::script::PropertyValue::Current);
                        }
                    }
                    spdlog::info("test: the host player is killed on purpose");
                }
            }
            // Test hook: with the host player down, defeat the hostile so the
            // revive-after-the-fight path can be exercised.
            if (g_role == Role::Host && g_testVictory && !g_testVictoryDone)
            {
                dao::CClientExoApp *exo = dao::CClientExoApp::Get();
                const dao::ObjectId fightAvatar =
                    g_myAvatar != dao::kInvalidObject ? g_myAvatar : dao::party::Controlled();
                const dao::CCCreature *avatarCreature = exo != nullptr ? exo->GetCreatureById(fightAvatar) : nullptr;
                const float avatarHealth = avatarCreature != nullptr ? avatarCreature->Health().value_or(1.0F) : 1.0F;
                if (avatarHealth <= 0.0F && g_testVictoryAt == 0)
                {
                    g_testVictoryAt = frame + 180;
                    spdlog::info("test: the host player is down; the hostile will be defeated in three seconds");
                }
                if (g_testVictoryAt != 0 && frame >= g_testVictoryAt && exo != nullptr)
                {
                    g_testVictoryDone = true;
                    if (const dao::CCCreature *ogre = exo->GetCreatureById(g_hostileId))
                    {
                        ogre->SetProperty(dao::script::constants::PROPERTY_DEPLETABLE_HEALTH, 0.0F,
                                          dao::script::PropertyValue::Current);
                        spdlog::info("test: the hostile is defeated for the revive test");
                    }
                }
            }
            // Test hook: load another save on the host once the guest is in, so
            // the automatic guest rejoin can be exercised.
            if (g_role == Role::Host && g_testReload && !g_testReloaded && g_firstLoadSeen && g_haveRemote)
            {
                if (g_testReloadAt == 0)
                {
                    g_testReloadAt = frame + 120;
                }
                if (frame >= g_testReloadAt)
                {
                    g_testReloaded = true;
                    const std::wstring character = FirstCharacter();
                    spdlog::info("test: loading another save (slot 2) to exercise the guest rejoin");
                    dao::saves::Load(dao::saves::Slot{character, 2, dao::saves::Kind::Manual});
                }
            }
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
        const std::uint64_t handoffDelay = g_demoFight ? 120 : kHandoffDelayFrames;
        if (frame - g_handoffWorldFrame > handoffDelay && frame % 60 == 0)
        {
            if (dao::saves::Save(dao::saves::Kind::Manual, kHandoffSlot, L"coop-handoff"))
            {
                g_handoffRequested = true;
                spdlog::info("handoff: save requested for the guest (slot {})", kHandoffSlot);
            }
        }    }

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

    // Settle the mirrored creature when the fight is over (see the comment on
    // ClearStalePuppetCommands).
    ClearStalePuppetCommands(frame);

    // The in-game event log screen (no-op until the GUI system is up).
    coopui::Tick();

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
    g_driveAway = Flag("drive_away", "DAOCOOP_DRIVE_AWAY");
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
    if (g_role == Role::Guest && !g_join)
    {
        // A guest role with join off is a common misconfiguration: the game
        // loads its own save and the two players never share a world.
        Notify(coop::EventCategory::System,
               "Guest role without join=1: this game loads its own save; set join = 1 to join the host");
    }
    spdlog::info("drive {}", g_drive ? "on" : "off");

    g_addCompanion = g_role == Role::Host && Flag("add_companion", "DAOCOOP_ADD_COMPANION");
    const std::string selectParty = Setting("select_party", "DAOCOOP_SELECT_PARTY");
    g_selectParty = selectParty.empty() ? -1 : std::atoi(selectParty.c_str());
    // A guest that was given no party index takes the first companion: two
    // players on the same Warden would fight each other for the one input and
    // the mirror would keep teleporting the other one back.
    if (g_role == Role::Guest && g_selectParty < 0)
    {
        g_selectParty = 1;
        spdlog::info("party: the guest will take party index 1 by default");
    }
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
    g_demoFight = Flag("demo_fight", "DAOCOOP_DEMO_FIGHT");
    if (g_demoFight)
    {
        spdlog::info("demo fight on: the ogre spawns in sight and both creatures are ordered to attack it");
        const std::string partyHealth = Setting("demo_party_health", "DAOCOOP_DEMO_PARTY_HEALTH");
        if (!partyHealth.empty())
        {
            g_demoPartyHealth = std::strtof(partyHealth.c_str(), nullptr);
        }
        const std::string ogreHealth = Setting("demo_ogre_health", "DAOCOOP_DEMO_OGRE_HEALTH");
        if (!ogreHealth.empty())
        {
            g_demoOgreHealth = std::strtof(ogreHealth.c_str(), nullptr);
        }
    }
    g_testPause = Flag("test_pause", "DAOCOOP_TEST_PAUSE");
    g_testReload = Flag("test_reload", "DAOCOOP_TEST_RELOAD");
    g_testVictory = Flag("test_victory", "DAOCOOP_TEST_VICTORY");
    g_traceAnim = Flag("trace_anim", "DAOCOOP_TRACE_ANIM");
    g_traceSound = Flag("trace_sound", "DAOCOOP_TRACE_SOUND");
    g_testKillPlayer = Flag("test_kill_player", "DAOCOOP_TEST_KILL_PLAYER");
    dao::party::OnControlChanged([](dao::ObjectId before, dao::ObjectId now) {
        spdlog::info("control: 0x{:08x} -> 0x{:08x}", before, now);
        // The host's own creature is fixed when the world starts. When the
        // avatar falls the game may move control to a companion; that must not
        // change which creature this player's game names on the network.
        if (g_role == Role::Host && g_myAvatar == dao::kInvalidObject && now != dao::kInvalidObject)
        {
            g_myAvatar = now;
            spdlog::info("avatar: this player's creature is 0x{:08x}", now);
        }
        // And it must not take over the other player's creature: each side
        // keeps control of its own. If the game moved control away from the
        // avatar (to a companion, which on either side is the other player's
        // creature), ask for the avatar back.
        if (g_myAvatar != dao::kInvalidObject && now != dao::kInvalidObject && now != g_myAvatar)
        {
            const dao::script::Array party = dao::script::api::GetPartyList(dao::kInvalidObject);
            for (std::size_t i = 0; i < party.Size(); ++i)
            {
                if (party.Object(i) == g_myAvatar)
                {
                    spdlog::info("control: not taking 0x{:08x}; asking for the player's creature 0x{:08x}", now,
                                 g_myAvatar);
                    SendPartyIndex("keeping control on the player's creature at", static_cast<int>(i), 0);
                    break;
                }
            }
        }
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
            g_handoffWritten = true;
            spdlog::info("handoff: slot {} written; serving its folder", saved.number);
            StartHandoffSend(saved.Folder());
        }
    });

    // Diagnostics from DAOLib 0.6.0: log what the game plays, for comparing the
    // two machines' animations and sounds. Logging only; nothing is replayed.
    if (g_traceAnim)
    {
        dao::animation::OnPlayed([](dao::ObjectId creature, std::int32_t animation, float speed, float blend, bool flag) {
            if (creature != g_myAvatar && creature != g_remote.objectId && creature != g_hostileId)
            {
                return;
            }
            ++g_animEvents;
            double clock = -1.0;
            if (const auto now = dao::clock::Now())
            {
                clock = now->seconds;
            }
            spdlog::info("anim: frame {} clock {:.2f} 0x{:08x} row {} speed {:.2f} blend {:.2f} flag {} ({} total)",
                         dao::FrameCount(), clock, creature, animation, speed, blend, flag, g_animEvents);
        });
    }
    if (g_traceSound)
    {
        dao::audio::OnEventStart([](const dao::audio::EventStart &sound) {
            ++g_soundEvents;
            if (g_soundEvents > 400)
            {
                return;
            }
            spdlog::info("sound: frame {} [{}] ({:.1f}, {:.1f}, {:.1f})", dao::FrameCount(), std::string(sound.name),
                         sound.position[0], sound.position[1], sound.position[2]);
        });
    }
    dao::saves::OnGameLoaded([](const std::optional<dao::saves::Slot> &loaded) {
        if (loaded)
        {
            spdlog::info("game loaded: slot {} kind {}", loaded->number, dao::saves::Name(loaded->kind));
            if (!g_firstLoadSeen)
            {
                g_firstLoadSeen = true;
                if (g_role == Role::Guest)
                {
                    Notify(coop::EventCategory::Multiplayer, "Joined the host's game");
                }
            }
            else if (g_role == Role::Host)
            {
                // A load during a session replaces the world the guest is in.
                Notify(coop::EventCategory::World, "Host loaded another save; saving and serving the new world");
                g_worldReloadFrames = 120;
                ResetHandoffForResync();
            }
            else
            {
                // The guest's automatic rejoin: it loaded a new host save.
                g_myAvatar = dao::kInvalidObject;
                g_selected = false;
                g_selectPhase = 0;
                Notify(coop::EventCategory::Multiplayer, "Rejoined the host's new world");
            }
        }
        else
        {
            spdlog::info("game loaded: a new game");
        }
    });
    // Guest: watch its own creature's attack orders and turn them into requests
    // for the host. OnStart runs on the game thread while the queue is
    // processed, so keep it short and only record the intent.
    dao::commands::OnStart([](dao::ObjectId object, const dao::Command &command) {
        ++g_busyCreatures[object];
        if (command.Type() == dao::core::CommandType::Attack)
        {
            const std::optional<dao::ObjectId> attackTarget = command.Target();
            g_lastAttackStart[object] = dao::FrameCount();
            spdlog::info("cmd: 0x{:08x} attack start -> 0x{:08x}", object,
                         attackTarget.has_value() ? *attackTarget : dao::kInvalidObject);
            // A player-facing combat line, throttled per creature: the technical
            // command log above stays in the file log.
            const auto last = g_lastAttackLog.find(object);
            if (last == g_lastAttackLog.end() || dao::FrameCount() - last->second > 90)
            {
                g_lastAttackLog[object] = dao::FrameCount();
                const dao::ObjectId ourSide =
                    g_myAvatar != dao::kInvalidObject ? g_myAvatar : dao::party::Controlled();
                const bool attackerIsHostile =
                    object == g_hostileId || dao::script::api::IsObjectHostile(ourSide, object) != 0;
                const dao::ObjectId targetId = attackTarget.value_or(dao::kInvalidObject);
                const std::string attackerName = CreatureName(object);
                const std::string targetName = CreatureName(targetId);
                if (attackerIsHostile)
                {
                    const bool onOurSide =
                        targetId == ourSide || (g_role == Role::Host && targetId == g_remote.objectId);
                    const std::string who = attackerName.empty() ? "The enemy" : attackerName;
                    if (onOurSide)
                    {
                        Notify(coop::EventCategory::Combat,
                               who + " attacks " + (targetName.empty() ? "your creature" : targetName));
                    }
                    else
                    {
                        Notify(coop::EventCategory::Combat, who + " attacks");
                    }
                }
                else
                {
                    std::string who = attackerName;
                    if (who.empty())
                    {
                        who = (g_role == Role::Host && object == g_remote.objectId) ? "The other player's creature"
                                                                                    : "Your creature";
                    }
                    Notify(coop::EventCategory::Combat, who + " attacks " + (targetName.empty() ? "the enemy" : targetName));
                }
            }
        }
        // Host: remember what each creature is attacking, so the hostile table
        // can tell the guest's copy whom to fight.
        if (g_role == Role::Host && command.Type() == dao::core::CommandType::Attack)
        {
            const std::optional<dao::ObjectId> target = command.Target();
            if (target.has_value() && *target != dao::kInvalidObject)
            {
                g_hostileTargets[object] = {*target, dao::FrameCount()};
                ++g_hostileAttackSeq[object];
            }
        }
        if (g_role != Role::Guest || g_pendingAttack.valid)
        {
            return;
        }
        if (command.Type() != dao::core::CommandType::Attack)
        {
            return;
        }
        if (object != dao::party::Controlled())
        {
            return;
        }
        const std::optional<dao::ObjectId> target = command.Target();
        if (!target.has_value() || *target == dao::kInvalidObject)
        {
            return;
        }
        g_pendingAttack.valid = true;
        g_pendingAttack.requestId = ++g_attackRequestSeq;
        g_pendingAttack.attacker = object;
        g_pendingAttack.target = *target;
        spdlog::info("attack: guest saw 0x{:08x} ordered to attack 0x{:08x}; request {} queued", object, *target,
                     g_pendingAttack.requestId);
    });

    dao::commands::OnEnd([](dao::ObjectId object, const dao::Command &command, std::int32_t result) {
        if (command.Type() == dao::core::CommandType::Attack)
        {
            spdlog::info("cmd: 0x{:08x} attack end result {}", object, dao::commands::ResultName(result));
        }
        const auto it = g_busyCreatures.find(object);
        if (it != g_busyCreatures.end() && --it->second <= 0)
        {
            g_busyCreatures.erase(it);
        }
    });

    dao::OnFrame(&Frame);
    return true;
}




















