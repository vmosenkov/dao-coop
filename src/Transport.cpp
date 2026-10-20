// The UDP transport's implementation (Transport.h). Loopback only for now;
// milestone 3 will add the LAN address and the join handshake.
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
    if (!IsOpen())
    {
        return false;
    }
    StatePacket outgoing = packet;
    outgoing.magic = kMagic;
    outgoing.version = kVersion;
    outgoing.sentMs = NowMs();
    const int sent = ::sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char *>(&outgoing),
                              static_cast<int>(sizeof outgoing), 0, reinterpret_cast<const sockaddr *>(peer_),
                              static_cast<int>(peerSize_));
    if (sent == static_cast<int>(sizeof outgoing))
    {
        ++sent_;
        return true;
    }
    return false;
}

bool Transport::Receive(StatePacket &packet)
{
    if (!IsOpen())
    {
        return false;
    }
    for (;;)
    {
        StatePacket incoming{};
        const int got = ::recvfrom(static_cast<SOCKET>(socket_), reinterpret_cast<char *>(&incoming),
                                   static_cast<int>(sizeof incoming), 0, nullptr, nullptr);
        if (got == SOCKET_ERROR)
        {
            return false; // nothing waiting (WSAEWOULDBLOCK)
        }
        if (got != static_cast<int>(sizeof incoming) || incoming.magic != kMagic || incoming.version != kVersion ||
            incoming.kind != PacketKind::State)
        {
            ++dropped_;
            continue; // drain junk, keep looking for a real packet
        }
        ++received_;
        packet = incoming;
        return true;
    }
}

} // namespace coop
