// compaction.hpp - Module 4 of the LSM storage engine
//
// Merges all existing SSTables into one new compacted SSTable, then replaces
// them. Two goals:
//
//   SPACE RECLAMATION: older versions of the same key and tombstones for keys
//   that no longer exist in any newer SSTable are dropped entirely.
//
//   READ AMPLIFICATION REDUCTION: instead of checking N SSTables per read,
//   after compaction there is only one. This is the core LSM space-vs-read
//   tradeoff: you pay a write amplification cost (rewriting all data) to
//   recover read performance.
//
// Algorithm: k-way merge of sorted SSTable files (like merge-sort's merge
// phase). We walk all SSTables in NEWEST-to-OLDEST order. For each key we
// encounter, we keep the FIRST (newest) value seen and skip all older copies.
// Tombstones are dropped if no older version exists -- since we process
// newest-first and track seen keys, a tombstone with an already-seen key means
// it was already overwritten by something newer, so we drop it. A tombstone for
// a key we haven't seen yet is also dropped (nothing older to hide).
//
// In production (RocksDB, LevelDB) compaction is levelled or tiered and runs
// continuously in the background. Here we do a simple full compaction triggered
// when the number of SSTables exceeds a threshold -- same correctness guarantees,
// simpler to reason about.

#pragma once
#include "sstable.hpp"
#include "bloom.hpp"
#include <vector>
#include <string>
#include <map>
#include <set>
#include <memory>
#include <filesystem>
#include <cstdio>
namespace fs = std::filesystem;

struct CompactionResult {
    std::string output_path;   // the new compacted SSTable
    size_t keys_kept;          // live keys written
    size_t keys_dropped;       // stale versions + tombstones dropped
    size_t files_merged;       // number of input SSTables
};

class Compactor {
public:
    // sst_paths: SSTable file paths NEWEST-FIRST (caller must order them).
    // out_path : where to write the merged SSTable.
    static CompactionResult compact(
            const std::vector<std::string>& sst_paths,
            const std::string& out_path) {

        if (sst_paths.empty()) return {out_path, 0, 0, 0};

        // Open all readers. We'll do a full scan of each, newest-first.
        std::vector<std::unique_ptr<SSTableReader>> readers;
        for (const auto& p : sst_paths)
            readers.push_back(std::make_unique<SSTableReader>(p));

        // Collect all keys across all SSTables (union, no duplicates needed --
        // we want every unique key so we can find its newest value).
        // Strategy: scan each SSTable's data sequentially by doing a lookup
        // for every key in its index. We gather (key -> newest_value) by
        // processing newest SSTable first and skipping keys already seen.
        std::set<std::string> seen;
        std::map<std::string, std::string> merged;   // key -> value (or TOMBSTONE)
        size_t dropped = 0;

        // scan each SSTable (newest first = sst_paths[0] first)
        for (size_t si = 0; si < sst_paths.size(); ++si) {
            // Read the index to enumerate all keys in this SSTable
            auto keys = read_keys(sst_paths[si]);
            for (const auto& k : keys) {
                if (seen.count(k)) { ++dropped; continue; }   // newer version already kept
                seen.insert(k);
                auto v = readers[si]->get(k);
                if (!v) continue;
                if (*v == SSTableWriter::TOMBSTONE_VAL) {
                    // tombstone: the key was deleted. Drop it (and mark dropped).
                    ++dropped; continue;
                }
                merged[k] = *v;
            }
        }

        // Write the merged result as a new SSTable
        SSTableWriter::write(merged, out_path);

        return {out_path,
                merged.size(),
                dropped,
                sst_paths.size()};
    }

private:
    // Read just the keys from an SSTable's index block (no data reads).
    static std::vector<std::string> read_keys(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        f.seekg(-16, std::ios::end);
        uint64_t idx_off, num;
        f.read(reinterpret_cast<char*>(&idx_off), 8);
        f.read(reinterpret_cast<char*>(&num), 8);
        f.seekg(static_cast<std::streamoff>(idx_off));
        std::vector<std::string> keys;
        keys.reserve(num);
        for (uint64_t i = 0; i < num; ++i) {
            uint32_t klen; f.read(reinterpret_cast<char*>(&klen), 4);
            std::string k(klen, '\0'); f.read(&k[0], klen);
            uint64_t off; f.read(reinterpret_cast<char*>(&off), 8);
            keys.push_back(std::move(k));
        }
        return keys;
    }
};
