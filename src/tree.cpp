#include "tree.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

// Live entry with a live directory parent? Root is its own parent - not
// a child of anything. The sequence check rejects stale refs: if the
// parent record was deleted and recycled, its sequence bumped and no
// longer matches what our $FILE_NAME captured.
inline bool HasLiveParent(const MftData& mft, const TreeData& tree, uint32_t i) {
    uint32_t p = mft.entries[i].parent;
    return i != MFT_ROOT && p != i && p < mft.entries.size() && tree.dirSlot[p] != NO_DIR &&
           mft.entries[p].seq == mft.entries[i].parentSeq;
}

} // namespace

void BuildTree(const MftData& mft, TreeData& tree) {
    const auto& entries = mft.entries;
    const uint32_t n = uint32_t(entries.size());

    tree.dirSlot.assign(n, NO_DIR);
    uint32_t dirCount = 0;
    for (uint32_t i = 0; i < n; i++) {
        if ((entries[i].flags & (MFT_IN_USE | MFT_DIR)) == (MFT_IN_USE | MFT_DIR))
            tree.dirSlot[i] = dirCount++;
    }
    tree.agg.assign(dirCount, {});

    // CSR children: count per parent, prefix-sum into offsets, then fill.
    tree.childOffset.assign(dirCount + 1, 0);
    for (uint32_t i = 0; i < n; i++) {
        if (!(entries[i].flags & MFT_IN_USE))
            continue;
        if (HasLiveParent(mft, tree, i))
            tree.childOffset[tree.dirSlot[entries[i].parent] + 1]++;
        else if (i != MFT_ROOT)
            tree.orphans++;
    }
    for (uint32_t s = 0; s < dirCount; s++)
        tree.childOffset[s + 1] += tree.childOffset[s];

    tree.children.resize(tree.childOffset[dirCount]);
    std::vector<uint32_t> cursor(tree.childOffset.begin(), tree.childOffset.end() - 1);
    for (uint32_t i = 0; i < n; i++) {
        if ((entries[i].flags & MFT_IN_USE) && HasLiveParent(mft, tree, i))
            tree.children[cursor[tree.dirSlot[entries[i].parent]]++] = i;
    }

    // Roll up: each entry adds itself to every ancestor. Depth cap guards
    // corrupt parent chains that cycle without passing through root.
    for (uint32_t i = 0; i < n; i++) {
        const MftEntry& e = entries[i];
        if (!(e.flags & MFT_IN_USE))
            continue;
        // A dir's own bytes (index blocks, ADS) belong in its own subtree
        // total too, not just its ancestors'.
        if (tree.dirSlot[i] != NO_DIR) {
            tree.agg[tree.dirSlot[i]].size += e.size;
            tree.agg[tree.dirSlot[i]].allocated += e.allocated;
        }
        if (i == MFT_ROOT || !HasLiveParent(mft, tree, i))
            continue;
        uint32_t p = e.parent;
        for (int depth = 0; depth < 1024; depth++) {
            TreeData::Agg& a = tree.agg[tree.dirSlot[p]];
            a.size += e.size;
            a.allocated += e.allocated;
            if (e.flags & MFT_DIR)
                a.dirs++;
            else
                a.files++;
            if (p == MFT_ROOT || !HasLiveParent(mft, tree, p))
                break;
            p = entries[p].parent;
        }
    }
}

uint32_t FindDir(const MftData& mft, const TreeData& tree, const char* path) {
    // To UTF-16 once - names in the arena are UTF-16, compare there.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wpath(wlen ? wlen - 1 : 0, 0);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);

    const wchar_t* p = wpath.c_str();
    if (iswalpha(p[0]) && p[1] == L':')
        p += 2; // strip drive prefix

    uint32_t cur = MFT_ROOT;
    while (*p) {
        while (*p == L'\\' || *p == L'/')
            p++;
        if (!*p)
            break;
        const wchar_t* start = p;
        while (*p && *p != L'\\' && *p != L'/')
            p++;
        int compLen = int(p - start);

        uint32_t slot = tree.dirSlot[cur];
        uint32_t found = NO_DIR;
        for (uint32_t c = tree.childOffset[slot]; c < tree.childOffset[slot + 1]; c++) {
            const MftEntry& e = mft.entries[tree.children[c]];
            if (tree.dirSlot[tree.children[c]] == NO_DIR)
                continue; // path components must be directories
            if (CompareStringOrdinal(mft.names.data() + e.nameOffset, e.nameLen, start,
                                     compLen, TRUE) == CSTR_EQUAL) {
                found = tree.children[c];
                break;
            }
        }
        if (found == NO_DIR)
            return NO_DIR;
        cur = found;
    }
    return cur;
}
