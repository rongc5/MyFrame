#include "pipeline/pipeline.h"
#include "containers/double_buffer.h"
#include "search/inverted_index.h"
#include "search/top_k.h"
#include "task_executor.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using myframe::TaskExecutor;
using myframe::containers::DoubleBuffer;
using myframe::pipeline::AtomicWriteStatus;
using myframe::pipeline::BufferedLineReader;
using myframe::pipeline::ByteRange;
using myframe::pipeline::FileStat;
using myframe::pipeline::ParallelFileOptions;
using myframe::pipeline::ScopedFd;
using myframe::pipeline::StringView;
using myframe::pipeline::kNpos;
using myframe::pipeline::parallel_map_reduce;
using myframe::pipeline::parallel_map_reduce_with_executor;
using myframe::pipeline::parallel_parse_file;
using myframe::pipeline::parse_double;
using myframe::pipeline::parse_i64;
using myframe::pipeline::parse_u64;
using myframe::pipeline::read_file;
using myframe::pipeline::split;
using myframe::pipeline::split_fd_by_lines;
using myframe::pipeline::split_file_by_lines;
using myframe::pipeline::split_into;
using myframe::pipeline::split_until;
using myframe::pipeline::stat_file;
using myframe::pipeline::write_file_atomic;
using myframe::pipeline::write_file_atomic_ex;
using myframe::search::InvertedIndex;
using myframe::search::TopK;

static std::atomic<int> g_checks(0);

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "]: "        \
                      << #cond << std::endl;                                     \
            std::exit(1);                                                        \
        }                                                                        \
        g_checks.fetch_add(1, std::memory_order_relaxed);                        \
    } while (0)

class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/myframe_pipeline_test_XXXXXX";
        char* res = ::mkdtemp(tmpl);
        CHECK(res != nullptr);
        path_ = res;
    }

    ~TempDir() noexcept {
        for (std::size_t i = 0; i < files_.size(); ++i) {
            (void)::unlink(files_[i].c_str());
        }
        (void)::rmdir(path_.c_str());
    }

    std::string file(const std::string& name) {
        const std::string full = path_ + "/" + name;
        files_.push_back(full);
        return full;
    }

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    std::vector<std::string> files_;
};

// ---------------------------------------------------------------------------
// 1. StringView, split, and strict numeric parsers
// ---------------------------------------------------------------------------

