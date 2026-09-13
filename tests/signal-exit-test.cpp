/*
 * Contract test: graceful termination from a signal context must only flip
 * flags — never tear down Wayland (dispatch races SIGINT / SIGTERM).
 *
 * Mirrors handle_graceful_termination() in src/main.cpp.
 */

#include <atomic>
#include <csignal>
#include <iostream>

static std::atomic<bool> exit_main_loop{false};
static std::atomic<bool> buffer_copy_done{false};
static std::atomic<int> forbidden_calls{0};

/* Stand-ins for Wayland teardown — must never be invoked from the handler. */
extern "C" void wl_display_disconnect(void *)
{
    forbidden_calls.fetch_add(1);
}

extern "C" void wl_display_roundtrip(void *)
{
    forbidden_calls.fetch_add(1);
}

static void handle_graceful_termination(int)
{
    /* Flags only — Wayland teardown from a signal handler races dispatch. */
    exit_main_loop.store(true);
    buffer_copy_done.store(true);
}

int main()
{
    exit_main_loop.store(false);
    buffer_copy_done.store(false);
    forbidden_calls.store(0);

    handle_graceful_termination(SIGINT);

    if (!exit_main_loop.load() || !buffer_copy_done.load()) {
        std::cerr << "signal-exit-test: FAIL flags not set\n";
        return 1;
    }
    if (forbidden_calls.load() != 0) {
        std::cerr << "signal-exit-test: FAIL handler performed Wayland teardown\n";
        return 1;
    }

    std::cout << "signal-exit-test: ok\n";
    return 0;
}
