// benchmark.cpp - Module 5 of the LSM storage engine
//
// Measures throughput (ops/sec) and latency percentiles (p50/p95/p99 in µs)
// for sequential writes, random writes, and random reads.
//
// Why these three workloads?
//   Sequential writes: best case for LSM (memtable fills in sorted order,
//     flushes are cheap). Shows the ceiling.
//   Random writes: realistic workload. LSM still wins here vs B-tree because
//     writes always go to memory first -- random disk seeks are avoided.
//   Random reads: stress test for the read path. Mix of hits (key exists)
//     and misses (key never inserted) -- misses exercise the Bloom filter.
//
// Compile: g++ -std=c++17 -O2 -Wall -o bench benchmark.cpp
// Run:     ./bench [n_ops]   (default: 50000)

#include "lsm.hpp"
#include "compaction.hpp"
#include <vector>
#include <algorithm>
#include <chrono>
#include <random>
#include <cstdio>
#include <string>
#include <filesystem>
namespace fs = std::filesystem;
using Clock = std::chrono::high_resolution_clock;
using US    = std::chrono::microseconds;

// ---- timing helpers ----
struct Stats {
    double   throughput;   // ops / second
    double   p50, p95, p99, mean;   // microseconds
};

Stats compute(std::vector<double>& latencies, double elapsed_sec) {
    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();
    double sum = 0; for (auto v : latencies) sum += v;
    return {
        n / elapsed_sec,
        latencies[n * 50 / 100],
        latencies[n * 95 / 100],
        latencies[n * 99 / 100],
        sum / n
    };
}

void print_stats(const char* label, const Stats& s) {
    printf("  %-22s  tput=%8.0f ops/s  "
           "p50=%6.1f µs  p95=%7.1f µs  p99=%8.1f µs  mean=%6.1f µs\n",
           label, s.throughput, s.p50, s.p95, s.p99, s.mean);
}

// ---- workloads ----
Stats bench_seq_write(LSMEngine& db, int n) {
    std::vector<double> lat; lat.reserve(n);
    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) {
        std::string k = "key_" + std::to_string(i);
        std::string v = "val_" + std::to_string(i);
        auto s = Clock::now();
        db.put(k, v);
        lat.push_back(std::chrono::duration<double, std::micro>(Clock::now()-s).count());
    }
    double elapsed = std::chrono::duration<double>(Clock::now()-t0).count();
    return compute(lat, elapsed);
}

Stats bench_rand_write(LSMEngine& db, int n, std::mt19937& rng) {
    std::uniform_int_distribution<int> dist(0, n * 10);
    std::vector<double> lat; lat.reserve(n);
    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) {
        std::string k = "rkey_" + std::to_string(dist(rng));
        std::string v = "rval_" + std::to_string(i);
        auto s = Clock::now();
        db.put(k, v);
        lat.push_back(std::chrono::duration<double, std::micro>(Clock::now()-s).count());
    }
    double elapsed = std::chrono::duration<double>(Clock::now()-t0).count();
    return compute(lat, elapsed);
}

Stats bench_rand_read(LSMEngine& db, int n, std::mt19937& rng) {
    // 70% hits (keys we wrote), 30% misses (keys that don't exist)
    std::uniform_int_distribution<int> dist(0, n * 10);
    std::uniform_int_distribution<int> hit_dist(0, n - 1);
    std::vector<double> lat; lat.reserve(n);
    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) {
        std::string k;
        if (dist(rng) % 10 < 7)
            k = "key_" + std::to_string(hit_dist(rng));   // likely hit
        else
            k = "miss_" + std::to_string(dist(rng));       // definite miss
        auto s = Clock::now();
        (void)db.get(k);
        lat.push_back(std::chrono::duration<double, std::micro>(Clock::now()-s).count());
    }
    double elapsed = std::chrono::duration<double>(Clock::now()-t0).count();
    return compute(lat, elapsed);
}

int main(int argc, char* argv[]) {
    int n = (argc > 1) ? std::stoi(argv[1]) : 50000;
    const std::string dir = "bench_dir";
    fs::remove_all(dir);

    // memtable threshold: 256KB -- realistic small value so we get multiple flushes
    Stats sw, rw, rr;
    size_t num_ssts = 0;
    std::vector<std::string> sst_paths;

    // scope the engine so the WAL is closed before we remove files (Windows)
    {
        LSMEngine db(dir, 256 * 1024);
        std::mt19937 rng(42);

        printf("LSM Storage Engine Benchmark\n");
        printf("ops per workload: %d\n", n);
        printf("%-24s  %s\n", "", "throughput        p50        p95         p99       mean");
        printf("%s\n", std::string(90, '-').c_str());

        sw = bench_seq_write(db, n);
        print_stats("sequential write", sw);

        rw = bench_rand_write(db, n, rng);
        print_stats("random write", rw);

        db.flush();
        num_ssts = db.num_ssts();
        printf("  [%zu SSTables on disk after writes]\n", num_ssts);

        rr = bench_rand_read(db, n, rng);
        print_stats("random read (70% hit)", rr);
        printf("%s\n", std::string(90, '-').c_str());

        for (size_t i = 0; i < num_ssts; ++i)
            sst_paths.push_back(dir + "/sst_" + std::to_string(i) + ".sst");
        std::reverse(sst_paths.begin(), sst_paths.end());
    } // LSMEngine destroyed here -> WAL file closed

    // --- compaction benchmark ---
    printf("\nCompaction:\n");
    auto ct0 = Clock::now();
    auto cr = Compactor::compact(sst_paths, dir + "/compacted.sst");
    double ct = std::chrono::duration<double>(Clock::now()-ct0).count();
    printf("  merged %zu SSTables -> %zu keys kept, %zu dropped in %.2fs\n",
           cr.files_merged, cr.keys_kept, cr.keys_dropped, ct);

    fs::remove_all(dir);
    return 0;
}