void test_str_view_and_parsers() {
    // Null / empty safety (UBSan clean: never pass nullptr to memcmp/memchr/std::string)
    {
        StringView empty1;
        StringView empty2(nullptr, 0);
        StringView empty3("");
        CHECK(empty1.empty() && empty2.empty() && empty3.empty());
        CHECK(empty1 == empty2 && empty2 == empty3);
        CHECK(!(empty1 < empty2) && !(empty2 < empty1));
        CHECK(empty1.find('a') == kNpos);
        CHECK(empty1.substr(10, 5).empty());
        CHECK(empty1.trim().empty());
        CHECK(empty1.trim_crlf().empty());
        CHECK(empty1.to_string().empty());
        CHECK(empty1.starts_with(empty2) && empty1.ends_with(empty2));
        CHECK(!empty1.starts_with("a") && !empty1.ends_with("a"));
    }

    // Slicing, UTF-8 unsigned byte comparison, ASCII trim with high-bit bytes
    {
        std::string utf8 = " \t\r\n广告搜索\xff \v\f ";
        StringView trimmed = StringView(utf8).trim();
        CHECK(trimmed == StringView("广告搜索\xff", std::strlen("广告搜索") + 1));
        CHECK(StringView("hello\r\n").trim_crlf() == "hello");
        CHECK(StringView("abcdef").substr(2, 3) == "cde");
        CHECK(StringView("abcdef").substr(100, 3).empty());
        CHECK(StringView("abc").starts_with("ab") && StringView("abc").ends_with("bc"));
        CHECK(StringView("\x7f") < StringView("\x80")); // Unsigned byte comparison
    }

    // Exact split semantics: "", ",", "a,", "a,b,c" + early stop + capacity reuse
    {
        std::vector<StringView> fields;
        split_into("", ',', fields);
        CHECK(fields.size() == 1 && fields[0].empty());

        split_into(",", ',', fields);
        CHECK(fields.size() == 2 && fields[0].empty() && fields[1].empty());

        split_into("a,", ',', fields);
        CHECK(fields.size() == 2 && fields[0] == "a" && fields[1].empty());

        fields.reserve(16);
        const std::size_t cap_before = fields.capacity();
        split_into("k1\tk2\tk3\tk4", '\t', fields);
        CHECK(fields.size() == 4 && fields[0] == "k1" && fields[3] == "k4");
        CHECK(fields.capacity() == cap_before);

        int visited = 0;
        split_until("1,2,3,4,5", ',', [&visited](StringView f) -> bool {
            ++visited;
            return f != "3";
        });
        CHECK(visited == 3);
    }

    // Strict parse_i64 (INT64_MIN, INT64_MAX, overflow, leading '+', junk)
    {
        std::int64_t v = 777;
        CHECK(parse_i64("0", v) && v == 0);
        CHECK(parse_i64("-0", v) && v == 0);
        CHECK(parse_i64("+42", v) && v == 42);
        CHECK(parse_i64("9223372036854775807", v) &&
              v == std::numeric_limits<std::int64_t>::max());
        CHECK(parse_i64("-9223372036854775808", v) &&
              v == std::numeric_limits<std::int64_t>::min());

        v = 123;
        CHECK(!parse_i64("9223372036854775808", v) && v == 123);  // overflow +1
        CHECK(!parse_i64("-9223372036854775809", v) && v == 123); // underflow -1
        CHECK(!parse_i64("", v));
        CHECK(!parse_i64("+", v));
        CHECK(!parse_i64("-", v));
        CHECK(!parse_i64(" 12", v));
        CHECK(!parse_i64("12 ", v));
        CHECK(!parse_i64("12abc", v));
        CHECK(!parse_i64(StringView("12\0x", 4), v));
    }

    // Strict parse_u64 (UINT64_MAX, overflow, rejects '-')
    {
        std::uint64_t v = 999;
        CHECK(parse_u64("0", v) && v == 0);
        CHECK(parse_u64("+18446744073709551615", v) &&
              v == std::numeric_limits<std::uint64_t>::max());
        v = 55;
        CHECK(!parse_u64("18446744073709551616", v) && v == 55);
        CHECK(!parse_u64("-1", v) && v == 55);
        CHECK(!parse_u64("", v));
        CHECK(!parse_u64("+", v));
        CHECK(!parse_u64(" 1", v));
        CHECK(!parse_u64("1x", v));
    }

    // Strict parse_double (grammar, subnormals, overflow, NaN/Inf/hex, >128B fallback)
    {
        double d = -1.0;
        CHECK(parse_double("0", d) && d == 0.0);
        CHECK(parse_double("-0.0", d) && d == 0.0);
        CHECK(parse_double("+3.14159", d) && std::fabs(d - 3.14159) < 1e-12);
        CHECK(parse_double(".5", d) && std::fabs(d - 0.5) < 1e-12);
        CHECK(parse_double("5.", d) && std::fabs(d - 5.0) < 1e-12);
        CHECK(parse_double("1.25e-4", d) && std::fabs(d - 1.25e-4) < 1e-15);
        CHECK(parse_double("1e-310", d) && std::isfinite(d) && d > 0.0); // subnormal accepted

        // Long valid decimal (>128 bytes) exercises heap_buf fallback safely
        std::string long_num = "1.";
        long_num.append(150, '0');
        long_num.append("5");
        CHECK(parse_double(long_num, d) && std::fabs(d - 1.0) < 1e-12);

        d = 42.0;
        CHECK(!parse_double("", d) && d == 42.0);
        CHECK(!parse_double(".", d));
        CHECK(!parse_double("1e", d));
        CHECK(!parse_double("1e+", d));
        CHECK(!parse_double(" 1.0", d));
        CHECK(!parse_double("1.0 ", d));
        CHECK(!parse_double("1.0x", d));
        CHECK(!parse_double("nan", d));
        CHECK(!parse_double("NaN", d));
        CHECK(!parse_double("inf", d));
        CHECK(!parse_double("-Infinity", d));
        CHECK(!parse_double("0x1p4", d));
        CHECK(!parse_double("1e400", d) && d == 42.0); // overflow rejected
        CHECK(!parse_double(StringView("1.5\0x", 5), d));
    }
}

