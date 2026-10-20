// Milestone 3's save path: a tiny one-shot TCP transfer of one save slot's
// folder from the host to the guest.
//
// Why TCP and not the UDP state channel: a save is a few MB and every byte
// must arrive, in order, exactly once. The UDP channel carries state that is
// resent 20 times a second and does not care; this does. A blocking socket on
// a worker thread keeps the game thread free, and on loopback/LAN a transfer
// of a save takes well under a second.
//
// Wire format (little-endian):
//   "DCOS", version u16, file count u32
//   per file: name length u16, name bytes, size u64, bytes
// Names are file names only (no directories): a slot folder is flat.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace coop
{

struct FileBlob
{
    std::string name;
    std::vector<std::uint8_t> data;
};

// Host side: listens on 0.0.0.0:port for one client, sends every file in the
// folder, and returns. Blocking: call it from a worker thread.
// @param summary written with what was sent, for the log
bool SendFolder(const std::filesystem::path &folder, std::uint16_t port, std::string &summary, std::string &error);

// Guest side: connects to host:port and receives the files into memory.
// Blocking: call it from a worker thread, then write the files on the game
// thread when the world is at a menu.
bool ReceiveFolder(const std::string &host, std::uint16_t port, std::vector<FileBlob> &files, std::string &error);

// The most a save may be, and the most files a slot may hold: a guard against
// a wrong port feeding the game a bogus save.
inline constexpr std::uint64_t kMaxSaveBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaxSaveFiles = 64;

} // namespace coop
