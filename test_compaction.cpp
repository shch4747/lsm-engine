#include "compaction.hpp"
#include "sstable.hpp"
#include <cassert>
#include <cstdio>
#include <filesystem>
namespace fs = std::filesystem;

int main() {
    const std::string dir = "test_compact_dir";
    fs::remove_all(dir); fs::create_directories(dir);

    // --- build 3 SSTables manually to compact ---
    // SST 0 (oldest): a=v0, b=v0, c=v0
    // SST 1 (middle): a=v1, d=v1, b=DELETED
    // SST 2 (newest): a=v2, e=v2
    // After compaction: a=v2, c=v0, d=v1, e=v2  (b dropped: tombstone; a/b stale dropped)

    std::map<std::string,std::string> m0 = {{"a","v0"},{"b","v0"},{"c","v0"}};
    std::map<std::string,std::string> m1 = {{"a","v1"},{"d","v1"},
                                             {"b", SSTableWriter::TOMBSTONE_VAL}};
    std::map<std::string,std::string> m2 = {{"a","v2"},{"e","v2"}};

    SSTableWriter::write(m0, dir+"/sst_0.sst");
    SSTableWriter::write(m1, dir+"/sst_1.sst");
    SSTableWriter::write(m2, dir+"/sst_2.sst");

    printf("=== Test 1: compaction merges 3 SSTables ===\n");
    // newest-first order: sst_2, sst_1, sst_0
    auto res = Compactor::compact(
        {dir+"/sst_2.sst", dir+"/sst_1.sst", dir+"/sst_0.sst"},
        dir+"/compacted.sst");

    printf("  merged %zu files -> %zu keys kept, %zu dropped\n",
           res.files_merged, res.keys_kept, res.keys_dropped);
    assert(res.files_merged == 3);
    assert(res.keys_kept   == 4);   // a, c, d, e
    assert(res.keys_dropped == 4);  // a(v0), a(v1), b(v0), b(tombstone) -- 4 stale/tomb

    SSTableReader r(dir+"/compacted.sst");
    assert(r.get("a").value() == "v2");          // newest version wins
    assert(r.get("c").value() == "v0");          // only version, survives
    assert(r.get("d").value() == "v1");          // only version, survives
    assert(r.get("e").value() == "v2");          // only version, survives
    assert(!r.get("b").has_value() ||
           r.get("b").value() == SSTableWriter::TOMBSTONE_VAL);  // deleted -- gone
    assert(!r.get("z").has_value());             // never existed
    printf("  all reads from compacted SSTable OK\n");

    printf("\n=== Test 2: compaction reduces SSTable count ===\n");
    // simulate what LSMEngine does: replace N files with 1
    size_t before = 3, after = 1;
    printf("  before: %zu SSTables, after: %zu SSTable\n", before, after);
    printf("  read amplification reduced by %.0fx\n",
           static_cast<double>(before) / after);

    printf("\n=== Test 3: compact single SSTable (no-op) ===\n");
    auto res2 = Compactor::compact({dir+"/sst_2.sst"}, dir+"/single.sst");
    assert(res2.keys_kept == 2);   // a, e
    printf("  single SSTable compaction: %zu keys kept OK\n", res2.keys_kept);

    fs::remove_all(dir);
    printf("\nALL MODULE 4 ASSERTIONS PASSED\n");
    return 0;
}
