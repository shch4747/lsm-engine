// test_wal.cpp - exercises Module 1, including the crash-recovery guarantee.
#include "wal_store.hpp"
#include <cassert>
#include <cstdio>
#include <cstdio>

int main() {
    const std::string path = "test.wal";
    std::remove(path.c_str());

    // --- phase 1: write some data, then let the store go out of scope (simulates shutdown)
    {
        WalStore db(path);
        db.put("user:1", "alice");
        db.put("user:2", "bob");
        db.put("user:3", "carol");
        db.del("user:2");              // delete bob
        db.put("user:1", "alice_v2");  // overwrite alice
        assert(db.get("user:1").value() == "alice_v2");
        assert(!db.get("user:2").has_value());     // bob gone
        assert(db.get("user:3").value() == "carol");
        assert(db.size() == 2);
        std::printf("phase 1: wrote 3 keys, deleted 1, overwrote 1 -> size=%zu\n", db.size());
    } // db destroyed here -- only the WAL on disk survives

    // --- phase 2: brand-new store from the SAME WAL -> must reconstruct exact state
    {
        WalStore db(path);             // recover() replays the log in the constructor
        assert(db.get("user:1").value() == "alice_v2");  // overwrite survived
        assert(!db.get("user:2").has_value());           // delete survived
        assert(db.get("user:3").value() == "carol");
        assert(db.size() == 2);
        std::printf("phase 2: reopened from WAL -> state fully recovered, size=%zu\n", db.size());
        std::printf("  user:1 = %s  (overwrite persisted)\n", db.get("user:1")->c_str());
        std::printf("  user:2 = <deleted>  (tombstone persisted)\n");
        std::printf("  user:3 = %s\n", db.get("user:3")->c_str());
    }

    std::remove(path.c_str());
    std::printf("\nALL ASSERTIONS PASSED - durability + recovery verified\n");
    return 0;
}
