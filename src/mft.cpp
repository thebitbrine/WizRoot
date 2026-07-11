#include "mft.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <memory>

namespace {

#pragma pack(push, 1)

struct RecordHeader {
    char magic[4]; // "FILE"
    uint16_t usaOffset;
    uint16_t usaCount;
    uint64_t lsn;
    uint16_t sequence;
    uint16_t hardLinks;
    uint16_t firstAttrOffset;
    uint16_t flags; // 0x01 in use, 0x02 directory
    uint32_t usedSize;
    uint32_t allocatedSize;
    uint64_t baseRecord; // nonzero = this is an extension of another record
    uint16_t nextAttrId;
};

struct AttrHeader {
    uint32_t type;
    uint32_t length;
    uint8_t nonResident;
    uint8_t nameLength;
    uint16_t nameOffset;
    uint16_t flags;
    uint16_t id;
    union {
        struct {
            uint32_t valueLength;
            uint16_t valueOffset;
            uint8_t indexed;
            uint8_t pad;
        } res;
        struct {
            uint64_t lowestVcn;
            uint64_t highestVcn;
            uint16_t runOffset;
            uint16_t compressionUnit;
            uint32_t pad;
            uint64_t allocatedSize;
            uint64_t dataSize;
            uint64_t initializedSize;
            uint64_t compressedSize; // only present when compressionUnit != 0
        } nr;
    };
};

struct FileNameAttr {
    uint64_t parent; // low 48 bits = record index, high 16 = sequence
    uint64_t times[4];
    uint64_t allocatedSize; // often stale, don't trust for sizes
    uint64_t dataSize;
    uint32_t fileAttrs;
    uint32_t reparse;
    uint8_t nameLen; // in wchars
    uint8_t nameSpace; // 0 posix, 1 win32, 2 dos-only, 3 win32+dos
    wchar_t name[1];
};

#pragma pack(pop)

constexpr uint32_t ATTR_STD_INFO = 0x10;
constexpr uint32_t ATTR_FILE_NAME = 0x30;
constexpr uint32_t ATTR_DATA = 0x80;
constexpr uint32_t ATTR_INDEX_ALLOC = 0xA0;
constexpr uint32_t ATTR_END = 0xFFFFFFFF;

constexpr uint32_t FILE_ATTR_REPARSE = 0x400;

struct Extent {
    uint64_t diskOffset; // bytes from volume start
    uint64_t length;     // bytes
};

// Sector ends are stamped with an update sequence number to detect torn
// writes; the real bytes live in the update sequence array. Undo that.
bool ApplyFixup(uint8_t* rec, uint32_t recordSize, uint32_t sectorSize) {
    auto* hdr = reinterpret_cast<RecordHeader*>(rec);
    if (memcmp(hdr->magic, "FILE", 4) != 0)
        return false;
    uint32_t count = hdr->usaCount; // 1 (the usn itself) + one entry per sector
    if (count < 2 || hdr->usaOffset + count * 2 > recordSize)
        return false;
    auto* usa = reinterpret_cast<uint16_t*>(rec + hdr->usaOffset);
    for (uint32_t i = 1; i < count; i++) {
        uint32_t end = i * sectorSize;
        if (end > recordSize)
            return false;
        auto* last = reinterpret_cast<uint16_t*>(rec + end - 2);
        if (*last != usa[0])
            return false; // torn record
        *last = usa[i];
    }
    return true;
}

// NTFS run list: packed (length, relative LCN) pairs. Header nibble
// encodes how many bytes each field takes. Offset is signed, relative
// to the previous run's LCN.
bool DecodeRuns(const uint8_t* p, const uint8_t* limit, uint64_t bytesPerCluster,
                std::vector<Extent>& out) {
    int64_t lcn = 0;
    while (p < limit && *p != 0) {
        uint32_t lenSize = *p & 0x0F;
        uint32_t ofsSize = *p >> 4;
        p++;
        if (lenSize == 0 || lenSize > 8 || ofsSize > 8 || p + lenSize + ofsSize > limit)
            return false;
        uint64_t runLen = 0;
        for (uint32_t i = 0; i < lenSize; i++)
            runLen |= uint64_t(p[i]) << (i * 8);
        p += lenSize;
        if (ofsSize == 0)
            return false; // sparse run - $MFT is never sparse
        int64_t delta = 0;
        for (uint32_t i = 0; i < ofsSize; i++)
            delta |= int64_t(p[i]) << (i * 8);
        // sign-extend
        if (p[ofsSize - 1] & 0x80)
            delta |= ~int64_t(0) << (ofsSize * 8);
        p += ofsSize;
        lcn += delta;
        if (lcn < 0)
            return false;
        out.push_back({uint64_t(lcn) * bytesPerCluster, runLen * bytesPerCluster});
    }
    return !out.empty();
}

// Physical clusters actually backing a run list - skips sparse holes
// (header byte with zero offset size). This is the only trustworthy
// allocation figure for sparse/compressed streams: their allocatedSize
// header field counts holes too ($BadClus:$Bad claims the whole volume).
uint64_t SumRealRuns(const uint8_t* p, const uint8_t* limit, uint64_t bytesPerCluster) {
    uint64_t clusters = 0;
    while (p < limit && *p != 0) {
        uint32_t lenSize = *p & 0x0F;
        uint32_t ofsSize = *p >> 4;
        p++;
        if (lenSize == 0 || lenSize > 8 || ofsSize > 8 || p + lenSize + ofsSize > limit)
            break;
        uint64_t runLen = 0;
        for (uint32_t i = 0; i < lenSize; i++)
            runLen |= uint64_t(p[i]) << (i * 8);
        if (ofsSize != 0) // zero offset = hole, no clusters behind it
            clusters += runLen;
        p += lenSize + ofsSize;
    }
    return clusters * bytesPerCluster;
}

constexpr uint16_t ATTRF_COMPRESSED = 0x0001;
constexpr uint16_t ATTRF_SPARSE = 0x8000;

// Walks a record's attributes, filling entry. Sizes may land on a
// different index than the record being parsed (extension records
// route to their base record).
void ParseRecord(uint8_t* rec, uint32_t recordSize, uint32_t recordIndex, MftData& data) {
    auto* hdr = reinterpret_cast<RecordHeader*>(rec);
    if (!(hdr->flags & 0x01))
        return; // deleted

    // Extension records hold overflow attributes for files whose attribute
    // list outgrew one record. Their $DATA sizes belong to the base record.
    uint32_t target = recordIndex;
    bool isExtension = hdr->baseRecord != 0;
    if (isExtension) {
        target = uint32_t(hdr->baseRecord & 0xFFFFFFFFFFFF);
        if (target >= data.entries.size())
            return;
    }
    MftEntry& e = data.entries[target];

    if (!isExtension) {
        e.flags |= MFT_IN_USE;
        e.seq = hdr->sequence;
        if (hdr->flags & 0x02)
            e.flags |= MFT_DIR;
    }

    uint32_t ofs = hdr->firstAttrOffset;
    bool haveName = e.nameLen != 0;
    while (ofs + sizeof(AttrHeader) <= recordSize) {
        auto* attr = reinterpret_cast<AttrHeader*>(rec + ofs);
        if (attr->type == ATTR_END)
            break;
        if (attr->length < 24 || ofs + attr->length > recordSize)
            break; // corrupt, stop here

        if (attr->type == ATTR_STD_INFO && !attr->nonResident &&
            attr->res.valueLength >= 16) {
            // times: created +0, modified +8 (the $FILE_NAME copies go
            // stale, these are the live ones)
            memcpy(&e.mtime, rec + ofs + attr->res.valueOffset + 8, 8);
        } else if (attr->type == ATTR_FILE_NAME && !attr->nonResident) {
            auto* fn = reinterpret_cast<FileNameAttr*>(rec + ofs + attr->res.valueOffset);
            // A file has one $FILE_NAME per namespace (plus one per hard
            // link). Skip DOS-only 8.3 names; first win32/posix name wins.
            if (fn->nameSpace != 2 && !haveName &&
                ofs + attr->res.valueOffset + offsetof(FileNameAttr, name) +
                        fn->nameLen * 2 <= recordSize) {
                e.parent = uint32_t(fn->parent & 0xFFFFFFFFFFFF);
                e.parentSeq = uint16_t(fn->parent >> 48);
                e.nameOffset = uint32_t(data.names.size());
                e.nameLen = fn->nameLen;
                if (fn->fileAttrs & FILE_ATTR_REPARSE)
                    e.flags |= MFT_REPARSE;
                data.names.insert(data.names.end(), fn->name, fn->name + fn->nameLen);
                haveName = true;
            }
        } else if (attr->type == ATTR_DATA) {
            // All $DATA streams count - unnamed main stream plus named
            // alternate streams (ADS). Allocation always comes from
            // summing the stream's real runs: the allocatedSize header
            // field counts sparse holes too, so it lies for sparse and
            // compressed streams. Logical size comes from dataSize,
            // except sparse streams count physical - a hole isn't data.
            //
            // Record 8 is $BadClus: its $Bad stream is an all-hole map
            // spanning the whole volume, without the sparse flag (it
            // predates it). Skip it or the volume "contains" itself.
            if (target == 8 && attr->nameLength != 0) {
                // skip $BadClus:$Bad
            } else if (!attr->nonResident) {
                e.size += attr->res.valueLength;
                // resident data lives inside the MFT record - 0 on disk
            } else {
                uint64_t physical = SumRealRuns(rec + ofs + attr->nr.runOffset,
                                                rec + ofs + attr->length, data.bytesPerCluster);
                e.allocated += physical;
                if (attr->flags & ATTRF_SPARSE)
                    e.size += physical;
                else if (attr->nr.lowestVcn == 0)
                    e.size += attr->nr.dataSize;
            }
        } else if (attr->type == ATTR_INDEX_ALLOC && attr->nonResident &&
                   attr->nr.lowestVcn == 0) {
            // Directory index blocks ($I30 b-tree) occupy real clusters.
            // Without this, totals run ~1% under what Windows calls used.
            e.allocated += attr->nr.allocatedSize;
        }
        ofs += attr->length;
    }
}

std::string WinErr(const char* what) {
    char buf[256];
    DWORD code = GetLastError();
    snprintf(buf, sizeof(buf), "%s (error %lu)", what, code);
    if (code == ERROR_ACCESS_DENIED)
        return std::string(buf) + " - run as administrator";
    return buf;
}

} // namespace