// ---------------------------------------------------------------------------
// 2. Atomic file I/O, stat, read_file, split_file_by_lines, BufferedLineReader
// ---------------------------------------------------------------------------

void test_file_util_and_line_reader() {
    TempDir dir;

    // write_file_atomic + read_file + stat_file + mode preservation + error paths
    {
        const std::string p = dir.file("config.dat");
        std::string err;

        // Empty file with nullptr + 0 is valid
        CHECK(write_file_atomic_ex(p, nullptr, 0, &err, 0640) ==
              myframe::pipeline::kAtomicOk);
        std::string content = "stale";
        CHECK(read_file(p, content, 1024, &err) && content.empty());

        FileStat st1;
        CHECK(stat_file(p, st1, &err) && st1.is_regular && st1.size == 0);

        // Overwrite with preserve_mode=true keeps 0640
        const std::string payload = "line1\nline2\n";
        CHECK(write_file_atomic(p, payload, &err, 0600, true));
        CHECK(read_file(p, content, 1024, &err) && content == payload);

        struct stat raw_st;
        CHECK(::stat(p.c_str(), &raw_st) == 0);
        CHECK((raw_st.st_mode & 0777) == 0640);

        // read_file size cap rejection
        CHECK(!read_file(p, content, 4, &err));
        CHECK(content.empty());

        // Invalid paths rejected cleanly
        CHECK(!write_file_atomic("", "x", &err));
        CHECK(!write_file_atomic(dir.path() + "/", "x", &err));
        CHECK(!write_file_atomic(dir.path() + "/no_such_subdir/f.txt", "x", &err));
    }

    // Edge-case line reader: \r at exact byte 65535 + \n at byte 65536,
    // trailing "abc\r" without \n, embedded NUL bytes, max_line_bytes -> kTooLong
    {
        const std::string p = dir.file("crlf_boundary.txt");
        std::string data;
        // Line 0: 65535 'A' bytes + '\r' at index 65535 + '\n' at index 65536!
        // With buf_size=65536, '\r' is the last byte of read #1 and '\n' is byte 0 of read #2!
        std::string line0(65535, 'A');
        data.append(line0);
        data.push_back('\r');
        data.push_back('\n');
        // Line 1: embedded NUL bytes + \r\n
        std::string line1("nul\0mid\0end", 11);
        data.append(line1);
        data.append("\r\n");
        // Line 2: empty line
        data.push_back('\n');
        // Line 3: final line ending with '\r' and NO '\n' -> '\r' preserved!
        std::string line3 = "final_cr\r";
        data.append(line3);

        std::string err;
        CHECK(write_file_atomic(p, data, &err));

        ScopedFd fd(::open(p.c_str(), O_RDONLY | O_CLOEXEC));
        CHECK(fd.valid());
        BufferedLineReader reader(fd.get(), 0, data.size(), data.size(), 1024 * 1024, 65536);

        StringView sv;
        CHECK(reader.next(sv) == BufferedLineReader::kLine);
        CHECK(reader.line_offset() == 0);
        CHECK(sv == StringView(line0));

        CHECK(reader.next(sv) == BufferedLineReader::kLine);
        CHECK(reader.line_offset() == 65537);
        CHECK(sv == StringView(line1));

        CHECK(reader.next(sv) == BufferedLineReader::kLine);
        CHECK(sv.empty());

        CHECK(reader.next(sv) == BufferedLineReader::kLine);
        CHECK(sv == StringView(line3)); // '\r' preserved without '\n'

        CHECK(reader.next(sv) == BufferedLineReader::kEof);

        // Now test max_line_bytes enforcement (kTooLong skips to next '\n' cleanly)
        const std::string p2 = dir.file("too_long.txt");
        const std::string d2 = "ok1\n" + std::string(500, 'Z') + "\nok2\n";
        CHECK(write_file_atomic(p2, d2, &err));
        ScopedFd fd2(::open(p2.c_str(), O_RDONLY | O_CLOEXEC));
        CHECK(fd2.valid());
        BufferedLineReader r2(fd2.get(), 0, d2.size(), d2.size(), 100, 64);

        CHECK(r2.next(sv) == BufferedLineReader::kLine && sv == "ok1");
        CHECK(r2.next(sv) == BufferedLineReader::kTooLong && r2.line_offset() == 4);
        CHECK(r2.next(sv) == BufferedLineReader::kLine && sv == "ok2");
        CHECK(r2.next(sv) == BufferedLineReader::kEof);
    }

    // Giant single line spanning multiple chunk targets -> deduplicated into 1 chunk!
    {
        const std::string p = dir.file("giant_line.txt");
        const std::string giant(200000, 'G');
        std::string err;
        CHECK(write_file_atomic(p, giant, &err));

        std::vector<ByteRange> ranges;
        CHECK(split_file_by_lines(p, 8, ranges, &err));
        CHECK(ranges.size() == 1);
        CHECK(ranges[0].start == 0 && ranges[0].end == giant.size());
    }

    // Differential split + BufferedLineReader test across 1..16 chunks vs exact lines
    {
        const std::string p = dir.file("diff_lines.tsv");
        std::vector<std::string> expected;
        std::string blob;
        for (int i = 0; i < 2500; ++i) {
            std::string line = "id_" + std::to_string(i) + "\t" +
                               std::string(static_cast<std::size_t>(i % 73), 'x') +
                               "\t广告_" + std::to_string(i * 3);
            expected.push_back(line);
            blob.append(line);
            if (i % 5 == 0) {
                blob.append("\r\n");
            } else if (i + 1 < 2500) {
                blob.push_back('\n');
            }
            // Deliberately omit trailing '\n' on the final line!
        }

        std::string err;
        CHECK(write_file_atomic(p, blob, &err));

        ScopedFd fd(::open(p.c_str(), O_RDONLY | O_CLOEXEC));
        CHECK(fd.valid());
        const std::size_t chunk_counts[] = {1, 2, 3, 5, 8, 16};
        for (std::size_t ci = 0; ci < sizeof(chunk_counts) / sizeof(chunk_counts[0]); ++ci) {
            std::vector<ByteRange> ranges;
            CHECK(split_fd_by_lines(fd.get(), blob.size(), chunk_counts[ci], ranges, &err));
            std::vector<std::string> got;
            for (std::size_t r = 0; r < ranges.size(); ++r) {
                BufferedLineReader reader(
                    fd.get(), ranges[r].start, ranges[r].end, blob.size(), 1024 * 1024, 257);
                StringView sv;
                BufferedLineReader::Status st;
                while ((st = reader.next(sv)) == BufferedLineReader::kLine) {
                    got.push_back(sv.to_string());
                }
                CHECK(st == BufferedLineReader::kEof);
            }
            CHECK(got == expected);
        }
    }
}

