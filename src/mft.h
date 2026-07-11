#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One MFT record, 32 bytes. Names live in a shared arena (MftData::names)
// instead of per-entry strings - avoids millions of heap allocations.
struct MftEntry {
    uint64_t size = 0;        // logical bytes, all $DATA streams (main + ADS)
    uint64_t allocated = 0;   // bytes on disk; includes dir index allocations
    uint64_t mtime = 0;       // last modified, FILETIME (from $STANDARD_INFORMATION)
    uint32_t parent = 0;      // MFT record index of parent directory
    uint32_t nameOffset = 0;  // offset into MftData::names, in wchars
    uint16_t seq = 0;         // this record's sequence number
    uint16_t parentSeq = 0;   // sequence the parent ref expects - mismatch
                              // means the parent record was recycled
    uint8_t nameLen = 0;
    uint8_t flags = 0;
};

constexpr uint8_t MFT_IN_USE = 1;
constexpr uint8_t MFT_DIR = 2;
constexpr uint8_t MFT_REPARSE = 4; // junction / symlink / mount point

// NTFS root directory is always MFT record 5.
constexpr uint32_t MFT_ROOT = 5;

struct MftData {
    std::vector<MftEntry> entries; // indexed by MFT record number
    std::vector<wchar_t> names;    // name arena

    uint64_t fileCount = 0;
    uint64_t dirCount = 0;
    uint64_t totalSize = 0;
    uint64_t totalAllocated = 0;
    uint64_t bytesPerCluster = 0;
};

// Reads the full MFT of an NTFS volume ("C", "c:", etc). Needs admin.
// Returns false with a message in err on failure.
bool ReadMft(char driveLetter, MftData& out, std::string& err);
