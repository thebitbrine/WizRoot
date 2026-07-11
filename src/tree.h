#pragma once

#include "mft.h"

constexpr uint32_t NO_DIR = 0xFFFFFFFF;

// Directory tree over the flat MFT: subtree aggregates per directory
// plus CSR child lists (offsets into one flat array - no per-node
// allocations).
struct TreeData {
    struct Agg {
        uint64_t size = 0;      // subtree logical bytes
        uint64_t allocated = 0; // subtree bytes on disk
        uint32_t files = 0;     // subtree file count
        uint32_t dirs = 0;      // subtree dir count (excluding self)
    };

    std::vector<uint32_t> dirSlot; // per MFT record: index into agg, or NO_DIR
    std::vector<Agg> agg;          // per directory slot

    std::vector<uint32_t> childOffset; // per dir slot, size dirCount+1
    std::vector<uint32_t> children;    // MFT record indices, grouped by parent

    uint32_t orphans = 0; // in-use entries whose parent isn't a live dir
};

void BuildTree(const MftData& mft, TreeData& tree);

// Resolves "C:\foo\bar" (or "\foo\bar", or "" for root) to an MFT record
// index by walking child lists. Case-insensitive. Returns NO_DIR if not
// found or not a directory.
uint32_t FindDir(const MftData& mft, const TreeData& tree, const char* path);
