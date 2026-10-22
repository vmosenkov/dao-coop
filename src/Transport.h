// The co-op transport: one UDP socket per game process, bound to any
// interface (loopback for the two-instance rig, a LAN address for two PCs).
// It is deliberately small and boring: fixed-size packets, no reliability
// (state is resent 20 times a second and only the latest one matters), and a
// non-blocking receive that the plugin drains from its frame callback.
//
// Wire format: little-endian fields in native structs (all members are 4-byte
// or smaller, so the layout is the same everywhere and the static_asserts make
// a change to it fail the build). Magic and version are checked on every
// packet; a datagram from anyone but the configured peer is dropped.
//
// Three packet kinds share the socket. A datagram of a kind the caller did not
// ask for is kept in one pending slot instead of being dropped, so an attack
// request that arrives between two state packets is not lost.
//
// Version 3 (combat arbitration): the single hostile of version 2 became a
// table of up to four nearby hostiles the host owns, plus the guest's attack
// intent and the host's answer to it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop
{

// 'DCOC' in little-endian, so a wrong port or an unrelated datagram is cheap
// to reject.
inline constexpr std::uint32_t kMagic = 0x434F4344;
inline constexpr std::uint16_t kVersion = 3;

// How many nearby hostiles the host mirrors.
inline constexpr std::uint32_t kMaxHostiles = 4;

enum class PacketKind : std::uint8_t
{
    State = 1,
    // A guest's attack intent; the host validates and executes it.
    AttackRequest = 2,
    // The host's answer to an AttackRequest; the outcome arrives as state.
    AttackResult = 3,
};

// One hostile creature as the host sees it.
struct HostileEntry
{
    std::uint32_t id = 0xFFFFFFFF;
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float health = 0.0F;
    std::uint32_t flags = 0; // HostileFlag::kHostileDead, else 0
};
static_assert(sizeof(HostileEntry) == 24, "the packet is its own wire format");

inline constexpr std::uint32_t kHostileDead = 1U << 0;

// One player's creature, as its own game sees it, plus the hostiles the host
// mirrors. The id is the engine's object id; both games name the same creature
// when they load the same save.
struct StatePacket
{
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    PacketKind kind = PacketKind::State;
    std::uint8_t flags = 0;
    std::uint32_t sequence = 0;
    std::uint32_t objectId = 0;
    std::uint32_t areaId = 0;
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float health = 0.0F;
    std::uint32_t sentMs = 0;
    std::uint8_t hostileCount = 0; // 0..kMaxHostiles
    std::uint8_t reserved[3] = {};
    HostileEntry hostiles[kMaxHostiles] = {};
};
static_assert(sizeof(StatePacket) == 140, "the packet is its own wire format; keep it fixed");

// A guest's request that the host execute an attack for the guest's creature.
// The guest sends an intent, never a result: the host resolves the fight with
// the game's own combat code.
struct AttackRequestPacket
{
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    PacketKind kind = PacketKind::AttackRequest;
    std::uint8_t flags = 0;
    std::uint32_t requestId = 0; // the guest's monotonically increasing id
    std::uint32_t attacker = 0;  // the guest's controlled creature
    std::uint32_t target = 0;    // the creature the guest wants to attack
};
static_assert(sizeof(AttackRequestPacket) == 20, "the packet is its own wire format");

// The host's answer: what happened to the request. Accepted only means the
// order was queued; the outcome arrives as hostile state.
struct AttackResultPacket
{
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    PacketKind kind = PacketKind::AttackResult;
    std::uint8_t flags = 0; // the AttackResult value
    std::uint32_t requestId = 0;
    std::uint32_t attacker = 0;
    std::uint32_t target = 0;
};
static_assert(sizeof(AttackResultPacket) == 20, "the packet is its own wire format");

// Why the host did or did not queue the attack.
enum class AttackResult : std::uint8_t
{
    Accepted = 0,
    StaleRequest = 1,
    BadAttacker = 2,
    BadTarget = 3,
    NotHostile = 4,
    WrongArea = 5,
    OrderRefused = 6,
};

class Transport
{
  public:
    Transport() = default;
    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;
    ~Transport();

    // Opens the socket, binds it to 0.0.0.0:listenPort (any interface, so the
    // same code serves loopback and LAN) and remembers the peer at
    // peerIp:peerPort. Safe to call once per process.
    // @param peerIp an IPv4 address as text; empty or invalid means 127.0.0.1
    bool Open(std::uint16_t listenPort, std::uint16_t peerPort, const char *peerIp = "127.0.0.1");
    void Close();
    bool IsOpen() const { return socket_ != kNoSocket; }

    // Sends one packet to the peer; false if the send failed.
    bool Send(const StatePacket &packet);
    bool Send(const AttackRequestPacket &packet);
    bool Send(const AttackResultPacket &packet);

    // Takes the next datagram of the kind asked for, if one is waiting; false
    // when there is none. Never blocks. A datagram of another kind is kept in
    // one pending slot until its own Receive call asks for it.
    bool Receive(StatePacket &packet);
    bool Receive(AttackRequestPacket &packet);
    bool Receive(AttackResultPacket &packet);

    std::uint64_t Sent() const { return sent_; }
    std::uint64_t Received() const { return received_; }
    std::uint64_t Dropped() const { return dropped_; }

    // Returns the local port the socket is bound to, 0 when not open.
    std::uint16_t LocalPort() const { return localPort_; }
    // Returns the peer's address as text, for the log.
    const char *PeerAddress() const { return peerText_; }

  private:
    bool ReceiveRaw(void *out, std::size_t outSize, PacketKind expected);

    static constexpr std::uintptr_t kNoSocket = ~std::uintptr_t{0};
    std::uintptr_t socket_ = kNoSocket;
    std::uintptr_t peerSize_ = 0; // sockaddr_in size, kept out of the header
    std::uint8_t peer_[16] = {};  // sockaddr_in, opaque here
    std::uint16_t localPort_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t received_ = 0;
    std::uint64_t dropped_ = 0;
    char peerText_[32] = "127.0.0.1";

    // One datagram of a kind the caller did not ask for, kept for the next
    // Receive call. It is always one of the fixed packet types.
    static constexpr std::size_t kMaxPacketSize = sizeof(StatePacket);
    std::uint8_t pendingData_[kMaxPacketSize] = {};
    std::size_t pendingSize_ = 0;
    PacketKind pendingKind_ = PacketKind::State;
    bool pendingValid_ = false;
};

} // namespace coop
