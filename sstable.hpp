// sstable.hpp - Module 2 of the LSM storage engine
//
// An SSTable (Sorted String Table) is an IMMUTABLE on-disk file of key-value
// pairs in SORTED order. Two things make it the cornerstone of LSM trees:
//
//   SORTED  -> binary search for point reads; merge-sort for compaction.
//   IMMUTABLE -> no in-place updates ever; writes are always sequential appends,
//               which is the fastest thing a disk can do. Mutations go to a new
//               file (or are resolved at read time by checking newer files first).
//
// File format:
//   [data block]  N records: [klen:4][key][vlen:4][value][is_tombstone:1]
//   [index block] N entries: [klen:4][key][offset:8]   <- byte offset of record
//   [footer]      [index_offset:8][num_records:8]       <- always last 16 bytes
//
// The index lets SSTableReader do a BINARY SEARCH over keys without scanning
// the whole file -- essential once SSTables grow large.
//
// Tombstones: deletes are stored as records with is_tombstone=1. This is why
// LSM deletes are "soft" -- the key isn't gone until compaction (Module 4)
// drops the tombstone after confirming no older versions exist below it.

#pragma once
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <optional>
#include <cstdint>
#include <stdexcept>
#include <algorithm>

// One entry in the in-memory index: key -> byte offset of its record on disk.
struct IndexEntry { std::string key; uint64_t offset; };

// -----------------------------------------------------------------------
// SSTableWriter: flush a sorted memtable snapshot to a new SSTable file.
// -----------------------------------------------------------------------
class SSTableWriter {
public:
    // mem  : the memtable contents (already sorted -- std::map guarantees this)
    // dels : set of deleted keys (written as tombstones)
    // path : output file path
    static void write(const std::map<std::string,std::string>& mem,
                      const std::string& path) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot create SSTable: " + path);

        std::vector<IndexEntry> index;
        uint64_t offset = 0;

        // --- data block: iterate in sorted order (std::map is sorted by key) ---
        for (const auto& [k, v] : mem) {
            index.push_back({k, offset});

            uint32_t klen = k.size(), vlen = v.size();
            uint8_t  tomb = 0;                          // 0 = live, 1 = tombstone

            // detect tombstone: we encode deletes as value == "\x00TOMBSTONE"
            if (v == TOMBSTONE_VAL) { vlen = 0; tomb = 1; }

            f.write(reinterpret_cast<const char*>(&klen), 4);
            f.write(k.data(), klen);
            f.write(reinterpret_cast<const char*>(&vlen), 4);
            if (!tomb) f.write(v.data(), vlen);
            f.write(reinterpret_cast<const char*>(&tomb), 1);

            offset += 4 + klen + 4 + (tomb ? 0 : vlen) + 1;
        }

        // --- index block ---
        uint64_t index_offset = offset;
        for (const auto& ie : index) {
            uint32_t klen = ie.key.size();
            f.write(reinterpret_cast<const char*>(&klen), 4);
            f.write(ie.key.data(), klen);
            f.write(reinterpret_cast<const char*>(&ie.offset), 8);
        }

        // --- footer: index_offset (8) + num_records (8) = 16 bytes ---
        uint64_t num = index.size();
        f.write(reinterpret_cast<const char*>(&index_offset), 8);
        f.write(reinterpret_cast<const char*>(&num), 8);
        f.flush();
    }

    static const std::string TOMBSTONE_VAL;
};
const std::string SSTableWriter::TOMBSTONE_VAL = "\x00TOMBSTONE";

// -----------------------------------------------------------------------
// SSTableReader: point-lookup via binary search over the in-memory index.
// -----------------------------------------------------------------------
class SSTableReader {
public:
    explicit SSTableReader(const std::string& path) : path_(path) {
        load_index();
    }

    // Returns nullopt if key not in this SSTable.
    // Returns TOMBSTONE_VAL if the key was deleted (caller must handle).
    std::optional<std::string> get(const std::string& key) const {
        // binary search the index
        int lo = 0, hi = static_cast<int>(index_.size()) - 1, pos = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (index_[mid].key == key) { pos = mid; break; }
            else if (index_[mid].key < key) lo = mid + 1;
            else hi = mid - 1;
        }
        if (pos < 0) return std::nullopt;  // not in this SSTable

        // seek to the record and read it
        std::ifstream f(path_, std::ios::binary);
        f.seekg(static_cast<std::streamoff>(index_[pos].offset));

        uint32_t klen; f.read(reinterpret_cast<char*>(&klen), 4);
        std::string k(klen, '\0'); f.read(&k[0], klen);
        uint32_t vlen; f.read(reinterpret_cast<char*>(&vlen), 4);
        std::string v(vlen, '\0'); if (vlen) f.read(&v[0], vlen);
        uint8_t tomb; f.read(reinterpret_cast<char*>(&tomb), 1);

        if (tomb) return SSTableWriter::TOMBSTONE_VAL;
        return v;
    }

    size_t num_records() const { return index_.size(); }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::vector<IndexEntry> index_;

    void load_index() {
        std::ifstream f(path_, std::ios::binary);
        if (!f) throw std::runtime_error("cannot open SSTable: " + path_);

        // read footer (last 16 bytes)
        f.seekg(-16, std::ios::end);
        uint64_t index_offset, num;
        f.read(reinterpret_cast<char*>(&index_offset), 8);
        f.read(reinterpret_cast<char*>(&num), 8);

        // read index block
        f.seekg(static_cast<std::streamoff>(index_offset));
        index_.resize(num);
        for (auto& ie : index_) {
            uint32_t klen; f.read(reinterpret_cast<char*>(&klen), 4);
            ie.key.resize(klen); f.read(&ie.key[0], klen);
            f.read(reinterpret_cast<char*>(&ie.offset), 8);
        }
    }
};

// -----------------------------------------------------------------------
// Memtable: wraps the in-memory map with a flush-to-SSTable trigger.
// -----------------------------------------------------------------------
class Memtable {
public:
    explicit Memtable(size_t flush_threshold_bytes = 4096)
        : threshold_(flush_threshold_bytes), size_bytes_(0) {}

    void put(const std::string& k, const std::string& v) {
        size_bytes_ += k.size() + v.size();
        mem_[k] = v;
    }
    void del(const std::string& k) {
        size_bytes_ += k.size() + SSTableWriter::TOMBSTONE_VAL.size();
        mem_[k] = SSTableWriter::TOMBSTONE_VAL;  // soft delete
    }
    std::optional<std::string> get(const std::string& k) const {
        auto it = mem_.find(k);
        if (it == mem_.end()) return std::nullopt;
        if (it->second == SSTableWriter::TOMBSTONE_VAL)
            return SSTableWriter::TOMBSTONE_VAL;
        return it->second;
    }

    bool needs_flush() const { return size_bytes_ >= threshold_; }
    size_t size_bytes() const { return size_bytes_; }
    size_t num_keys() const { return mem_.size(); }
    const std::map<std::string,std::string>& data() const { return mem_; }

    // Flush to SSTable and reset -- returns the path of the new SSTable.
    std::string flush(const std::string& dir, int seq) {
        std::string path = dir + "/sst_" + std::to_string(seq) + ".sst";
        SSTableWriter::write(mem_, path);
        mem_.clear(); size_bytes_ = 0;
        return path;
    }

private:
    std::map<std::string,std::string> mem_;
    size_t threshold_, size_bytes_;
};
