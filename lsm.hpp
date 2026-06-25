// lsm.hpp - Module 3: the unified LSM engine read/write interface
//
// Brings together wal_store (durability), sstable (flush/read), and bloom
// (fast miss detection) into a single LSM engine class.
//
// READ PATH (newest-first -- crucial for correctness):
//   1. Check memtable (most recent writes).
//   2. Check SSTables newest-to-oldest, skipping each via its Bloom filter
//      if the filter says "definitely not here."
//   3. First non-nullopt result wins (shadows older versions).
//   4. If the result is a tombstone, the key is deleted -> return nullopt.
//
// WRITE PATH:
//   put/del -> WAL append -> memtable update.
//   When memtable exceeds threshold -> flush to new SSTable + new WAL segment.
//
// This newest-first ordering is why LSM reads can be slow with many SSTables
// (you may check several before finding the key) -- exactly why compaction
// (Module 4) is essential: it merges SSTables and eliminates redundant versions.

#pragma once
#include "wal_store.hpp"
#include "sstable.hpp"
#include "bloom.hpp"
#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <filesystem>
#include <cstdio>
namespace fs = std::filesystem;

struct SSTLevel {
    std::unique_ptr<SSTableReader> reader;
    BloomFilter                    bloom;
    int                            seq;
    SSTLevel(const std::string& path, int s, size_t n_keys)
        : reader(std::make_unique<SSTableReader>(path)), bloom(n_keys), seq(s) {}
};

class LSMEngine {
public:
    explicit LSMEngine(const std::string& dir,
                       size_t memtable_bytes = 4096)
        : dir_(dir), flush_seq_(0), memtable_(memtable_bytes)
    {
        fs::create_directories(dir);
        wal_ = std::make_unique<WalStore>(dir + "/wal.log");
        load_existing_ssts();
    }

    void put(const std::string& key, const std::string& value) {
        wal_->put(key, value);
        memtable_.put(key, value);
        maybe_flush();
    }

    void del(const std::string& key) {
        wal_->del(key);
        memtable_.del(key);
        maybe_flush();
    }

    std::optional<std::string> get(const std::string& key) const {
        // 1. memtable -- most recent
        auto v = memtable_.get(key);
        if (v.has_value()) {
            if (*v == SSTableWriter::TOMBSTONE_VAL) return std::nullopt;
            return v;
        }
        // 2. SSTables newest-to-oldest
        for (int i = static_cast<int>(ssts_.size()) - 1; i >= 0; --i) {
            const auto& lvl = ssts_[i];
            if (!lvl.bloom.maybe_contains(key)) {
                continue;              // Bloom says definitely not here -- skip disk read
            }
            auto sv = lvl.reader->get(key);
            if (!sv.has_value()) continue;
            if (*sv == SSTableWriter::TOMBSTONE_VAL) return std::nullopt;
            return sv;
        }
        return std::nullopt;           // not found anywhere
    }

    // Force a memtable flush (useful for testing / shutdown)
    void flush() {
        if (memtable_.num_keys() == 0) return;
        do_flush();
    }

    size_t num_ssts()    const { return ssts_.size(); }
    size_t memtable_keys() const { return memtable_.num_keys(); }

private:
    std::string dir_;
    int flush_seq_;
    Memtable memtable_;
    std::unique_ptr<WalStore> wal_;
    std::vector<SSTLevel> ssts_;

    void maybe_flush() { if (memtable_.needs_flush()) do_flush(); }

    void do_flush() {
        size_t n = memtable_.num_keys();
        std::string path = memtable_.flush(dir_, flush_seq_);
        // build Bloom filter for the new SSTable
        SSTLevel lvl(path, flush_seq_, n);
        SSTableReader tmp(path);
        // re-read keys to populate the Bloom filter
        // (in production you'd pass the key set directly; this keeps it simple)
        // populate bloom from the SSTable index (via a fresh reader scan)
        // We keep it simple: read back the index and insert every key
        {
            SSTableReader rdr(path);
            // SSTableReader exposes its index through get; here we rebuild
            // the bloom by re-iterating the data from the original memtable snapshot.
            // (The memtable was cleared by flush(), so we read from the SSTable.)
            // Simpler: just mark all keys from the file's index -- achieved by
            // inserting the path's keys via a helper.
            populate_bloom(lvl.bloom, path, rdr.num_records());
        }
        flush_seq_++;
        // rotate WAL: close current, open fresh segment
        wal_ = std::make_unique<WalStore>(
            dir_ + "/wal_" + std::to_string(flush_seq_) + ".log");
        ssts_.push_back(std::move(lvl));
        std::fprintf(stderr, "[flush] flushed %zu keys -> %s (%zu SSTables total)\n",
                     n, path.c_str(), ssts_.size());
    }

    // Walk the SSTable file to populate a Bloom filter with all its keys.
    static void populate_bloom(BloomFilter& bf, const std::string& path, size_t /*n*/) {
        std::ifstream f(path, std::ios::binary);
        // read footer
        f.seekg(-16, std::ios::end);
        uint64_t idx_off, num;
        f.read(reinterpret_cast<char*>(&idx_off), 8);
        f.read(reinterpret_cast<char*>(&num), 8);
        // read index keys and insert into Bloom
        f.seekg(static_cast<std::streamoff>(idx_off));
        for (uint64_t i = 0; i < num; ++i) {
            uint32_t klen; f.read(reinterpret_cast<char*>(&klen), 4);
            std::string key(klen, '\0'); f.read(&key[0], klen);
            uint64_t off; f.read(reinterpret_cast<char*>(&off), 8);
            bf.insert(key);
        }
    }

    // On startup, load any SSTable files left from a previous run.
    void load_existing_ssts() {
        std::vector<std::pair<int,std::string>> found;
        for (const auto& e : fs::directory_iterator(dir_)) {
            std::string name = e.path().filename().string();
            if (name.rfind("sst_", 0) == 0 && name.size() > 8) {
                // sst_<seq>.sst
                try {
                    int seq = std::stoi(name.substr(4, name.size() - 8));
                    found.push_back({seq, e.path().string()});
                } catch (...) {}
            }
        }
        std::sort(found.begin(), found.end());
        for (auto& [seq, path] : found) {
            SSTableReader rdr(path);
            SSTLevel lvl(path, seq, rdr.num_records());
            populate_bloom(lvl.bloom, path, rdr.num_records());
            flush_seq_ = std::max(flush_seq_, seq + 1);
            ssts_.push_back(std::move(lvl));
        }
        if (!found.empty())
            std::fprintf(stderr, "[startup] loaded %zu existing SSTables\n", found.size());
    }
};
