#include "snapshot.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace {

#pragma pack(push, 1)
struct SnapHeader {
    char magic[4]; // "WZRT"
    uint32_t version;
    uint8_t drive;
    uint8_t pad[7];
    uint64_t takenFiletime;
    uint64_t entryCount;
    uint64_t nameCount; // wchars
    uint64_t bytesPerCluster;
};
#pragma pack(pop)

uint64_t NowFiletime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

} // namespace

bool SaveSnapshot(const char* path, const MftData& mft, char drive, std::string& err) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        err = std::string("cannot write snapshot: ") + path;
        return false;
    }
    SnapHeader h{{'W', 'Z', 'R', 'T'}, SNAP_VERSION,          uint8_t(drive), {},
                 NowFiletime(),        mft.entries.size(),    mft.names.size(),
                 mft.bytesPerCluster};
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
              fwrite(mft.entries.data(), sizeof(MftEntry), mft.entries.size(), f) ==
                  mft.entries.size() &&
              fwrite(mft.names.data(), sizeof(wchar_t), mft.names.size(), f) ==
                  mft.names.size();
    fclose(f);
    if (!ok)
        err = "snapshot write failed (disk full?)";
    return ok;
}

bool LoadSnapshot(const char* path, MftData& out, char& drive, uint64_t& takenFiletime,
                  std::string& err) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        err = std::string("cannot open snapshot: ") + path;
        return false;
    }
    SnapHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "WZRT", 4) != 0) {
        err = "not a wizroot snapshot";
        fclose(f);
        return false;
    }
    if (h.version != SNAP_VERSION) {
        err = "snapshot version mismatch - rescan and save again";
        fclose(f);
        return false;
    }
    out.entries.resize(h.entryCount);
    out.names.resize(h.nameCount);
    out.bytesPerCluster = h.bytesPerCluster;
    bool ok = fread(out.entries.data(), sizeof(MftEntry), h.entryCount, f) == h.entryCount &&
              fread(out.names.data(), sizeof(wchar_t), h.nameCount, f) == h.nameCount;
    fclose(f);
    if (!ok) {
        err = "snapshot truncated or corrupt";
        return false;
    }
    drive = char(h.drive);
    takenFiletime = h.takenFiletime;
    for (auto& e : out.entries) {
        if (!(e.flags & MFT_IN_USE))
            continue;
        if (e.flags & MFT_DIR)
            out.dirCount++;
        else
            out.fileCount++;
        out.totalSize += e.size;
        out.totalAllocated += e.allocated;
    }
    return true;
}