// ---------------------------------------------------------------------------
// 3. parallel_parse_file (deterministic merge, move-only state, exceptions,
//    end-to-end InvertedIndex + DoubleBuffer hot reload)
// ---------------------------------------------------------------------------

struct NonDefaultMoveOnlyAcc {
    std::unique_ptr<std::vector<std::uint64_t> > ids;
    std::uint64_t sum;

    explicit NonDefaultMoveOnlyAcc(std::size_t reserve_n)
        : ids(new std::vector<std::uint64_t>()), sum(0) {
        ids->reserve(reserve_n);
    }
    NonDefaultMoveOnlyAcc(const NonDefaultMoveOnlyAcc&) = delete;
    NonDefaultMoveOnlyAcc& operator=(const NonDefaultMoveOnlyAcc&) = delete;
    NonDefaultMoveOnlyAcc(NonDefaultMoveOnlyAcc&&) noexcept = default;
    NonDefaultMoveOnlyAcc& operator=(NonDefaultMoveOnlyAcc&&) noexcept = default;
    ~NonDefaultMoveOnlyAcc() noexcept {}
};

void test_parallel_parse_file() {
    TempDir dir;

    // Empty file: must succeed without invoking make_local or merge_local
    {
        const std::string empty_p = dir.file("empty.tsv");
        std::string err;
        CHECK(write_file_atomic(empty_p, "", &err));
        int calls = 0;
        parallel_parse_file(
            empty_p,
            [&]() { ++calls; return 0; },
            [](StringView, int&) {},
            [&](int&&) { ++calls; });
        CHECK(calls == 0);
    }

    // 30,000-line ad-targeting TSV parsed in parallel -> deterministic order +
    // exact InvertedIndex + DoubleBuffer publish
    {
        const std::string tsv_p = dir.file("ad_targeting.tsv");
        const std::uint64_t kLines = 30000;
        std::string content;
        content.reserve(kLines * 36);
        std::uint64_t expected_sum = 0;
        for (std::uint64_t i = 1; i <= kLines; ++i) {
            content.append(std::to_string(i));
            content.push_back('\t');
            content.append((i % 2 == 0) ? "os=ios" : "os=android");
            content.push_back('\t');
            content.append("geo=region_" + std::to_string(i % 10));
            content.push_back('\t');
            content.append(std::to_string(static_cast<double>(i) * 0.25));
            content.push_back('\n');
            expected_sum += i;
        }
        std::string err;
        CHECK(write_file_atomic(tsv_p, content, &err));

        const std::size_t thread_counts[] = {1, 2, 4, 8};
        for (std::size_t ti = 0; ti < sizeof(thread_counts) / sizeof(thread_counts[0]); ++ti) {
            ParallelFileOptions opts;
            opts.num_threads = thread_counts[ti];

            std::vector<std::uint64_t> merged_ids;
            merged_ids.reserve(kLines);
            std::uint64_t total_sum = 0;

            parallel_parse_file(
                tsv_p,
                []() { return NonDefaultMoveOnlyAcc(1024); },
                [](StringView line, NonDefaultMoveOnlyAcc& acc) {
                    std::vector<StringView> cols;
                    split_into(line, '\t', cols);
                    CHECK(cols.size() == 4);
                    std::uint64_t id = 0;
                    double ecpm = 0.0;
                    CHECK(parse_u64(cols[0], id));
                    CHECK(parse_double(cols[3], ecpm));
                    CHECK(std::fabs(ecpm - static_cast<double>(id) * 0.25) < 1e-9);
                    acc.ids->push_back(id);
                    acc.sum += id;
                },
                [&](NonDefaultMoveOnlyAcc&& acc) {
                    merged_ids.insert(merged_ids.end(), acc.ids->begin(), acc.ids->end());
                    total_sum += acc.sum;
                },
                opts);

            CHECK(total_sum == expected_sum);
            CHECK(merged_ids.size() == kLines);
            // Strictly deterministic file order 1..kLines regardless of thread count!
            for (std::uint64_t i = 0; i < kLines; ++i) {
                CHECK(merged_ids[static_cast<std::size_t>(i)] == i + 1);
            }
        }

        // End-to-end: parallel_parse_file -> serial InvertedIndex merge -> DoubleBuffer publish
        typedef InvertedIndex<std::string, std::uint64_t> AdIndex;
        struct LocalPostings {
            std::vector<std::pair<std::string, std::uint64_t> > pairs;
        };
        std::unique_ptr<AdIndex> fresh(new AdIndex());
        ParallelFileOptions opts;
        opts.num_threads = 4;
        parallel_parse_file(
            tsv_p,
            []() { return LocalPostings(); },
            [](StringView line, LocalPostings& lp) {
                std::vector<StringView> cols;
                split_into(line, '\t', cols);
                std::uint64_t id = 0;
                if (cols.size() >= 3 && parse_u64(cols[0], id)) {
                    lp.pairs.push_back(std::make_pair(cols[1].to_string(), id));
                    lp.pairs.push_back(std::make_pair(cols[2].to_string(), id));
                }
            },
            [&](LocalPostings&& lp) {
                for (std::size_t i = 0; i < lp.pairs.size(); ++i) {
                    fresh->add_unsorted(lp.pairs[i].first, lp.pairs[i].second);
                }
            },
            opts);
        fresh->finalize();

        DoubleBuffer<AdIndex> db;
        db.publish(std::move(fresh));
        {
            DoubleBuffer<AdIndex>::ReadGuard g = db.read();
            // Even IDs in [1..30000] with i % 10 == 4 -> 3000 exact matches!
            std::vector<std::uint64_t> hits = g->query_and({"os=ios", "geo=region_4"});
            CHECK(hits.size() == 3000);
            CHECK(hits.front() == 4 && hits.back() == 29994);
        }

        // Worker exception cancels remaining chunks, joins all threads, rethrows cleanly
        bool threw = false;
        try {
            ParallelFileOptions err_opts;
            err_opts.num_threads = 4;
            parallel_parse_file(
                tsv_p,
                []() { return std::size_t(0); },
                [](StringView line, std::size_t& cnt) {
                    ++cnt;
                    if (line.starts_with("15000\t")) {
                        throw std::runtime_error("poison line 15000");
                    }
                },
                [](std::size_t&&) {
                    // Must NEVER run merge if any worker threw!
                    CHECK(false);
                },
                err_opts);
        } catch (const std::runtime_error& e) {
            threw = (std::string(e.what()) == "poison line 15000");
        }
        CHECK(threw);
    }
}

