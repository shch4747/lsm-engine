// test_sstable.cpp - verifies Module 2: memtable flush + SSTable read
#include "sstable.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
namespace fs = std::filesystem;

int main() {
    const std::string dir = "test_sst_dir";
    fs::create_directories(dir);

    // --- test 1: write + flush a memtable, read back via SSTableReader ---
    printf("=== Test 1: memtable flush + SSTable point reads ===\n");
    {
        Memtable mt(1024 * 1024);   // large threshold so it doesn't auto-flush
        mt.put("alpha", "1");
        mt.put("gamma", "3");
        mt.put("beta",  "2");       // inserted out of order -- SSTable must sort
        mt.put("delta", "4");
        mt.del("beta");             // soft delete -- tombstone in SSTable

        assert(!mt.needs_flush());
        std::string sst_path = mt.flush(dir, 0);
        printf("  flushed %s\n", sst_path.c_str());
        assert(mt.num_keys() == 0 && mt.size_bytes() == 0);  // memtable reset

        SSTableReader r(sst_path);
        printf("  SSTable has %zu records\n", r.num_records());
        assert(r.num_records() == 4);   // alpha, beta(tomb), gamma, delta

        assert(r.get("alpha").value() == "1");
        assert(r.get("gamma").value() == "3");
        assert(r.get("delta").value() == "4");
        assert(r.get("beta").value()  == SSTableWriter::TOMBSTONE_VAL);  // tombstone
        assert(!r.get("epsilon").has_value());   // not in SSTable -> nullopt
        printf("  point reads OK (including tombstone and miss)\n");
    }

    // --- test 2: flush threshold triggers ---
    printf("\n=== Test 2: flush threshold ===\n");
    {
        Memtable mt(50);    // tiny threshold: 50 bytes
        mt.put("k1", std::string(30, 'x'));   // 32 bytes -> below threshold
        assert(!mt.needs_flush());
        mt.put("k2", std::string(30, 'y'));   // now 64 bytes -> above threshold
        assert(mt.needs_flush());
        printf("  threshold triggered correctly at %zu bytes\n", mt.size_bytes());
        mt.flush(dir, 1);
        assert(!mt.needs_flush());             // reset after flush
        printf("  memtable reset after flush OK\n");
    }

    // --- test 3: multiple SSTables, each with sorted keys ---
    printf("\n=== Test 3: two SSTables, both sorted ===\n");
    {
        Memtable mt(1024*1024);
        mt.put("zoo", "z"); mt.put("ant", "a"); mt.put("moo", "m");
        std::string p0 = mt.flush(dir, 2);
        mt.put("cat", "c"); mt.put("bat", "b");
        std::string p1 = mt.flush(dir, 3);

        SSTableReader r0(p0), r1(p1);
        // r0 should have ant, moo, zoo (sorted); r1 should have bat, cat
        assert(r0.get("ant")->front() == 'a');
        assert(r0.get("zoo")->front() == 'z');
        assert(r1.get("bat")->front() == 'b');
        assert(r1.get("cat")->front() == 'c');
        assert(!r0.get("cat").has_value());    // cat only in r1
        printf("  SSTable 0: %zu records, SSTable 1: %zu records\n",
               r0.num_records(), r1.num_records());
        printf("  cross-SSTable isolation OK\n");
    }

    fs::remove_all(dir);
    printf("\nALL MODULE 2 ASSERTIONS PASSED\n");
    return 0;
}
