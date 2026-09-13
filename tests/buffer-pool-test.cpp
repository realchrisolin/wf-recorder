/*
 * Unit tests for buffer_pool — Miracast/Extend latency contract:
 * prefer the newest captured frame over queue depth; never abort when full.
 */

#include "buffer-pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "        \
                      << #cond << std::endl;                                   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static void test_initial_size()
{
    buffer_pool<buffer_pool_buf, MAX_FRAME_FAILURES> pool;
    EXPECT(pool.size() == INITIAL_BUFFERS_SIZE);
}

/* Capture without encode until the ring must grow. */
static void test_grows_until_cap()
{
    buffer_pool<buffer_pool_buf, MAX_FRAME_FAILURES> pool;

    /* Drain growth: each next_capture without next_encode eventually
     * collides with an unreleased slot and grows. */
    for (int i = 0; i < MAX_FRAME_FAILURES * 2; ++i) {
        pool.next_capture();
    }

    EXPECT(pool.size() == MAX_FRAME_FAILURES);
    EXPECT(pool.size() <= MAX_FRAME_FAILURES);
}

/* Once capped, keep capturing: must drop oldest pending, not abort. */
static void test_drop_oldest_when_full()
{
    buffer_pool<buffer_pool_buf, MAX_FRAME_FAILURES> pool;

    for (int i = 0; i < MAX_FRAME_FAILURES * 3; ++i) {
        buffer_pool_buf &buf = pool.next_capture();
        /* Slot returned for the next capture must be marked released. */
        EXPECT(buf.ready_capture() == true);
        EXPECT(buf.ready_encode() == false);
    }

    EXPECT(pool.size() == MAX_FRAME_FAILURES);

    /* Encoder can still pull something after backlog pressure. */
    bool saw_encode = false;
    for (int i = 0; i < MAX_FRAME_FAILURES; ++i) {
        if (pool.encode().ready_encode()) {
            saw_encode = true;
            pool.next_encode();
            break;
        }
        pool.next_capture();
    }
    EXPECT(saw_encode);
}

/* Capture + encode keep the ring consistent (no stuck available flags). */
static void test_capture_encode_handshake()
{
    buffer_pool<buffer_pool_buf, MAX_FRAME_FAILURES> pool;

    for (int round = 0; round < 32; ++round) {
        pool.next_capture();
        EXPECT(pool.encode().ready_encode() == true);
        pool.next_encode();
    }

    /* Steady capture/encode should not force pool growth. */
    EXPECT(pool.size() == INITIAL_BUFFERS_SIZE);
}

/* Concurrent capture/encode threads must not deadlock. */
static void test_concurrent_capture_encode()
{
    buffer_pool<buffer_pool_buf, MAX_FRAME_FAILURES> pool;
    std::atomic<bool> stop{false};
    std::atomic<int> captures{0};
    std::atomic<int> encodes{0};

    std::thread cap([&] {
        while (!stop.load()) {
            pool.next_capture();
            captures.fetch_add(1);
            std::this_thread::yield();
        }
    });

    std::thread enc([&] {
        while (!stop.load()) {
            if (pool.encode().ready_encode()) {
                pool.next_encode();
                encodes.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
    cap.join();
    enc.join();

    EXPECT(captures.load() > 0);
    EXPECT(encodes.load() > 0);
    EXPECT(pool.size() <= MAX_FRAME_FAILURES);
}

/* Full pool with encode_idx == capture_idx path must not abort. */
static void test_full_same_index_path()
{
    buffer_pool<buffer_pool_buf, MAX_FRAME_FAILURES> pool;

    /* Force growth to max with no encodes. */
    for (int i = 0; i < MAX_FRAME_FAILURES * 2; ++i) {
        pool.next_capture();
    }
    EXPECT(pool.size() == MAX_FRAME_FAILURES);

    /* Keep hammering — exercises drop-oldest and same-index fallback. */
    for (int i = 0; i < 64; ++i) {
        buffer_pool_buf &buf = pool.next_capture();
        EXPECT(&buf != nullptr);
    }
}

int main()
{
    test_initial_size();
    test_grows_until_cap();
    test_drop_oldest_when_full();
    test_capture_encode_handshake();
    test_concurrent_capture_encode();
    test_full_same_index_path();

    if (g_failures) {
        std::cerr << g_failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "buffer-pool-test: ok\n";
    return 0;
}