// ---------------------------------------------------------------------------
// 4. parallel_map_reduce & parallel_map_reduce_with_executor (TaskExecutor +
//    rejection fallback + recursive saturated-pool deadlock-freedom!)
// ---------------------------------------------------------------------------

void test_parallel_map_reduce_and_executor() {
    // Edge cases: 0 items, 1 item
    {
        int calls = 0;
        parallel_map_reduce(
            0, 4,
            [&]() { ++calls; return 0; },
            [](std::size_t, int&) {},
            [&](int&&) { ++calls; });
        CHECK(calls == 0);

        std::uint64_t sum = 0;
        parallel_map_reduce(
            1, 8,
            []() { return std::uint64_t(0); },
            [](std::size_t i, std::uint64_t& acc) { acc += i + 10; },
            [&](std::uint64_t&& acc) { sum += acc; });
        CHECK(sum == 10);
    }

    // Parallel ad scoring -> per-thread TopK -> serial TopK merge
    {
        const std::size_t kN = 100000;
        std::vector<std::uint64_t> scores(kN);
        std::uint64_t expected_sum = 0;
        for (std::size_t i = 0; i < kN; ++i) {
            scores[i] = (static_cast<std::uint64_t>(i) * 2654435761ULL) % 1000000ULL;
            expected_sum += scores[i];
        }

        struct LocalRank {
            TopK<std::uint64_t> top;
            std::uint64_t sum;
            LocalRank() : top(10), sum(0) {}
        };

        TopK<std::uint64_t> global_top(10);
        std::uint64_t global_sum = 0;
        parallel_map_reduce(
            kN, 4,
            []() { return LocalRank(); },
            [&](std::size_t idx, LocalRank& loc) {
                loc.top.push(scores[idx]);
                loc.sum += scores[idx];
            },
            [&](LocalRank&& loc) {
                global_sum += loc.sum;
                std::vector<std::uint64_t> part = loc.top.extract_sorted();
                for (std::size_t j = 0; j < part.size(); ++j) {
                    global_top.push(part[j]);
                }
            });

        CHECK(global_sum == expected_sum);
        std::vector<std::uint64_t> got_top = global_top.extract_sorted();
        std::sort(scores.begin(), scores.end());
        std::vector<std::uint64_t> exp_top(scores.rbegin(), scores.rbegin() + 10);
        CHECK(got_top == exp_top);
    }

    // Worker exception cancellation & clean join in parallel_map_reduce
    {
        bool threw = false;
        try {
            parallel_map_reduce(
                50000, 4,
                []() { return 0; },
                [](std::size_t i, int& acc) {
                    acc += 1;
                    if (i == 25000) {
                        throw std::runtime_error("map boom");
                    }
                },
                [](int&&) { CHECK(false); });
        } catch (const std::runtime_error& e) {
            threw = (std::string(e.what()) == "map boom");
        }
        CHECK(threw);
    }

    // parallel_map_reduce_with_executor: real TaskExecutor (4 threads)
    {
        TaskExecutor pool(4, 64);
        const std::size_t kN = 80000;
        std::uint64_t total = 0;
        parallel_map_reduce_with_executor(
            kN, 4,
            [&pool](std::function<void()> fn) { return pool.submit(std::move(fn)); },
            []() { return std::uint64_t(0); },
            [](std::size_t i, std::uint64_t& acc) { acc += static_cast<std::uint64_t>(i + 1); },
            [&](std::uint64_t&& acc) { total += acc; });
        const std::uint64_t expected =
            static_cast<std::uint64_t>(kN) * (kN + 1) / 2;
        CHECK(total == expected);
        pool.shutdown();
    }

    // Rejecting & throwing executors degrade cleanly to caller-inline execution
    {
        const std::size_t kN = 10000;
        std::uint64_t total = 0;
        parallel_map_reduce_with_executor(
            kN, 4,
            [](std::function<void()>) -> bool { return false; },
            []() { return std::uint64_t(0); },
            [](std::size_t i, std::uint64_t& acc) { acc += i; },
            [&](std::uint64_t&& acc) { total += acc; });
        CHECK(total == static_cast<std::uint64_t>(kN) * (kN - 1) / 2);

        total = 0;
        parallel_map_reduce_with_executor(
            kN, 4,
            [](std::function<void()>) -> bool { throw std::runtime_error("submit rejected"); },
            []() { return std::uint64_t(0); },
            [](std::size_t i, std::uint64_t& acc) { acc += i; },
            [&](std::uint64_t&& acc) { total += acc; });
        CHECK(total == static_cast<std::uint64_t>(kN) * (kN - 1) / 2);
    }

    // Saturated / recursive 1-thread TaskExecutor MUST NOT deadlock!
    {
        TaskExecutor single_pool(1, 16);
        std::atomic<bool> done(false);
        std::uint64_t nested_sum = 0;

        CHECK(single_pool.submit([&]() {
            parallel_map_reduce_with_executor(
                5000, 4,
                [&single_pool](std::function<void()> fn) {
                    return single_pool.submit(std::move(fn));
                },
                []() { return std::uint64_t(0); },
                [](std::size_t i, std::uint64_t& acc) { acc += i + 1; },
                [&](std::uint64_t&& acc) { nested_sum += acc; });
            done.store(true, std::memory_order_release);
        }));

        single_pool.shutdown();
        CHECK(done.load(std::memory_order_acquire));
        CHECK(nested_sum == 5000ULL * 5001ULL / 2ULL);
    }

    // Audit regressions:
    // 1. FIFO paths fail immediately without hanging (O_NONBLOCK + S_ISREG)
    // 2. ScopedFd self-reset safety
    // 3. parse_double preserves caller errno
    // 4. LocalState in parallel_map_reduce_with_executor is ALWAYS destroyed on caller thread
    //    even when exceptions unwind while queued tasks still hold shared_ptr<SharedState>
    {
        TempDir dir;
        const std::string fifo = dir.file("nonblock.fifo");
        CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
        std::string out, err;
        CHECK(!read_file(fifo, out, 1024, &err));
        std::vector<ByteRange> ranges;
        CHECK(!split_file_by_lines(fifo, 4, ranges, &err));
        bool fifo_threw = false;
        try {
            parallel_parse_file(
                fifo,
                []() { return 0; },
                [](StringView, int&) {},
                [](int&&) {});
        } catch (const std::runtime_error&) {
            fifo_threw = true;
        }
        CHECK(fifo_threw);

        const std::string reg = dir.file("self_reset.txt");
        CHECK(write_file_atomic(reg, "ok", &err));
        ScopedFd sfd(::open(reg.c_str(), O_RDONLY | O_CLOEXEC));
        CHECK(sfd.valid());
        const int raw = sfd.get();
        sfd.reset(raw); // Self-reset must NOT close fd!
        CHECK(sfd.valid() && sfd.get() == raw);
        FileStat st;
        CHECK(myframe::pipeline::stat_fd(sfd.get(), st, &err) && st.size == 2);

        errno = EINVAL;
        double dv = 0.0;
        CHECK(parse_double("1e-310", dv));
        CHECK(errno == EINVAL); // Caller errno preserved even on ERANGE subnormal

        // Verify LocalState destroyed strictly on caller thread even on error while
        // deferred tasks still hold SharedState!
        const std::thread::id caller_tid = std::this_thread::get_id();
        std::atomic<int> wrong_thread_dtors(0);
        std::atomic<int> total_dtors(0);
        struct CallerScopedLocal {
            std::thread::id expected_tid;
            std::atomic<int>* wrong_cnt;
            std::atomic<int>* total_cnt;
            bool active;
            CallerScopedLocal(std::thread::id tid, std::atomic<int>* w, std::atomic<int>* t)
                : expected_tid(tid), wrong_cnt(w), total_cnt(t), active(true) {}
            CallerScopedLocal(CallerScopedLocal&& o) noexcept
                : expected_tid(o.expected_tid),
                  wrong_cnt(o.wrong_cnt),
                  total_cnt(o.total_cnt),
                  active(o.active) {
                o.active = false;
            }
            ~CallerScopedLocal() noexcept {
                if (active) {
                    total_cnt->fetch_add(1, std::memory_order_relaxed);
                    if (std::this_thread::get_id() != expected_tid) {
                        wrong_cnt->fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        };

        std::vector<std::function<void()> > deferred_tasks;
        bool exec_threw = false;
        try {
            parallel_map_reduce_with_executor(
                1000, 4,
                [&deferred_tasks](std::function<void()> fn) -> bool {
                    deferred_tasks.push_back(std::move(fn)); // Hold task (never runs before return!)
                    return true;
                },
                [&]() { return CallerScopedLocal(caller_tid, &wrong_thread_dtors, &total_dtors); },
                [](std::size_t i, CallerScopedLocal&) {
                    if (i == 10) {
                        throw std::runtime_error("executor unwind check");
                    }
                },
                [](CallerScopedLocal&&) { CHECK(false); });
        } catch (const std::runtime_error& e) {
            exec_threw = (std::string(e.what()) == "executor unwind check");
        }
        CHECK(exec_threw);
        // All LocalStates were ALREADY destroyed on caller_tid BEFORE deferred tasks drop!
        CHECK(total_dtors.load() >= 1);
        CHECK(wrong_thread_dtors.load() == 0);
        // Running deferred tasks on another thread after return must safely exit & not touch LocalState!
        std::thread late_runner([&deferred_tasks]() {
            for (std::size_t i = 0; i < deferred_tasks.size(); ++i) {
                deferred_tasks[i]();
            }
            deferred_tasks.clear();
        });
        late_runner.join();
        CHECK(wrong_thread_dtors.load() == 0);
    }
}

} // namespace

int main() {
    test_str_view_and_parsers();
    test_file_util_and_line_reader();
    test_parallel_parse_file();
    test_parallel_map_reduce_and_executor();
    std::cout << "[PASS] test_pipeline (" << g_checks.load()
              << " checks passed)" << std::endl;
    return 0;
}
