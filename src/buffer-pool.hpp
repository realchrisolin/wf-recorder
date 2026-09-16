#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <mutex>
#include <type_traits>

/* Prefer newest frames under encode backlog (interactive capture). Cap
 * growth so we never abort; when full, discard the oldest pending encode
 * and reuse that slot — never drop the frame just captured. */
#define MAX_FRAME_FAILURES 16
#define INITIAL_BUFFERS_SIZE 4
/* Soft backpressure: pause capture before the ring is forced to drop. */
#define BUFFER_POOL_HIGH_WATER (MAX_FRAME_FAILURES - 2)

class buffer_pool_buf
{
public:
    bool ready_capture() const
    {
        return released;
    }

    bool ready_encode() const
    {
        return available;
    }

    std::atomic<bool> released{true}; // if the buffer can be used to store new pending frames
    std::atomic<bool> available{false}; // if the buffer can be used to feed the encoder
};

template <class T, int N>
class buffer_pool
{
public:
    static_assert(std::is_base_of<buffer_pool_buf, T>::value, "T must be subclass of buffer_pool_buf");

    buffer_pool()
    {
        for (size_t i = 0; i < bufs_size; ++i) {
            bufs[i] = new T;
        }
    }

    ~buffer_pool()
    {
        for (size_t i = 0; i < bufs_size; ++i) {
            delete bufs[i];
        }
    }

    size_t size() const
    {
        return bufs_size;
    }

    const T* at(size_t i) const
    {
        return bufs[i];
    }

    T* at(size_t i)
    {
        return bufs[i];
    }

    /* Frames queued for encode (available=true). Used for soft backpressure. */
    size_t pending() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t n = 0;
        for (size_t i = 0; i < bufs_size; ++i) {
            if (bufs[i]->ready_encode()) {
                ++n;
            }
        }
        return n;
    }

    T& capture()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return *bufs[capture_idx];
    }

    T& encode()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return *bufs[encode_idx];
    }

    // Signal that the current capture buffer has been successfully obtained
    // from the compositor and select the next buffer to capture in.
    T& next_capture()
    {
        std::lock_guard<std::mutex> lock(mutex);
        int next = (capture_idx + 1) % bufs_size;
        if (!bufs[next]->ready_capture())
        {
            if (bufs_size < MAX_FRAME_FAILURES)
            {
                bufs_size++;
                std::cerr << "bufs_size: " << bufs_size << std::endl;
                bufs[bufs_size - 1] = new T;
                next = (capture_idx + 1) % bufs_size;
            }
            else if (encode_idx != capture_idx)
            {
                /* Drop oldest pending encode; keep the frame we just captured. */
                static bool warned = false;
                if (!warned)
                {
                    std::cerr << "buffer pool full; dropping oldest pending frame"
                              << std::endl;
                    warned = true;
                }
                bufs[encode_idx]->available = false;
                bufs[encode_idx]->released = true;
                next = encode_idx;
                encode_idx = (encode_idx + 1) % bufs_size;
            }
            else
            {
                bufs[capture_idx]->released = true;
                bufs[capture_idx]->available = false;
                return *bufs[capture_idx];
            }
        }
        bufs[capture_idx]->released = false;
        bufs[capture_idx]->available = true;
        capture_idx = next;
        bufs[capture_idx]->released = true;
        bufs[capture_idx]->available = false;
        return *bufs[capture_idx];
    }

    // Signal that the encode buffer has been submitted for encoding
    // and select the next buffer for encoding.
    T& next_encode()
    {
        std::lock_guard<std::mutex> lock(mutex);
        bufs[encode_idx]->available = false;
        bufs[encode_idx]->released = true;
        encode_idx = (encode_idx + 1) % bufs_size;
        return *bufs[encode_idx];
    }

private:
    mutable std::mutex mutex;
    std::array<T*, MAX_FRAME_FAILURES> bufs;
    size_t bufs_size = INITIAL_BUFFERS_SIZE;
    int capture_idx = 0; // head
    int encode_idx = 0; // tail
};
