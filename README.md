# WizRoot

Disk space analyzer for Windows that reads the NTFS Master File Table directly
instead of walking directories. Scans millions of files in seconds. CLI only,
built to be equally usable by humans and AI agents.

Inspired by WizTree. Not affiliated with it.

## Why

Directory walking chokes on big volumes. Reading the MFT is one sequential read:

|                                                  | files | time            |
|--------------------------------------------------|-------|-----------------|
| wizroot, full scan of C:                         | 6.7M  | ~8 s            |
| wizroot, query from snapshot                     | 6.7M  | ~1.5 s, no admin |
| PowerShell Get-ChildItem -Recurse, one subfolder | 2.9M  | 25+ min         |

Measured on a live 1TB NVMe volume with 7.8M MFT records.

## Usage

Scanning needs admin (raw volume access). Querying a snapshot doesn't.

```
wizroot C:                            top 20 entries by size
wizroot C:\Users --by count           dirs hoarding the most files
wizroot C: --files --top 50           biggest files on the volume
wizroot C: --files --older-than 365 --min-size 100M
                                      big files nobody touched in a year
wizroot C: --files --match *.log      biggest logs
wizroot C:\Users --depth 2            two levels deep, top 20 each
wizroot C: --find node_modules        locate entries by name substring
wizroot C: --json                     same data, machine readable
wizroot C: --csv dump.csv             full recursive dump
```

Snapshot workflow, scan once as admin then query forever without:

```
wizroot C: --save c.wzr
wizroot C:\Users --load c.wzr --files --top 50
```

## Agent usage

Built for this first. Output is always bounded (top-N per level, never a
million-line dump), everything has a `--json` form, and snapshots make repeat
queries cost about a second with no elevation. Typical flow: one elevated
scan with `--save`, then drill down with as many `--load` queries as needed.

## How it works

Opens the raw volume, reads MFT record 0 to get the MFT's own extent map,
then streams the whole table in 8MB chunks and parses every FILE record:
names and parent references from `$FILE_NAME`, sizes from `$DATA` (all
streams, so ADS counts), directory index blocks from `$INDEX_ALLOCATION`.
Sparse and compressed streams get their run lists summed because their
header sizes lie. Parent references are validated against sequence numbers
so recycled records don't misattach subtrees. The result is a flat array
plus one name arena, aggregated bottom-up into per-directory subtree totals.

## Accuracy

Byte-exact against a PowerShell recursive sum on link-free trees. Volume
totals land within ~0.5% of what Windows reports as used; the remainder is
filesystem metadata that isn't file data (index bitmaps, security store).
Hardlinked files count once and show under their first non-DOS name.

## Building

```
cmake -S . -B build
cmake --build build --config Release
```

VS 2022 opens the folder directly. No dependencies, plain Win32 API.

## Limits

- Windows and NTFS only, by design. The MFT is the entire trick.
- Volumes only ($MFT is per-volume); scan each drive separately.
- Junction/symlink dirs are flagged `[reparse]` but not followed - their
  targets are counted where they really live.
