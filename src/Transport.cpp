// The UDP transport's implementation (Transport.h). One socket, three packet
// kinds, and a receive that refuses anything not from the configured peer.
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

std::uint32_t NowMs()
{
    return static_cast<std::uint32_t>(GetTickCount64());
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

// One datagram of the kind asked for, from the peer only; other kinds and
// other senders are dropped without blocking.
template <class T> bool ReceiveOne(SOCKET socket, const sockaddr_in &peer, PacketKind kind, T &packet, std::uint64_t &received,
                                   std::uint64_t &dropped)
{
    for (;;)
    {
        T incoming{};
        sockaddr_in from{};
        int fromSize = sizeof from;
        const int got = ::recvfrom(socket, reinterpret_cast<char *>(&incoming), static_cast<int>(sizeof incoming), 0,
                                   reinterpret_cast<sockaddr *>(&from), &fromSize);
        if (got == SOCKET_ERROR)
        {
            return false; // nothing waiting (WSAEWOULDBLOCK)
        }
        if (!SamePeer(from, peer) || got != static_cast<int>(sizeof incoming) || incoming.magic != kMagic ||
            incoming.version != kVersion || incoming.kind != kind)
        {
            ++dropped;
            continue;
        }
        ++received;
        packet = incoming;
        return true;
    }
}

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
}

bool Transport::Send(const StatePacket &packet)
{
    if (!IsOpen() || !SendOne(static_cast<SOCKET>(socket_),
                              *reinterpret_cast<const sockaddr_in *>(peer_), packet))
    {
        return false;
    }
    ++sent_;
    return true;
}

bool Transport::Send(const AttackRequestPacket &packet)
{
    if (!IsOpen() || !SendOne(static_cast<SOCKET>(socket_),
                              *reinterpret_cast<const sockaddr_in *>(peer_), packet))
    {
        return false;
    }
    ++sent_;
    return true;
}

bool Transport::Send(const AttackResultPacket &packet)
{
    if (!IsOpen() || !SendOne(static_cast<SOCKET>(socket_),
                              *reinterpret_cast<const sockaddr_in *>(peer_), packet))
    {
        return false;
    }
    ++sent_;
    return true;
}

bool Transport::Receive(StatePacket &packet)
{
    return IsOpen() && ReceiveOne(static_cast<SOCKET>(socket_), *reinterpret_cast<const sockaddr_in *>(peer_),
                                  PacketKind::State, packet, received_, dropped_);
}

bool Transport::Receive(AttackRequestPacket &packet)
{
    return IsOpen() && ReceiveOne(static_cast<SOCKET>(socket_), *reinterpret_cast<const sockaddr_in *>(peer_),
                                  PacketKind::AttackRequest, packet, received_, dropped_);
}

bool Transport::Receive(AttackResultPacket &packet)
{
    return IsOpen() && ReceiveOne(static_cast<SOCKET>(socket_), *reinterpret_cast<const sockaddr_in *>(peer_),
                                  PacketKind::AttackResult, packet, received_, dropped_);
}

} // namespace coop
