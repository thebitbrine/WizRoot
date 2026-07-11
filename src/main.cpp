#include "mft.h"
#include "snapshot.h"
#include "tree.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

struct Options {
    const char* path = nullptr;
    const char* find = nullptr;
    const char* save = nullptr;
    const char* load = nullptr;
    const char* csv = nullptr;
    const char* match = nullptr; // wildcard for --files, e.g. *.log
    uint32_t top = 20;
    uint32_t depth = 1;
    uint64_t mtimeCutoff = 0; // FILETIME; nonzero = only files older than this
    uint64_t minSize = 0;
    int by = 0; // 0 size, 1 alloc, 2 count
    bool files = false;
    bool json = false;

    std::wstring matchW; // match converted once for arena comparisons
};

// Case-insensitive * and ? wildcard match, iterative with star backtrack.
bool WildMatch(const wchar_t* pat, const wchar_t* pend, const wchar_t* s, const wchar_t* send) {
    const wchar_t *star = nullptr, *starS = nullptr;
    while (s < send) {
        if (pat < pend && (*pat == L'?' || towupper(*pat) == towupper(*s))) {
            pat++, s++;
        } else if (pat < pend && *pat == L'*') {
            star = pat++;
            starS = s;
        } else if (star) {
            pat = star + 1; // retry the star eating one more char
            s = ++starS;
        } else {
            return false;
        }
    }
    while (pat < pend && *pat == L'*')
        pat++;
    return pat == pend;
}

// "500", "10K", "100M", "2G", "1T" -> bytes
uint64_t ParseSize(const char* s) {
    char* end = nullptr;
    uint64_t v = strtoull(s, &end, 10);
    switch (end ? toupper(uint8_t(*end)) : 0) {
    case 'K': return v << 10;
    case 'M': return v << 20;
    case 'G': return v << 30;
    case 'T': return v << 40;
    default: return v;
    }
}

