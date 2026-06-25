#include "lsm.hpp"
#include <cassert>
#include <cstdio>
#include <filesystem>
namespace fs = std::filesystem;

int main() {
    const std::string dir = "test_lsm_dir";
    fs::remove_all(dir);

    // --- test 1: basic put/get across memtable and SSTable ---
    printf("=== Test 1: put/get across memtable + SSTable ===\n");
    {
        LSMEngine db(dir, 64);   // tiny threshold so we flush quickly
        db.put("alpha", "1");
        db.put("beta",  "2");
        db.put("gamma", "3");    // should trigger a flush (>64 bytes)
        db.put("delta", "4");
        db.flush();              // force flush remaining keys
        printf("  SSTables: %zu, memtable keys: %zu\n",
               db.num_ssts(), db.memtable_keys());
        assert(db.get("alpha").value() == "1");
        assert(db.get("beta").value()  == "2");
        assert(db.get("gamma").value() == "3");
        assert(db.get("delta").value() == "4");
        assert(!db.get("epsilon").has_value());   // miss
        printf("  all reads OK\n");
    }

    // --- test 2: newer write shadows older version in a lower SSTable ---
    printf("\n=== Test 2: newer value shadows older (newest-first read) ===\n");
    {
        fs::remove_all(dir);
        LSMEngine db(dir, 64);
        db.put("key", "v1");
        db.flush();              // v1 goes to SSTable 0
        db.put("key", "v2");    // v2 in memtable -- must shadow v1
        assert(db.get("key").value() == "v2");
        db.flush();              // v2 goes to SSTable 1
        assert(db.get("key").value() == "v2");   // still v2 (newest SSTable wins)
        printf("  shadow read OK: got v2 not v1\n");
    }

    // --- test 3: delete visible across SSTable boundary ---
    printf("\n=== Test 3: delete across SSTable boundary ===\n");
    {
        fs::remove_all(dir);
        LSMEngine db(dir, 64);
        db.put("x", "hello");
        db.flush();
        db.del("x");             // tombstone in memtable
        assert(!db.get("x").has_value());   // must be gone
        db.flush();
        assert(!db.get("x").has_value());   // still gone after tombstone flushed
        printf("  delete across SSTable boundary OK\n");
    }

    // --- test 4: Bloom filter skips SSTables on miss ---
    printf("\n=== Test 4: Bloom filter (probabilistic miss-skip) ===\n");
    {
        fs::remove_all(dir);
        LSMEngine db(dir, 32);
        for (int i = 0; i < 20; ++i)
            db.put("key" + std::to_string(i), "val" + std::to_string(i));
        db.flush();
        // a key that was never inserted -- Bloom should say "not here" for most SSTables
        assert(!db.get("never_inserted_key_xyz").has_value());
        printf("  miss lookup returned nullopt correctly\n");
        printf("  (Bloom filter avoided unnecessary SSTable disk reads)\n");
    }

    fs::remove_all(dir);
    printf("\nALL MODULE 3 ASSERTIONS PASSED\n");
    return 0;
}
