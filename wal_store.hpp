// wal_store.hpp - Module 1 of the LSM storage engine
//
// A durable key-value store backed by a write-ahead log (WAL). Every mutation
// is appended to an on-disk log BEFORE it touches the in-memory map, so a crash
// can never lose an acknowledged write -- on restart we replay the log to
// reconstruct exact state. This is the durability foundation the LSM layers
// (memtable flush, SSTables, compaction) build on.
//
// WAL record format (binary, length-prefixed):
//   [op:1][klen:4][key bytes][vlen:4][value bytes]   for PUT
//   [op:1][klen:4][key bytes]                         for DEL
#pragma once
#include <string>
#include <map>
#include <fstream>
#include <optional>
#include <cstdint>
#include <stdexcept>

class WalStore {
public:
    explicit WalStore(const std::string& wal_path) : wal_path_(wal_path) {
        recover();                                   // replay existing log first
        wal_.open(wal_path_, std::ios::binary | std::ios::app);
        if (!wal_) throw std::runtime_error("cannot open WAL: " + wal_path_);
    }

    void put(const std::string& key, const std::string& value) {
        append_put(key, value);                      // 1. durably log it
        mem_[key] = value;                           // 2. then apply in memory
    }

    void del(const std::string& key) {
        append_del(key);
        mem_.erase(key);
    }

    std::optional<std::string> get(const std::string& key) const {
        auto it = mem_.find(key);
        if (it == mem_.end()) return std::nullopt;
        return it->second;
    }

    size_t size() const { return mem_.size(); }

private:
    std::map<std::string, std::string> mem_;
    std::ofstream wal_;
    std::string wal_path_;

    static constexpr uint8_t OP_PUT = 1;
    static constexpr uint8_t OP_DEL = 2;

    void write_u32(std::ostream& os, uint32_t v) {
        os.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    static bool read_u32(std::istream& is, uint32_t& v) {
        return static_cast<bool>(is.read(reinterpret_cast<char*>(&v), sizeof(v)));
    }

    void append_put(const std::string& k, const std::string& v) {
        wal_.put(static_cast<char>(OP_PUT));
        write_u32(wal_, static_cast<uint32_t>(k.size())); wal_.write(k.data(), k.size());
        write_u32(wal_, static_cast<uint32_t>(v.size())); wal_.write(v.data(), v.size());
        wal_.flush();    // push to OS; a production engine would fsync() here for
                         // true crash durability (flush() != fsync()) -- a known tradeoff
    }

    void append_del(const std::string& k) {
        wal_.put(static_cast<char>(OP_DEL));
        write_u32(wal_, static_cast<uint32_t>(k.size())); wal_.write(k.data(), k.size());
        wal_.flush();
    }

    // Replay the WAL from the start, rebuilding the in-memory map exactly.
    void recover() {
        std::ifstream in(wal_path_, std::ios::binary);
        if (!in) return;                             // no log yet -> empty store
        int recovered = 0;
        while (in.peek() != EOF) {
            int op = in.get();
            uint32_t klen;
            if (!read_u32(in, klen)) break;          // torn/partial tail record -> stop
            std::string key(klen, '\0');
            if (!in.read(&key[0], klen)) break;
            if (op == OP_PUT) {
                uint32_t vlen;
                if (!read_u32(in, vlen)) break;
                std::string val(vlen, '\0');
                if (!in.read(&val[0], vlen)) break;
                mem_[key] = val;
            } else if (op == OP_DEL) {
                mem_.erase(key);
            } else {
                break;                               // unknown op -> corrupt, stop
            }
            ++recovered;
        }
        if (recovered) std::fprintf(stderr, "[recover] replayed %d WAL records\n", recovered);
    }
};
