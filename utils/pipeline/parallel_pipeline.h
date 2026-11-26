#pragma once

#include "file_util.h"
#include "str_view.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace myframe {
namespace pipeline {

namespace detail {

inline std::size_t resolve_worker_count(std::size_t requested, std::size_t max_units) noexcept {
    if (max_units == 0) {
        return 0;
    }
    std::size_t n = requested;
    if (n == 0) {
        const unsigned int hw = std::thread::hardware_concurrency();
        n = (hw == 0) ? 1 : static_cast<std::size_t>(hw);
    }
    if (n > max_units) {
        n = max_units;
    }
    return (n == 0) ? 1 : n;
}

class ThreadJoiner {
public:
    explicit ThreadJoiner(std::vector<std::thread>& threads) noexcept : threads_(threads) {}
    ~ThreadJoiner() noexcept {
        for (std::size_t i = 0; i < threads_.size(); ++i) {
            if (threads_[i].joinable()) {
                threads_[i].join();
            }
        }
    }
    ThreadJoiner(const ThreadJoiner&) = delete;
    ThreadJoiner& operator=(const ThreadJoiner&) = delete;

private:
    std::vector<std::thread>& threads_;
};

} // namespace detail

// -----------------------------------------------------------------------------
// parallel_map_reduce: in-memory parallel map + serial reduce
//
// Parameters:
//   - `count`: total item indices [0, count). If 0, returns without calling make_local.
//   - `num_threads`: desired parallelism (0 = hardware_concurrency, clamped to [1, count]).
//   - `make_local()`: creates a per-worker LocalState. Invoked concurrently from
//     worker threads, so it MUST be thread-safe. LocalState need not be
//     default-constructible; move-only types are supported; must have
//     alignof(L) <= alignof(std::max_align_t) and nothrow destructor.
//   - `map_index(std::size_t idx, LocalState& local)`: processes item `idx` lock-free.
//   - `merge_local(LocalState&& local)`: runs serially on the calling thread after
//     all workers complete. Because grains are claimed dynamically across workers,
//     `merge_local` must be commutative and associative (e.g. sums, TopK, sets).
//     (For strict order-preserving merges on files, see parallel_parse_file.)
//
// Resilience:
//   - Caller participates as worker 0 and grains are claimed dynamically, so if a
//     worker thread fails to spawn (system_error or bad_alloc), remaining workers
//     cleanly finish all work with reduced parallelism.
//   - Any worker exception cancels remaining grains, joins all threads via RAII,
//     and rethrows the first captured exception on the calling thread.
// -----------------------------------------------------------------------------
template <typename MakeLocal, typename MapIndex, typename MergeLocal>
void parallel_map_reduce(std::size_t count,
                         std::size_t num_threads,
                         MakeLocal&& make_local,
                         MapIndex&& map_index,
                         MergeLocal&& merge_local) {
    typedef typename std::decay<decltype(make_local())>::type LocalState;
    static_assert(alignof(LocalState) <= alignof(std::max_align_t),
                  "LocalState over-alignment > max_align_t is not supported by C++11 new; "
                  "wrap in std::unique_ptr or remove alignas");
    static_assert(std::is_nothrow_destructible<LocalState>::value,
                  "LocalState must be nothrow-destructible");

    if (count == 0) {
        return;
    }

    const std::size_t nworkers = detail::resolve_worker_count(num_threads, count);
    if (nworkers == 1) {
        LocalState local(make_local());
        for (std::size_t i = 0; i < count; ++i) {
            map_index(i, local);
        }
        merge_local(std::move(local));
        return;
    }

    const std::size_t target_grains = nworkers * 8;
    const std::size_t grain =
        (count > target_grains) ? (count / target_grains) : 1;

    std::vector<std::unique_ptr<LocalState> > slots(nworkers);
    std::atomic<std::size_t> next_idx(0);
    std::atomic<bool> cancelled(false);
    std::mutex err_mu;
    std::exception_ptr first_err;

    auto record_error = [&](std::exception_ptr ep) noexcept {
        cancelled.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lk(err_mu);
        if (!first_err) {
            first_err = ep;
        }
    };

    auto worker = [&](std::size_t w) noexcept {
        try {
            std::unique_ptr<LocalState> local;
            for (;;) {
                if (cancelled.load(std::memory_order_acquire)) {
                    break;
                }
                const std::size_t begin =
                    next_idx.fetch_add(grain, std::memory_order_relaxed);
                if (begin >= count) {
                    break;
                }
                if (!local) {
                    local.reset(new LocalState(make_local()));
                }
                const std::size_t end =
                    (count - begin > grain) ? (begin + grain) : count;
                for (std::size_t i = begin; i < end; ++i) {
                    if (cancelled.load(std::memory_order_relaxed)) {
                        break;
                    }
                    map_index(i, *local);
                }
            }
            slots[w] = std::move(local);
        } catch (...) {
            record_error(std::current_exception());
        }
    };

    {
        std::vector<std::thread> threads;
        threads.reserve(nworkers - 1);
        detail::ThreadJoiner joiner(threads);

        for (std::size_t w = 1; w < nworkers; ++w) {
            try {
                threads.emplace_back(worker, w);
            } catch (...) {
                break; // Degrade gracefully on system_error or bad_alloc.
            }
        }
        worker(0);
    } // All spawned threads are guaranteed joined here.

    if (first_err) {
        std::rethrow_exception(first_err);
    }
    for (std::size_t w = 0; w < nworkers; ++w) {
        if (slots[w]) {
            merge_local(std::move(*slots[w]));
        }
    }
}

// -----------------------------------------------------------------------------
// parallel_map_reduce_with_executor: reuses an external thread pool (e.g.
// myframe::TaskExecutor) for online parallel map + serial reduce without spawning
// OS threads per request.
//
// `submit_fn(std::function<void()>) -> bool` submits a worker task to the pool
// and returns true if accepted, false if rejected (queue full / shutting down).
// `make_local()` is called concurrently from worker threads and must be thread-safe.
// `merge_local(LocalState&&)` must be commutative and associative.
// All LocalState instances are guaranteed to be destroyed on the calling thread
// before this function returns (even when exceptions occur and late queued tasks
// still hold references to the internal synchronization state).
//
// Deadlock safety:
//   - Waits ONLY on tasks that actually started executing (`active`), NEVER on
//     tasks still sitting in the executor queue. When the pool is saturated or
//     when invoked recursively from a pool worker thread, the caller drains all
//     remaining grains inline, sets `closed = true`, and late-starting tasks exit
//     immediately without touching user functors.
//   - Remaining precondition: `map_index` must NOT block waiting on another task
//     submitted to the same pool, and caller must NOT hold a mutex required by
//     pool workers. Under heavy load, execution may degrade to fully serial.
// -----------------------------------------------------------------------------
template <typename SubmitFn, typename MakeLocal, typename MapIndex, typename MergeLocal>
void parallel_map_reduce_with_executor(std::size_t count,
                                       std::size_t max_workers,
                                       SubmitFn&& submit_fn,
                                       MakeLocal&& make_local,
                                       MapIndex&& map_index,
                                       MergeLocal&& merge_local) {
    typedef typename std::decay<decltype(make_local())>::type LocalState;
    static_assert(alignof(LocalState) <= alignof(std::max_align_t),
                  "LocalState over-alignment > max_align_t is not supported by C++11 new");
    static_assert(std::is_nothrow_destructible<LocalState>::value,
                  "LocalState must be nothrow-destructible");

    if (count == 0) {
        return;
    }

    const std::size_t nworkers = detail::resolve_worker_count(max_workers, count);
    if (nworkers == 1) {
        LocalState local(make_local());
        for (std::size_t i = 0; i < count; ++i) {
            map_index(i, local);
        }
        merge_local(std::move(local));
        return;
    }

    typedef typename std::remove_reference<MakeLocal>::type MakeLocalT;
    typedef typename std::remove_reference<MapIndex>::type MapIndexT;

    struct SharedState {
        std::size_t count;
        std::size_t grain;
        MakeLocalT* make_local_ptr;
        MapIndexT* map_index_ptr;
        std::vector<std::unique_ptr<LocalState> > slots;
        std::atomic<std::size_t> next_idx;
        std::atomic<bool> cancelled;
        std::mutex mu;
        std::condition_variable cv;
        std::size_t next_slot;
        std::size_t active;
        bool closed;
        std::exception_ptr first_err;

        SharedState(std::size_t c,
                    std::size_t g,
                    std::size_t nslots,
                    MakeLocalT* ml,
                    MapIndexT* mi)
            : count(c),
              grain(g),
              make_local_ptr(ml),
              map_index_ptr(mi),
              slots(nslots),
              next_idx(0),
              cancelled(false),
              mu(),
              cv(),
              next_slot(1), // Slot 0 is reserved for caller thread.
              active(0),
              closed(false),
              first_err() {}
    };

    const std::size_t target_grains = nworkers * 8;
    const std::size_t grain =
        (count > target_grains) ? (count / target_grains) : 1;

    std::shared_ptr<SharedState> st(
        new SharedState(count, grain, nworkers, &make_local, &map_index));

    auto run_worker = [](SharedState& s, std::size_t slot_idx) noexcept {
        try {
            std::unique_ptr<LocalState> local;
            for (;;) {
                if (s.cancelled.load(std::memory_order_acquire)) {
                    break;
                }
                const std::size_t begin =
                    s.next_idx.fetch_add(s.grain, std::memory_order_relaxed);
                if (begin >= s.count) {
                    break;
                }
                if (!local) {
                    local.reset(new LocalState((*s.make_local_ptr)()));
                }
                const std::size_t end =
                    (s.count - begin > s.grain) ? (begin + s.grain) : s.count;
                for (std::size_t i = begin; i < end; ++i) {
                    if (s.cancelled.load(std::memory_order_relaxed)) {
                        break;
                    }
                    (*s.map_index_ptr)(i, *local);
                }
            }
            s.slots[slot_idx] = std::move(local);
        } catch (...) {
            s.cancelled.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(s.mu);
            if (!s.first_err) {
                s.first_err = std::current_exception();
            }
        }
    };

    // RAII guard ensures `closed = true` and `active == 0` wait ALWAYS execute
    // before stack unwinding destroys caller functors/captures.
    struct CloseAndWaitGuard {
        std::shared_ptr<SharedState> state;
        bool done;
        explicit CloseAndWaitGuard(std::shared_ptr<SharedState> s) noexcept
            : state(std::move(s)), done(false) {}
        void wait_and_extract(std::vector<std::unique_ptr<LocalState> >& out_slots,
                              std::exception_ptr& out_err) {
            if (done) {
                return;
            }
            std::unique_lock<std::mutex> lk(state->mu);
            state->closed = true;
            state->cv.wait(lk, [this]() { return state->active == 0; });
            out_slots.swap(state->slots);
            out_err = state->first_err;
            state->first_err = std::exception_ptr();
            done = true;
        }
        ~CloseAndWaitGuard() noexcept {
            if (!done) {
                std::vector<std::unique_ptr<LocalState> > discarded_slots;
                std::exception_ptr discarded_err;
                wait_and_extract(discarded_slots, discarded_err);
            }
        }
    } close_guard(st);

    // Submit up to nworkers - 1 helper tasks. Build std::function INSIDE try so an
    // OOM during task allocation simply degrades parallelism safely.
    for (std::size_t k = 1; k < nworkers; ++k) {
        bool accepted = false;
        try {
            std::shared_ptr<SharedState> cap = st;
            std::function<void()> task([cap, run_worker]() {
                std::size_t my_slot = 0;
                {
                    std::lock_guard<std::mutex> lk(cap->mu);
                    if (cap->closed || cap->next_slot >= cap->slots.size()) {
                        return;
                    }
                    my_slot = cap->next_slot++;
                    ++cap->active;
                }
                run_worker(*cap, my_slot);
                {
                    std::lock_guard<std::mutex> lk(cap->mu);
                    --cap->active;
                    if (cap->active == 0) {
                        cap->cv.notify_all();
                    }
                }
            });
            accepted = submit_fn(std::move(task));
        } catch (...) {
            accepted = false;
        }
        if (!accepted) {
            break;
        }
    }

    // Caller participates as slot 0 and drains any remaining unclaimed grains.
    run_worker(*st, 0);

    // Move slots and error out onto caller stack under lock so LocalState
    // destructors NEVER run later on a pool thread when late tasks drop `cap`.
    std::vector<std::unique_ptr<LocalState> > local_slots;
    std::exception_ptr err;
    close_guard.wait_and_extract(local_slots, err);

    if (err) {
        std::rethrow_exception(err);
    }
    for (std::size_t w = 0; w < local_slots.size(); ++w) {
        if (local_slots[w]) {
            merge_local(std::move(*local_slots[w]));
        }
    }
}

// Configuration for parallel_parse_file.
struct ParallelFileOptions {
    std::size_t num_threads;       // 0 = hardware_concurrency
    std::size_t max_line_bytes;    // Maximum single line length (excluding '\n')
    std::size_t read_buffer_bytes; // Per-worker pread buffer size
    bool verify_unchanged;         // Post-parse fstat check against concurrent in-place edits

