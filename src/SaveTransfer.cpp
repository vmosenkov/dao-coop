// The save transfer's implementation (SaveTransfer.h). One listen/connect per
// session; the sockets are closed on the way out of each call.
#include "SaveTransfer.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace coop
{
namespace
{

constexpr std::uint32_t kMagic = 0x534F4344; // "DCOS"
constexpr std::uint16_t kVersion = 1;

bool StartWinsock()
{
    static const bool started = []
    {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

bool SendAll(SOCKET socket, const void *data, std::size_t size)
{
    const char *bytes = static_cast<const char *>(data);
    std::size_t sent = 0;
    while (sent < size)
    {
        const int part = ::send(socket, bytes + sent, static_cast<int>(size - sent), 0);
        if (part <= 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(part);
    }
    return true;
}

bool ReceiveAll(SOCKET socket, void *data, std::size_t size)
{
    char *bytes = static_cast<char *>(data);
    std::size_t got = 0;
    while (got < size)
    {
        const int part = ::recv(socket, bytes + got, static_cast<int>(size - got), 0);
        if (part <= 0)
        {
            return false;
        }
        got += static_cast<std::size_t>(part);
    }
    return true;
}

SOCKET Listen(std::uint16_t port, std::string &error)
{
    if (!StartWinsock())
    {
        error = "winsock start-up failed";
        return INVALID_SOCKET;
    }
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
    {
        error = "socket() failed";
        return INVALID_SOCKET;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof address) == SOCKET_ERROR)
    {
        error = "bind() failed on port " + std::to_string(port) + " (in use?)";
        ::closesocket(listener);
        return INVALID_SOCKET;
    }
    if (::listen(listener, 1) == SOCKET_ERROR)
    {
        error = "listen() failed";
        ::closesocket(listener);
        return INVALID_SOCKET;
    }
    return listener;
}

SOCKET Connect(const std::string &host, std::uint16_t port, std::string &error)
{
    if (!StartWinsock())
    {
        error = "winsock start-up failed";
        return INVALID_SOCKET;
    }
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET)
    {
        error = "socket() failed";
        return INVALID_SOCKET;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
    {
        error = "bad host address: " + host;
        ::closesocket(socket);
        return INVALID_SOCKET;
    }
    if (::connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof address) == SOCKET_ERROR)
    {
        error = "connect() to " + host + ":" + std::to_string(port) + " failed";
        ::closesocket(socket);
        return INVALID_SOCKET;
    }
    return socket;
}

} // namespace

bool SendFolder(const std::filesystem::path &folder, std::uint16_t port, std::string &summary, std::string &error)
{
    std::error_code code;
    std::vector<FileBlob> files;
    for (const auto &entry : std::filesystem::directory_iterator(folder, code))
    {
        if (code || !entry.is_regular_file(code))
        {
            continue;
        }
        FileBlob blob;
        blob.name = entry.path().filename().string();
        const std::uint64_t size = entry.file_size(code);
        if (code || size > kMaxSaveBytes)
        {
            continue;
        }
        blob.data.resize(static_cast<std::size_t>(size));
        std::FILE *file = nullptr;
        if (_wfopen_s(&file, entry.path().wstring().c_str(), L"rb") != 0 || file == nullptr)
        {
            continue;
        }
        const std::size_t read = std::fread(blob.data.data(), 1, blob.data.size(), file);
        std::fclose(file);
        if (read != blob.data.size())
        {
            continue;
        }
        files.push_back(std::move(blob));
    }
    if (files.empty())
    {
        error = "the slot folder has no readable files";
        return false;
    }

    const SOCKET listener = Listen(port, error);
    if (listener == INVALID_SOCKET)
    {
        return false;
    }
    // Wait for the guest to connect. The transfer starts as soon as the host
    // has saved, but the guest may still be starting its game, so the window is
    // generous (ten minutes); the game keeps running while this thread waits.
    bool connected = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listener, &readSet);
        timeval timeout{};
        timeout.tv_sec = 5;
        const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready > 0 && FD_ISSET(listener, &readSet))
        {
            connected = true;
            break;
        }
    }
    if (!connected)
    {
        error = "no guest connected within 10 minutes";
        ::closesocket(listener);
        return false;
    }
    const SOCKET client = ::accept(listener, nullptr, nullptr);
    ::closesocket(listener);
    if (client == INVALID_SOCKET)
    {
        error = "accept() failed";
        return false;
    }

    bool ok = SendAll(client, &kMagic, sizeof kMagic) && SendAll(client, &kVersion, sizeof kVersion);
    const std::uint32_t count = static_cast<std::uint32_t>(files.size());
    ok = ok && SendAll(client, &count, sizeof count);
    std::uint64_t total = 0;
    for (const FileBlob &file : files)
    {
        if (!ok)
        {
            break;
        }
        const std::uint16_t nameLength = static_cast<std::uint16_t>(file.name.size());
        const std::uint64_t size = file.data.size();
        ok = SendAll(client, &nameLength, sizeof nameLength) && SendAll(client, file.name.data(), nameLength) &&
             SendAll(client, &size, sizeof size) && SendAll(client, file.data.data(), file.data.size());
        total += size;
    }
    ::shutdown(client, SD_SEND);
    ::closesocket(client);
    if (!ok)
    {
        error = "the connection failed while sending";
        return false;
    }
    summary = std::to_string(files.size()) + " file(s), " + std::to_string(total) + " bytes";
    return true;
}

bool ReceiveFolder(const std::string &host, std::uint16_t port, std::vector<FileBlob> &files, std::string &error)
{
    const SOCKET socket = Connect(host, port, error);
    if (socket == INVALID_SOCKET)
    {
        return false;
    }
    // Do not hang forever on a half-open transfer.
    DWORD timeoutMs = 60000;
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof timeoutMs);

    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint32_t count = 0;
    bool ok = ReceiveAll(socket, &magic, sizeof magic) && ReceiveAll(socket, &version, sizeof version) &&
              ReceiveAll(socket, &count, sizeof count);
    if (!ok || magic != kMagic || version != kVersion || count == 0 || count > kMaxSaveFiles)
    {
        error = "the sender is not a dao-coop host, or the header is bad";
        ::closesocket(socket);
        return false;
    }
    files.clear();
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::uint16_t nameLength = 0;
        if (!ReceiveAll(socket, &nameLength, sizeof nameLength) || nameLength == 0 || nameLength > 260)
        {
            ok = false;
            break;
        }
        FileBlob file;
        file.name.resize(nameLength);
        std::uint64_t size = 0;
        if (!ReceiveAll(socket, file.name.data(), nameLength) || !ReceiveAll(socket, &size, sizeof size) ||
            size > kMaxSaveBytes)
        {
            ok = false;
            break;
        }
        file.data.resize(static_cast<std::size_t>(size));
        if (!ReceiveAll(socket, file.data.data(), file.data.size()))
        {
            ok = false;
            break;
        }
        // A file name only, never a path: the writer joins it to the slot folder.
        if (file.name.find("..") != std::string::npos || file.name.find('/') != std::string::npos ||
            file.name.find('\\') != std::string::npos)
        {
            error = "a received file name is not a plain name";
            ok = false;
            break;
        }
        files.push_back(std::move(file));
    }
    ::closesocket(socket);
    if (!ok)
    {
        if (error.empty())
        {
            error = "the transfer ended early";
        }
        return false;
    }
    return true;
}

} // namespace coop
