// The co-op transport: one UDP socket per game process. Loopback for the
// two-instance rig, any interface for a LAN session (set the peer address).
// It is deliberately small and boring: a fixed-size state packet, no
// reliability (state is resent 20 times a second and only the latest one
// matters), and a non-blocking receive that the plugin drains from its frame
// callback.
//
// Wire format: little-endian fields in a packed struct. Magic and version are
// checked on every packet; a different build is ignored rather than guessed
// at.
//
// Version 2 (milestone 5) adds the hostile creature the host keeps in sync:
// its id, position and current health.
#pragma once

#include <cstdint>

namespace coop
{

// 'DCOC' in little-endian, so a wrong port or an unrelated datagram is cheap
// to reject.
inline constexpr std::uint32_t kMagic = 0x434F4344;
inline constexpr std::uint16_t kVersion = 2;

enum class PacketKind : std::uint8_t
{
    State = 1,
};

// One player's creature, as its own game sees it, plus the hostile the host
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
    // The hostile being mirrored (host-authoritative), or kNoHostile.
    std::uint32_t hostileId = 0xFFFFFFFF;
    float hostileX = 0.0F;
    float hostileY = 0.0F;
    float hostileZ = 0.0F;
    float hostileHealth = 0.0F;
};
static_assert(sizeof(StatePacket) == 60, "the packet is its own wire format; keep it fixed");

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

    // Takes the next datagram, if one is waiting; false when there is none or
    // it is not one of ours. Never blocks.
    bool Receive(StatePacket &packet);

    std::uint64_t Sent() const { return sent_; }
    std::uint64_t Received() const { return received_; }
    std::uint64_t Dropped() const { return dropped_; }

    // Returns the local port the socket is bound to, 0 when not open.
    std::uint16_t LocalPort() const { return localPort_; }
    // Returns the peer's address as text, for the log.
    const char *PeerAddress() const { return peerText_; }

  private:
    static constexpr std::uintptr_t kNoSocket = ~std::uintptr_t{0};
    std::uintptr_t socket_ = kNoSocket;
    std::uintptr_t peerSize_ = 0; // sockaddr_in size, kept out of the header
    std::uint8_t peer_[16] = {};  // sockaddr_in, opaque here
    std::uint16_t localPort_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t received_ = 0;
    std::uint64_t dropped_ = 0;
    char peerText_[32] = "127.0.0.1";
};

} // namespace coop
