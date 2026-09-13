/*
 * Layer 3 — backlog / newest-wins contract for buffer_pool.
 *
 * When capture outruns encode, drop-oldest must prefer recent frames so
 * interactive content (terminals on a Miracast head) is not stuck behind
 * a multi-second queue.
 */

#include "buffer-pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "        \
                      << #cond << std::endl;                                   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

struct tagged_buf : buffer_pool_buf
{
    int seq = -1;
};

static std::vector<int> drain_encode(buffer_pool<tagged_buf, MAX_FRAME_FAILURES> &pool)
{
    std::vector<int> got;
    while (pool.encode().ready_encode()) {
        got.push_back(pool.encode().seq);
        pool.next_encode();
    }
    return got;
}

/* Flood capture with no encode; drained seqs must be the newest window. */
static void test_drop_oldest_keeps_newest()
{
    buffer_pool<tagged_buf, MAX_FRAME_FAILURES> pool;
    int seq = 0;
    const int produce = MAX_FRAME_FAILURES * 4;

    for (int i = 0; i < produce; ++i) {
        pool.capture().seq = seq++;
        pool.next_capture();
    }

    const int last = seq - 1;
    auto got = drain_encode(pool);

    EXPECT(!got.empty());
    EXPECT(pool.size() == MAX_FRAME_FAILURES);

    /* Must not still be sitting on the earliest frames. */
    EXPECT(got.front() > last / 2);
    EXPECT(got.back() == last);

    /* Monotonic non-decreasing (ring delivers in capture order among survivors). */
    EXPECT(std::is_sorted(got.begin(), got.end()));

    /* Every delivered seq is in the newest pool-depth window. */
    for (int s : got) {
        EXPECT(s > last - MAX_FRAME_FAILURES);
        EXPECT(s <= last);
    }

    std::cout << "newest-wins: produced=" << produce << " last=" << last
              << " encoded=" << got.size() << " range=[" << got.front() << ","
              << got.back() << "]\n";
}

/* Slow encoder + fast capturer: after stop, drained frames are still recent. */
static void test_slow_encode_prefers_recent()
{
    buffer_pool<tagged_buf, MAX_FRAME_FAILURES> pool;
    std::atomic<bool> stop{false};
    std::atomic<int> seq{0};
    std::atomic<int> encoded_max{-1};

    std::thread capturer([&] {
        while (!stop.load()) {
            int s = seq.fetch_add(1);
            pool.capture().seq = s;
            pool.next_capture();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::thread encoder([&] {
        while (!stop.load()) {
            if (pool.encode().ready_encode()) {
                int s = pool.encode().seq;
                encoded_max.store(std::max(encoded_max.load(), s));
                pool.next_encode();
                /* Simulate slow encode relative to capture. */
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    capturer.join();
    encoder.join();

    int last = seq.load() - 1;
    auto rest = drain_encode(pool);
    int final_max = encoded_max.load();
    for (int s : rest) {
        final_max = std::max(final_max, s);
    }

    EXPECT(last > MAX_FRAME_FAILURES);
    EXPECT(final_max >= 0);
    /* Newest encoded/drained frame should be near the end of production. */
    EXPECT(final_max > last - MAX_FRAME_FAILURES * 2);

    std::cout << "slow-encode: last_produced=" << last
              << " max_encoded=" << final_max
              << " leftover=" << rest.size() << "\n";
}

int main()
{
    test_drop_oldest_keeps_newest();
    test_slow_encode_prefers_recent();

    if (g_failures) {
        std::cerr << g_failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "buffer-pool-backlog-test: ok\n";
    return 0;
}