uint64_t Now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::string IsoDate(uint64_t filetime) {
    FILETIME ft{DWORD(filetime), DWORD(filetime >> 32)};
    SYSTEMTIME st;
    if (!filetime || !FileTimeToSystemTime(&ft, &st))
        return "----------";
    char buf[16];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

uint64_t ToUnix(uint64_t filetime) {
    return filetime > 116444736000000000ULL ? (filetime - 116444736000000000ULL) / 10000000
                                            : 0;
}

std::string ToUtf8(const wchar_t* s, int len) {
    if (len <= 0)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
    return out;
}

std::string EntryName(const MftData& mft, const MftEntry& e) {
    return ToUtf8(mft.names.data() + e.nameOffset, e.nameLen);
}

// Walk parent chain up to the root. Depth cap guards against corrupt
// parent references creating cycles.
std::string BuildPath(const MftData& mft, uint32_t index, char drive) {
    std::string path;
    int depth = 0;
    while (index != MFT_ROOT && depth++ < 512) {
        if (index >= mft.entries.size() || !(mft.entries[index].flags & MFT_IN_USE))
            return std::string(1, drive) + ":\\<orphan>\\" + path;
        const MftEntry& e = mft.entries[index];
        path = EntryName(mft, e) + (path.empty() ? "" : "\\") + path;
        index = e.parent;
    }
    return std::string(1, drive) + ":\\" + path;
}

std::string Human(uint64_t bytes) {
    const char* unit[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.2f %s", v, unit[u]);
    return buf;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\', out += c;
        else if (uint8_t(c) < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else
            out += c;
    }
    return out;
}

// Sort key for an entry: subtree aggregates for dirs, own size for files.
uint64_t SortKey(const MftData& mft, const TreeData& tree, uint32_t idx, int by) {
    uint32_t slot = tree.dirSlot[idx];
    const MftEntry& e = mft.entries[idx];
    if (slot != NO_DIR) {
        const TreeData::Agg& a = tree.agg[slot];
        return by == 0 ? a.size : by == 1 ? a.allocated : a.files;
    }
    return by == 2 ? 1 : by == 1 ? e.allocated : e.size;
}

// Top-N children of a dir by sort key, descending.
std::vector<uint32_t> TopChildren(const MftData& mft, const TreeData& tree, uint32_t dir,
                                  const Options& opt) {
    uint32_t slot = tree.dirSlot[dir];
    std::vector<uint32_t> kids(tree.children.begin() + tree.childOffset[slot],
                               tree.children.begin() + tree.childOffset[slot + 1]);
    size_t n = std::min<size_t>(opt.top, kids.size());
    std::partial_sort(kids.begin(), kids.begin() + n, kids.end(),
                      [&](uint32_t a, uint32_t b) {
                          return SortKey(mft, tree, a, opt.by) > SortKey(mft, tree, b, opt.by);
                      });
    kids.resize(n);
    return kids;
}

void PrintListing(const MftData& mft, const TreeData& tree, uint32_t dir, uint64_t parentSize,
                  const Options& opt, uint32_t level) {
    for (uint32_t idx : TopChildren(mft, tree, dir, opt)) {
        const MftEntry& e = mft.entries[idx];
        uint32_t slot = tree.dirSlot[idx];
        // show allocated when ranking by it, logical size otherwise
        uint64_t val = slot != NO_DIR
                           ? (opt.by == 1 ? tree.agg[slot].allocated : tree.agg[slot].size)
                           : (opt.by == 1 ? e.allocated : e.size);
        if (opt.minSize && val < opt.minSize)
            continue;
        double pct = parentSize ? 100.0 * double(val) / double(parentSize) : 0;
        const char* mark = (e.flags & MFT_REPARSE) ? " [reparse]" : "";
        printf("%*s%10s %5.1f%%", int(level * 2), "", Human(val).c_str(), pct);
        if (slot != NO_DIR)
            printf("  %8u files  %s\\%s\n", tree.agg[slot].files, EntryName(mft, e).c_str(),
                   mark);
        else
            printf("  %8s        %s%s\n", "", EntryName(mft, e).c_str(), mark);
        if (slot != NO_DIR && level + 1 < opt.depth)
            PrintListing(mft, tree, idx, val, opt, level + 1);
    }
}

void PrintListingJson(const MftData& mft, const TreeData& tree, uint32_t dir,
                      const Options& opt, uint32_t level) {
    printf("[");
    bool first = true;
    for (uint32_t idx : TopChildren(mft, tree, dir, opt)) {
        const MftEntry& e = mft.entries[idx];
        uint32_t slot = tree.dirSlot[idx];
        printf("%s{\"name\":\"%s\"", first ? "" : ",", JsonEscape(EntryName(mft, e)).c_str());
        first = false;
        if (slot != NO_DIR) {
            const TreeData::Agg& a = tree.agg[slot];
            printf(",\"dir\":true,\"size\":%llu,\"allocated\":%llu,\"files\":%u,\"dirs\":%u",
                   (unsigned long long)a.size, (unsigned long long)a.allocated, a.files, a.dirs);
            if (level + 1 < opt.depth) {
                printf(",\"children\":");
                PrintListingJson(mft, tree, idx, opt, level + 1);
            }
        } else {
            printf(",\"dir\":false,\"size\":%llu,\"allocated\":%llu",
                   (unsigned long long)e.size, (unsigned long long)e.allocated);
        }
        if (e.flags & MFT_REPARSE)
            printf(",\"reparse\":true");
        printf("}");
    }
    printf("]");
}

// Top-N files in a subtree: DFS over dirs, keep a running worst-of-best
// cutoff instead of collecting all files.
std::vector<uint32_t> TopFiles(const MftData& mft, const TreeData& tree, uint32_t dir,
                               const Options& opt) {
    std::vector<uint32_t> best; // kept sorted desc by key
    std::vector<uint32_t> stack{dir};
    while (!stack.empty()) {
        uint32_t d = stack.back();
        stack.pop_back();
        uint32_t slot = tree.dirSlot[d];
        for (uint32_t c = tree.childOffset[slot]; c < tree.childOffset[slot + 1]; c++) {
            uint32_t idx = tree.children[c];
            if (tree.dirSlot[idx] != NO_DIR) {
                stack.push_back(idx);
                continue;
            }
            const MftEntry& e = mft.entries[idx];
            if (opt.mtimeCutoff && e.mtime >= opt.mtimeCutoff)
                continue;
            if (opt.minSize && (opt.by == 1 ? e.allocated : e.size) < opt.minSize)
                continue;
            if (!opt.matchW.empty() &&
                !WildMatch(opt.matchW.data(), opt.matchW.data() + opt.matchW.size(),
                           mft.names.data() + e.nameOffset,
                           mft.names.data() + e.nameOffset + e.nameLen))
                continue;
            uint64_t key = SortKey(mft, tree, idx, opt.by);
            if (best.size() == opt.top && key <= SortKey(mft, tree, best.back(), opt.by))
                continue;
            auto pos = std::lower_bound(best.begin(), best.end(), key, [&](uint32_t b, uint64_t k) {
                return SortKey(mft, tree, b, opt.by) > k;
            });
            best.insert(pos, idx);
            if (best.size() > opt.top)
                best.pop_back();
        }
    }
    return best;
}

void CsvField(FILE* f, const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) {
        fputs(s.c_str(), f);
        return;
    }
    fputc('"', f);
    for (char c : s) {
        if (c == '"')
            fputc('"', f);
        fputc(c, f);
    }
    fputc('"', f);
}

// Full recursive dump. Path built incrementally - no per-row rebuilding.
void CsvWalk(FILE* f, const MftData& mft, const TreeData& tree, uint32_t dir,
             std::string& path) {
    uint32_t slot = tree.dirSlot[dir];
    for (uint32_t c = tree.childOffset[slot]; c < tree.childOffset[slot + 1]; c++) {
        uint32_t idx = tree.children[c];
        const MftEntry& e = mft.entries[idx];
        size_t len = path.size();
        path += EntryName(mft, e);
        uint32_t childSlot = tree.dirSlot[idx];
        CsvField(f, path);
        if (childSlot != NO_DIR) {
            const TreeData::Agg& a = tree.agg[childSlot];
            fprintf(f, ",1,%llu,%llu,%llu,%u,%u\n", (unsigned long long)a.size,
                    (unsigned long long)a.allocated, (unsigned long long)ToUnix(e.mtime),
                    a.files, a.dirs);
            path += '\\';
            CsvWalk(f, mft, tree, idx, path);
        } else {
            fprintf(f, ",0,%llu,%llu,%llu,,\n", (unsigned long long)e.size,
                    (unsigned long long)e.allocated, (unsigned long long)ToUnix(e.mtime));
        }
        path.resize(len);
    }
}

int Usage() {
    printf("wizroot - NTFS disk space analyzer (reads the MFT directly, needs admin)\n\n");
    printf("usage: wizroot <path> [options]\n");
    printf("  <path>            drive or directory (C:, C:\\Users, ...)\n");
    printf("  --top N           entries per listing (default 20)\n");
    printf("  --by size|alloc|count   sort key (default size)\n");
    printf("  --depth D         expand directories D levels deep (default 1)\n");
    printf("  --files           list largest files in the subtree instead\n");
    printf("  --older-than N    with --files: only files untouched for N days\n");
    printf("  --match <pat>     with --files: filename wildcard (*.log, *cache*)\n");
    printf("  --min-size S      hide entries below S (500, 10K, 100M, 2G)\n");
    printf("  --find <name>     find entries by name substring\n");
    printf("  --csv <file>      dump every entry under <path> to CSV\n");
    printf("  --save <file>     save scan to a snapshot after querying\n");
    printf("  --load <file>     query a snapshot instead of scanning (no admin needed)\n");
    printf("  --json            machine-readable output\n");
    return 1;
}

bool ParseArgs(int argc, char** argv, Options& opt) {
    if (argc < 2)
        return false;
    opt.path = argv[1];
    for (int i = 2; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--top") == 0 && i + 1 < argc)
            opt.top = uint32_t(atoi(argv[++i]));
        else if (strcmp(a, "--depth") == 0 && i + 1 < argc)
            opt.depth = uint32_t(atoi(argv[++i]));
        else if (strcmp(a, "--by") == 0 && i + 1 < argc) {
            const char* v = argv[++i];
            if (strcmp(v, "size") == 0)
                opt.by = 0;
            else if (strcmp(v, "alloc") == 0)
                opt.by = 1;
            else if (strcmp(v, "count") == 0)
                opt.by = 2;
            else
                return false;
        } else if (strcmp(a, "--match") == 0 && i + 1 < argc) {
            opt.match = argv[++i];
        } else if (strcmp(a, "--min-size") == 0 && i + 1 < argc) {
            opt.minSize = ParseSize(argv[++i]);
        } else if (strcmp(a, "--older-than") == 0 && i + 1 < argc) {
            // FILETIME ticks are 100ns
            opt.mtimeCutoff = Now() - uint64_t(atoi(argv[++i])) * 864000000000ULL;
        } else if (strcmp(a, "--files") == 0)
            opt.files = true;
        else if (strcmp(a, "--json") == 0)
            opt.json = true;
        else if (strcmp(a, "--find") == 0 && i + 1 < argc)
            opt.find = argv[++i];
        else if (strcmp(a, "--csv") == 0 && i + 1 < argc)
            opt.csv = argv[++i];
        else if (strcmp(a, "--save") == 0 && i + 1 < argc)
            opt.save = argv[++i];
        else if (strcmp(a, "--load") == 0 && i + 1 < argc)
            opt.load = argv[++i];
        else
            return false;
    }
    if (opt.match) {
        int n = MultiByteToWideChar(CP_UTF8, 0, opt.match, -1, nullptr, 0);
        opt.matchW.resize(n ? n - 1 : 0);
        MultiByteToWideChar(CP_UTF8, 0, opt.match, -1, opt.matchW.data(), n);
    }
    return opt.top > 0 && opt.depth > 0;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    Options opt;
    if (!ParseArgs(argc, argv, opt))
        return Usage();

    char drive = char(toupper(uint8_t(opt.path[0])));
    if (!isalpha(uint8_t(drive)) || opt.path[1] != ':') {
        fprintf(stderr, "error: path must start with a drive letter (C: or C:\\...)\n");
        return 1;
    }

    auto t0 = std::chrono::steady_clock::now();
    MftData mft;
    std::string err;
    uint64_t snapAge = 0; // FILETIME ticks since snapshot was taken
    if (opt.load) {
        char snapDrive = 0;
        uint64_t taken = 0;
        if (!LoadSnapshot(opt.load, mft, snapDrive, taken, err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        if (snapDrive != drive) {
            fprintf(stderr, "error: snapshot is of %c:, query path is on %c:\n", snapDrive,
                    drive);
            return 1;
        }
        snapAge = Now() - taken;
    } else if (!ReadMft(drive, mft, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (opt.save && !SaveSnapshot(opt.save, mft, drive, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    TreeData tree;
    BuildTree(mft, tree);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();

    uint32_t scope = FindDir(mft, tree, opt.path);
    if (scope == NO_DIR) {
        fprintf(stderr, "error: directory not found: %s\n", opt.path);
        return 1;
    }
    const TreeData::Agg& top = tree.agg[tree.dirSlot[scope]];

    if (opt.csv) {
        FILE* f = nullptr;
        if (fopen_s(&f, opt.csv, "wb") != 0 || !f) {
            fprintf(stderr, "error: cannot write %s\n", opt.csv);
            return 1;
        }
        fprintf(f, "path,dir,size,allocated,mtime,files,dirs\n");
        std::string path = BuildPath(mft, scope, drive);
        if (path.back() != '\\')
            path += '\\';
        CsvWalk(f, mft, tree, scope, path);
        fclose(f);
        printf("csv written: %s (%u files, %u dirs)\n", opt.csv, top.files, top.dirs);
        return 0;
    }

    if (opt.find) {
        // Substring name search across the whole volume, results scoped
        // by path prefix match on the rebuilt path.
        std::string prefix = BuildPath(mft, scope, drive);
        std::string needle = opt.find;
        CharLowerBuffA(needle.data(), DWORD(needle.size()));
        uint32_t shown = 0;
        if (opt.json)
            printf("[");
        for (uint32_t i = 0; i < mft.entries.size() && shown < opt.top; i++) {
            const MftEntry& e = mft.entries[i];
            if (!(e.flags & MFT_IN_USE) || e.nameLen == 0)
                continue;
            std::string hay = EntryName(mft, e);
            CharLowerBuffA(hay.data(), DWORD(hay.size()));
            if (hay.find(needle) == std::string::npos)
                continue;
            std::string path = BuildPath(mft, i, drive);
            if (path.compare(0, prefix.size(), prefix) != 0)
                continue;
            uint64_t size = tree.dirSlot[i] != NO_DIR ? tree.agg[tree.dirSlot[i]].size : e.size;
            if (opt.json)
                printf("%s{\"path\":\"%s\",\"dir\":%s,\"size\":%llu}", shown ? "," : "",
                       JsonEscape(path).c_str(),
                       tree.dirSlot[i] != NO_DIR ? "true" : "false", (unsigned long long)size);
            else
                printf("%10s  %s%s\n", Human(size).c_str(), path.c_str(),
                       tree.dirSlot[i] != NO_DIR ? "\\" : "");
            shown++;
        }
        if (opt.json)
            printf("]\n");
        else if (shown == 0)
            printf("no matches for '%s' under %s\n", opt.find, prefix.c_str());
        return 0;
    }

    if (opt.json) {
        printf("{\"path\":\"%s\",\"size\":%llu,\"allocated\":%llu,\"files\":%u,\"dirs\":%u,"
               "\"scanMs\":%lld,",
               JsonEscape(BuildPath(mft, scope, drive)).c_str(), (unsigned long long)top.size,
               (unsigned long long)top.allocated, top.files, top.dirs, (long long)ms);
        if (opt.load)
            printf("\"snapshotAgeSec\":%llu,", (unsigned long long)(snapAge / 10000000));
        char rootPath[4] = {drive, ':', '\\', 0};
        ULARGE_INTEGER freeB{}, totalB{};
        if (GetDiskFreeSpaceExA(rootPath, nullptr, &totalB, &freeB))
            printf("\"volumeTotal\":%llu,\"volumeFree\":%llu,",
                   (unsigned long long)totalB.QuadPart, (unsigned long long)freeB.QuadPart);
        printf(opt.files ? "\"largestFiles\":" : "\"children\":");
        if (opt.files) {
            printf("[");
            bool first = true;
            for (uint32_t idx : TopFiles(mft, tree, scope, opt)) {
                const MftEntry& e = mft.entries[idx];
                printf("%s{\"path\":\"%s\",\"size\":%llu,\"allocated\":%llu,\"mtime\":%llu}",
                       first ? "" : ",", JsonEscape(BuildPath(mft, idx, drive)).c_str(),
                       (unsigned long long)e.size, (unsigned long long)e.allocated,
                       (unsigned long long)ToUnix(e.mtime));
                first = false;
            }
            printf("]");
        } else {
            PrintListingJson(mft, tree, scope, opt, 0);
        }
        printf("}\n");
        return 0;
    }

    char origin[48];
    if (opt.load)
        snprintf(origin, sizeof(origin), "snapshot %.1fh old", double(snapAge) / 36e9);
    else
        snprintf(origin, sizeof(origin), "%lld ms", (long long)ms);
    char rootPath[4] = {drive, ':', '\\', 0};
    ULARGE_INTEGER freeB{}, totalB{};
    GetDiskFreeSpaceExA(rootPath, nullptr, &totalB, &freeB); // live, best effort
    printf("%s  %s (%s on disk)  %u files  %u dirs  [%s, %s free of %s]\n",
           BuildPath(mft, scope, drive).c_str(), Human(top.size).c_str(),
           Human(top.allocated).c_str(), top.files, top.dirs, origin,
           Human(freeB.QuadPart).c_str(), Human(totalB.QuadPart).c_str());
    if (opt.files) {
        for (uint32_t idx : TopFiles(mft, tree, scope, opt)) {
            const MftEntry& e = mft.entries[idx];
            printf("%10s  %s  %s\n", Human(opt.by == 1 ? e.allocated : e.size).c_str(),
                   IsoDate(e.mtime).c_str(), BuildPath(mft, idx, drive).c_str());
        }
    } else {
        PrintListing(mft, tree, scope, opt.by == 1 ? top.allocated : top.size, opt, 0);
    }
    return 0;
}