bool ReadMft(char driveLetter, MftData& out, std::string& err) {
    char volPath[8] = {'\\', '\\', '.', '\\', driveLetter, ':', 0};
    HANDLE vol = CreateFileA(volPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (vol == INVALID_HANDLE_VALUE) {
        err = WinErr("cannot open volume");
        return false;
    }

    NTFS_VOLUME_DATA_BUFFER nvd;
    DWORD got = 0;
    if (!DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &nvd, sizeof(nvd),
                         &got, nullptr)) {
        err = WinErr("not an NTFS volume");
        CloseHandle(vol);
        return false;
    }

    const uint32_t recordSize = nvd.BytesPerFileRecordSegment;
    const uint32_t sectorSize = nvd.BytesPerSector;
    const uint64_t clusterSize = nvd.BytesPerCluster;
    const uint64_t mftValidBytes = uint64_t(nvd.MftValidDataLength.QuadPart);
    const uint64_t totalRecords = mftValidBytes / recordSize;
    out.bytesPerCluster = clusterSize;

    auto readAt = [&](uint64_t offset, void* buf, uint32_t len) {
        LARGE_INTEGER li;
        li.QuadPart = int64_t(offset);
        DWORD rd = 0;
        return SetFilePointerEx(vol, li, nullptr, FILE_BEGIN) &&
               ReadFile(vol, buf, len, &rd, nullptr) && rd == len;
    };

    // Bootstrap: record 0 describes the $MFT itself. Its $DATA run list
    // is the map of where all other records live (the MFT is fragmented).
    std::vector<uint8_t> rec0(recordSize);
    if (!readAt(uint64_t(nvd.MftStartLcn.QuadPart) * clusterSize, rec0.data(), recordSize) ||
        !ApplyFixup(rec0.data(), recordSize, sectorSize)) {
        err = "cannot read $MFT record 0";
        CloseHandle(vol);
        return false;
    }

    std::vector<Extent> extents;
    {
        auto* hdr = reinterpret_cast<RecordHeader*>(rec0.data());
        uint32_t ofs = hdr->firstAttrOffset;
        while (ofs + sizeof(AttrHeader) <= recordSize) {
            auto* attr = reinterpret_cast<AttrHeader*>(rec0.data() + ofs);
            if (attr->type == ATTR_END || attr->length < 24 || ofs + attr->length > recordSize)
                break;
            if (attr->type == ATTR_DATA && attr->nonResident) {
                DecodeRuns(rec0.data() + ofs + attr->nr.runOffset,
                           rec0.data() + ofs + attr->length, clusterSize, extents);
                break;
            }
            ofs += attr->length;
        }
    }
    uint64_t mapped = 0;
    for (auto& ex : extents)
        mapped += ex.length;
    if (mapped < mftValidBytes) {
        // Would need $ATTRIBUTE_LIST parsing on $MFT itself - only happens
        // on absurdly fragmented volumes. Bail loudly rather than undercount.
        err = "$MFT run list is incomplete (attribute list not supported yet)";
        CloseHandle(vol);
        return false;
    }

    out.entries.assign(totalRecords, {});
    out.names.reserve(totalRecords * 12); // ~12 wchars/name is typical

    // Stream the MFT in 8MB chunks. Extents are cluster-aligned, not
    // record-aligned, so a chunk may span extents - fill it piecewise.
    constexpr uint32_t CHUNK = 8 * 1024 * 1024;
    std::unique_ptr<uint8_t[]> chunk(new uint8_t[CHUNK]);

    uint64_t remaining = mftValidBytes;
    uint32_t recordIndex = 0;
    size_t extIdx = 0;
    uint64_t extPos = 0; // bytes consumed of current extent
    bool readFailed = false;

    while (remaining > 0 && extIdx < extents.size()) {
        uint32_t want = uint32_t(remaining < CHUNK ? remaining : CHUNK);
        uint32_t filled = 0;
        while (filled < want && extIdx < extents.size()) {
            uint64_t left = extents[extIdx].length - extPos;
            uint32_t take = uint32_t(left < want - filled ? left : want - filled);
            if (!readAt(extents[extIdx].diskOffset + extPos, chunk.get() + filled, take)) {
                readFailed = true;
                break;
            }
            filled += take;
            extPos += take;
            if (extPos == extents[extIdx].length) {
                extIdx++;
                extPos = 0;
            }
        }
        if (readFailed || filled < want)
            break;

        for (uint32_t o = 0; o + recordSize <= filled; o += recordSize, recordIndex++) {
            if (ApplyFixup(chunk.get() + o, recordSize, sectorSize))
                ParseRecord(chunk.get() + o, recordSize, recordIndex, out);
        }
        remaining -= want;
    }
    CloseHandle(vol);

    if (readFailed) {
        err = WinErr("volume read failed mid-scan");
        return false;
    }

    for (auto& e : out.entries) {
        if (!(e.flags & MFT_IN_USE))
            continue;
        if (e.flags & MFT_DIR)
            out.dirCount++;
        else
            out.fileCount++;
        out.totalSize += e.size;
        out.totalAllocated += e.allocated; // dirs contribute index space here
    }
    return true;
}
