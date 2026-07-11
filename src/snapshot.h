#pragma once

#include "mft.h"

// Snapshot = MftData dumped to disk. Scan once as admin, query forever
// without rescanning (or elevation). Format is tied to the MftEntry
// layout - bump SNAP_VERSION whenever that struct changes.
constexpr uint32_t SNAP_VERSION = 1;

bool SaveSnapshot(const char* path, const MftData& mft, char drive, std::string& err);
bool LoadSnapshot(const char* path, MftData& out, char& drive, uint64_t& takenFiletime,
                  std::string& err);
