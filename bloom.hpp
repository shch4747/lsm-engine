// bloom.hpp - a simple Bloom filter for fast negative lookups
//
// A Bloom filter is a bit array of size M with K independent hash functions.
// INSERT: set bits at positions h1(key), h2(key), ..., hk(key).
// QUERY:  if ANY of those bits is 0, the key is DEFINITELY NOT present.
//         if ALL bits are 1, the key is PROBABLY present (may be a false positive).
//
// False negative rate: 0% -- if a key was inserted, all its bits are set.
// False positive rate: (1 - e^(-kn/m))^k where n=keys inserted.
// With m=8*capacity bits and k=3 hash functions, FPR is ~2% at full capacity.
// That means we skip ~98% of unnecessary SSTable disk reads on misses.
#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>

class BloomFilter {
public:
    // capacity: expected number of keys; fpr: target false positive rate
    explicit BloomFilter(size_t capacity = 1000, double fpr = 0.02) {
        // m = -n*ln(p) / (ln2)^2
        size_t m = static_cast<size_t>(
            -static_cast<double>(capacity) * std::log(fpr) /
            (std::log(2.0) * std::log(2.0)));
        bits_.assign((m + 63) / 64, 0);   // round up to 64-bit words
        m_ = m;
        // k = (m/n)*ln2  -- optimal number of hash functions
        k_ = std::max(1u, static_cast<unsigned>(
            static_cast<double>(m) / capacity * std::log(2.0)));
    }

    void insert(const std::string& key) {
        for (unsigned i = 0; i < k_; ++i)
            set_bit(hash(key, i) % m_);
    }

    // Returns false -> key DEFINITELY not present (skip the SSTable).
    // Returns true  -> key PROBABLY present (go read the SSTable).
    bool maybe_contains(const std::string& key) const {
        for (unsigned i = 0; i < k_; ++i)
            if (!get_bit(hash(key, i) % m_)) return false;
        return true;
    }

private:
    std::vector<uint64_t> bits_;
    size_t m_;       // number of bits
    unsigned k_;     // number of hash functions

    void set_bit(size_t i) { bits_[i/64] |=  (1ULL << (i%64)); }
    bool get_bit(size_t i) const { return (bits_[i/64] >> (i%64)) & 1; }

    // Two independent hash functions combined (Kirsch-Mitzenmacher trick):
    // h_i(x) = h1(x) + i * h2(x)  gives k independent hashes from two.
    uint64_t hash(const std::string& key, unsigned seed) const {
        // FNV-1a for h1
        uint64_t h1 = 14695981039346656037ULL;
        for (unsigned char c : key) h1 = (h1 ^ c) * 1099511628211ULL;
        // djb2-variant for h2
        uint64_t h2 = 5381;
        for (unsigned char c : key) h2 = h2 * 33 ^ c;
        return h1 + seed * h2;
    }
};
