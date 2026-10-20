// The co-op transport: one UDP socket per game process, bound to loopback for
// milestone 2. It is deliberately small and boring: a fixed-size state packet,
// no reliability (state is resent 20 times a second and only the latest one
// matters), and a non-blocking receive that the plugin drains from its frame
// callback. A listener thread is not needed at this stage, so none of this
// blocks the game thread.
//
// Wire format: little-endian fields in a packed struct. Magic and version are
// checked on every packet; a different build is ignored rather than guessed
// at.
#pragma once

#include <cstdint>

namespace coop
{

// 'DCOC' in little-endian, so a wrong port or an unrelated datagram is cheap
// to reject.
inline constexpr std::uint32_t kMagic = 0x434F4344;
inline constexpr std::uint16_t kVersion = 1;

enum class PacketKind : std::uint8_t
{
    State = 1,
};

// One player's creature, as its own game sees it. The id is the engine's
// object id; both games name the same creature when they load the same save.
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
};
static_assert(sizeof(StatePacket) == 40, "the packet is its own wire format; keep it fixed");

class Transport
{
  public:
    Transport() = default;
    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;
    ~Transport();

    // Opens the socket, binds it to 127.0.0.1:listenPort and remembers the
    // peer at 127.0.0.1:peerPort. Safe to call once per process.
    bool Open(std::uint16_t listenPort, std::uint16_t peerPort);
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

  private:
    static constexpr std::uintptr_t kNoSocket = ~std::uintptr_t{0};
    std::uintptr_t socket_ = kNoSocket;
    std::uintptr_t peerSize_ = 0; // sockaddr_in size, kept out of the header
    std::uint8_t peer_[16] = {};  // sockaddr_in, opaque here
    std::uint16_t localPort_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t received_ = 0;
    std::uint64_t dropped_ = 0;
};

} // namespace coop