    ParallelFileOptions() noexcept
        : num_threads(0),
          max_line_bytes(16 * 1024 * 1024),
          read_buffer_bytes(65536),
          verify_unchanged(true) {}
};

// -----------------------------------------------------------------------------
// parallel_parse_file: open once -> newline-aligned split -> parallel parse
// into per-chunk LocalState -> deterministic serial merge in exact file order.
//
// Snapshot guarantee:
//   - Opens `path` ONCE (`O_RDONLY|O_CLOEXEC|O_NONBLOCK`) and shares the fd across
//     splitting and all workers via `pread()`, so concurrent atomic `rename()`
//     reloads never mix old and new inodes and FIFOs fail fast at S_ISREG.
//   - `make_local()` is called concurrently from worker threads and must be thread-safe.
//   - Writers modifying target files concurrently should use write_file_atomic().
//     When `verify_unchanged` is true, an end-of-parse fstat verifies size, mtime,
//     and ctime against the pre-parse snapshot.
// -----------------------------------------------------------------------------
template <typename MakeLocal, typename OnLine, typename MergeLocal>
void parallel_parse_file(const std::string& path,
                         MakeLocal&& make_local,
                         OnLine&& on_line,
                         MergeLocal&& merge_local,
                         const ParallelFileOptions& opts = ParallelFileOptions()) {
    typedef typename std::decay<decltype(make_local())>::type LocalState;
    static_assert(alignof(LocalState) <= alignof(std::max_align_t),
                  "LocalState over-alignment > max_align_t is not supported by C++11 new");
    static_assert(std::is_nothrow_destructible<LocalState>::value,
                  "LocalState must be nothrow-destructible");

    ScopedFd fd(detail::open_retry(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (!fd) {
        throw std::runtime_error(detail::format_errno("parallel_parse_file open failed", errno));
    }

    FileStat before_stat;
    std::string err;
    if (!stat_fd(fd.get(), before_stat, &err)) {
        throw std::runtime_error("parallel_parse_file: " + err);
    }
    if (!before_stat.is_regular) {
        throw std::runtime_error("parallel_parse_file requires a regular file");
    }
    if (before_stat.size == 0) {
        return;
    }

    const std::size_t desired_threads =
        detail::resolve_worker_count(opts.num_threads, static_cast<std::size_t>(
            (before_stat.size > 65536ULL) ? 65536ULL : before_stat.size));
    // Oversubscribe chunks 4x relative to threads so dynamic claiming balances
    // skewed line lengths while keeping chunk-indexed merge strictly in file order.
    const std::size_t target_chunks =
        (desired_threads <= 1) ? 1 : (desired_threads * 4);

    std::vector<ByteRange> ranges;
    if (!split_fd_by_lines(fd.get(), before_stat.size, target_chunks, ranges, &err)) {
        throw std::runtime_error("parallel_parse_file: " + err);
    }
    if (ranges.empty()) {
        return;
    }

    auto verify_snapshot = [&]() {
        if (!opts.verify_unchanged) {
            return;
        }
        FileStat after_stat;
        std::string stat_err;
        if (!stat_fd(fd.get(), after_stat, &stat_err)) {
            throw std::runtime_error("parallel_parse_file: " + stat_err);
        }
        if (after_stat != before_stat) {
            throw std::runtime_error("parallel_parse_file: file modified during read");
        }
    };

    auto parse_chunk = [&](std::size_t chunk_idx,
                           LocalState& local,
                           const std::atomic<bool>& cancelled) {
        const ByteRange r = ranges[chunk_idx];
        BufferedLineReader reader(
            fd.get(), r.start, r.end, before_stat.size,
            opts.max_line_bytes, opts.read_buffer_bytes);
        StringView line;
        for (;;) {
            if (cancelled.load(std::memory_order_relaxed)) {
                return;
            }
            const BufferedLineReader::Status st = reader.next(line);
            if (st == BufferedLineReader::kLine) {
                on_line(line, local);
            } else if (st == BufferedLineReader::kEof) {
                break;
            } else if (st == BufferedLineReader::kTooLong) {
                throw std::runtime_error(
                    "parallel_parse_file: line exceeds max_line_bytes at offset " +
                    std::to_string(reader.line_offset()));
            } else {
                throw std::runtime_error("parallel_parse_file: " + reader.error());
            }
        }
    };

    const std::size_t num_chunks = ranges.size();
    if (num_chunks == 1 || desired_threads == 1) {
        std::atomic<bool> never_cancelled(false);
        LocalState local(make_local());
        for (std::size_t c = 0; c < num_chunks; ++c) {
            parse_chunk(c, local, never_cancelled);
        }
        verify_snapshot();
        merge_local(std::move(local));
        return;
    }

    // Dynamic chunk claiming into chunk-indexed slots ensures:
    //   1. Load balancing across skewed chunks and graceful degradation on spawn failure.
    //   2. Strictly deterministic 0..num_chunks-1 serial merge order (exact file order).
    const std::size_t nworkers =
        (desired_threads < num_chunks) ? desired_threads : num_chunks;
    std::vector<std::unique_ptr<LocalState> > chunk_slots(num_chunks);
    std::vector<std::exception_ptr> chunk_errors(num_chunks);
    std::atomic<std::size_t> next_chunk(0);
    std::atomic<bool> cancelled(false);

    auto worker = [&]() noexcept {
        for (;;) {
            if (cancelled.load(std::memory_order_acquire)) {
                break;
            }
            const std::size_t c = next_chunk.fetch_add(1, std::memory_order_relaxed);
            if (c >= num_chunks) {
                break;
            }
            try {
                std::unique_ptr<LocalState> local(new LocalState(make_local()));
                parse_chunk(c, *local, cancelled);
                if (!cancelled.load(std::memory_order_acquire)) {
                    chunk_slots[c] = std::move(local);
                }
            } catch (...) {
                chunk_errors[c] = std::current_exception();
                cancelled.store(true, std::memory_order_release);
                break;
            }
        }
    };

    {
        std::vector<std::thread> threads;
        threads.reserve(nworkers - 1);
        detail::ThreadJoiner joiner(threads);

        for (std::size_t w = 1; w < nworkers; ++w) {
            try {
                threads.emplace_back(worker);
            } catch (...) {
                break; // Caller + spawned workers dynamically claim all chunks.
            }
        }
        worker();
    } // All worker threads joined here.

    // Rethrow the lowest-index captured chunk error (if multiple chunks failed).
    for (std::size_t c = 0; c < num_chunks; ++c) {
        if (chunk_errors[c]) {
            std::rethrow_exception(chunk_errors[c]);
        }
    }

    verify_snapshot();

    for (std::size_t c = 0; c < num_chunks; ++c) {
        if (chunk_slots[c]) {
            merge_local(std::move(*chunk_slots[c]));
        }
    }
}

} // namespace pipeline
} // namespace myframe
