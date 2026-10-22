// The UDP transport's implementation (Transport.h). One socket, three packet
// kinds, one pending slot, and a receive that refuses anything not from the
// configured peer.
#include "Transport.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <cstdio>
#include <cstring>

namespace coop
{
namespace
{

// Winsock needs one start-up per process, before any socket call.
bool StartWinsock()
{
    static const bool started = []
    {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

template <class T> bool SendOne(SOCKET socket, const sockaddr_in &peer, T packet)
{
    packet.magic = kMagic;
    packet.version = kVersion;
    const int sent = ::sendto(socket, reinterpret_cast<const char *>(&packet), static_cast<int>(sizeof packet), 0,
                              reinterpret_cast<const sockaddr *>(&peer), static_cast<int>(sizeof peer));
    return sent == static_cast<int>(sizeof packet);
}

bool SamePeer(const sockaddr_in &a, const sockaddr_in &b)
{
    return a.sin_family == b.sin_family && a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
}

// The first eight bytes every packet starts with.
struct Header
{
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    PacketKind kind = PacketKind::State;
    std::uint8_t flags = 0;
};
static_assert(sizeof(Header) == 8, "the header is the shared prefix of every packet");

} // namespace

Transport::~Transport()
{
    Close();
}

bool Transport::Open(std::uint16_t listenPort, std::uint16_t peerPort, const char *peerIp)
{
    if (IsOpen())
    {
        return true;
    }
    if (!StartWinsock())
    {
        return false;
    }

    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET)
    {
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(listenPort);
    // Any interface: loopback works, and a LAN address reaches us too.
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket, reinterpret_cast<const sockaddr *>(&local), sizeof local) == SOCKET_ERROR)
    {
        ::closesocket(socket);
        return false;
    }

    // Never make the game thread wait on the network.
    u_long nonBlocking = 1;
    if (::ioctlsocket(socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        ::closesocket(socket);
        return false;
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(peerPort);
    const char *address = (peerIp != nullptr && peerIp[0] != '\0') ? peerIp : "127.0.0.1";
    if (::inet_pton(AF_INET, address, &peer.sin_addr) != 1)
    {
        address = "127.0.0.1";
        ::inet_pton(AF_INET, address, &peer.sin_addr);
    }
    std::snprintf(peerText_, sizeof peerText_, "%s", address);
    peerText_[sizeof peerText_ - 1] = '\0';
    std::memcpy(peer_, &peer, sizeof peer);
    peerSize_ = sizeof peer;
    socket_ = static_cast<std::uintptr_t>(socket);
    localPort_ = listenPort;
    return true;
}

void Transport::Close()
{
    if (IsOpen())
    {
        ::closesocket(static_cast<SOCKET>(socket_));
        socket_ = kNoSocket;
    }
    pendingValid_ = false;
}

bool Transport::Send(const StatePacket &packet)
{
    if (!IsOpen() || !SendOne(static_cast<SOCKET>(socket_), *reinterpret_cast<const sockaddr_in *>(peer_), packet))
    {
        return false;
    }
    ++sent_;
    return true;
}

bool Transport::Send(const AttackRequestPacket &packet)
{
    if (!IsOpen() || !SendOne(static_cast<SOCKET>(socket_), *reinterpret_cast<const sockaddr_in *>(peer_), packet))
    {
        return false;
    }
    ++sent_;
    return true;
}

bool Transport::Send(const AttackResultPacket &packet)
{
    if (!IsOpen() || !SendOne(static_cast<SOCKET>(socket_), *reinterpret_cast<const sockaddr_in *>(peer_), packet))
    {
        return false;
    }
    ++sent_;
    return true;
}

bool Transport::ReceiveRaw(void *out, std::size_t outSize, PacketKind expected)
{
    if (!IsOpen())
    {
        return false;
    }
    // A datagram of another kind that arrived earlier and was kept for us.
    if (pendingValid_ && pendingKind_ == expected && pendingSize_ == outSize)
    {
        std::memcpy(out, pendingData_, outSize);
        pendingValid_ = false;
        ++received_;
        return true;
    }

    const sockaddr_in &peer = *reinterpret_cast<const sockaddr_in *>(peer_);
    for (;;)
    {
        std::uint8_t buffer[256] = {};
        sockaddr_in from{};
        int fromSize = sizeof from;
        const int got = ::recvfrom(static_cast<SOCKET>(socket_), reinterpret_cast<char *>(buffer),
                                   static_cast<int>(sizeof buffer), 0, reinterpret_cast<sockaddr *>(&from), &fromSize);
        if (got == SOCKET_ERROR)
        {
            return false; // nothing waiting (WSAEWOULDBLOCK)
        }
        if (!SamePeer(from, peer) || got < static_cast<int>(sizeof(Header)))
        {
            ++dropped_;
            continue;
        }
        Header header{};
        std::memcpy(&header, buffer, sizeof header);
        if (header.magic != kMagic || header.version != kVersion)
        {
            ++dropped_;
            continue;
        }
        if (header.kind == expected)
        {
            if (got != static_cast<int>(outSize))
            {
                ++dropped_;
                continue;
            }
            std::memcpy(out, buffer, outSize);
            ++received_;
            return true;
        }
        // Another kind: keep one for its own Receive call, with its own size.
        std::size_t otherSize = 0;
        switch (header.kind)
        {
        case PacketKind::State:
            otherSize = sizeof(StatePacket);
            break;
        case PacketKind::AttackRequest:
            otherSize = sizeof(AttackRequestPacket);
            break;
        case PacketKind::AttackResult:
            otherSize = sizeof(AttackResultPacket);
            break;
        default:
            break;
        }
        if (otherSize == 0 || got != static_cast<int>(otherSize) || pendingValid_)
        {
            ++dropped_;
            continue;
        }
        std::memcpy(pendingData_, buffer, otherSize);
        pendingSize_ = otherSize;
        pendingKind_ = header.kind;
        pendingValid_ = true;
    }
}

bool Transport::Receive(StatePacket &packet)
{
    return ReceiveRaw(&packet, sizeof packet, PacketKind::State);
}

bool Transport::Receive(AttackRequestPacket &packet)
{
    return ReceiveRaw(&packet, sizeof packet, PacketKind::AttackRequest);
}

bool Transport::Receive(AttackResultPacket &packet)
{
    return ReceiveRaw(&packet, sizeof packet, PacketKind::AttackResult);
}

} // namespace coop

