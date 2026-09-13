/*
 * Layer 5b — mock Wayland compositor advertising ICC globals.
 *
 * Spins a wayland-server display, publishes the registry globals an ICC
 * wf-recorder needs, and verifies a client sees a complete output-capture
 * set (and that omitting copy-capture fails the shared check).
 */

#include "icc-proto-check.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-server.h>

/* Interface structs live in the generated protocol .c linked via wf_protos. */
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "        \
                      << #cond << std::endl;                                   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

struct mock_compositor
{
    struct wl_display *display = nullptr;
    std::vector<struct wl_global *> globals;
    std::atomic<bool> stop{false};
    std::string sock_name;
};

static void bind_stub(struct wl_client *client, void *data, uint32_t version,
    uint32_t id)
{
    auto *iface = static_cast<const struct wl_interface *>(data);
    struct wl_resource *res = wl_resource_create(client, iface, static_cast<int>(version), id);
    if (res)
        wl_resource_set_implementation(res, nullptr, nullptr, nullptr);
}

static void add_global(mock_compositor &mc, const struct wl_interface *iface, int version)
{
    struct wl_global *g = wl_global_create(mc.display, iface, version, (void *)iface, bind_stub);
    EXPECT(g != nullptr);
    if (g)
        mc.globals.push_back(g);
}

/* Minimal wl_output so clients see an output global. */
static void bind_wl_output(struct wl_client *client, void *, uint32_t version, uint32_t id)
{
    struct wl_resource *res = wl_resource_create(client, &wl_output_interface,
        static_cast<int>(version), id);
    if (!res)
        return;
    wl_resource_set_implementation(res, nullptr, nullptr, nullptr);
    /* Send a geometry/mode/done so list-oriented clients aren't empty-handed. */
    wl_output_send_geometry(res, 0, 0, 300, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN,
        "mock", "Mock-1", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
        1920, 1080, 30000);
    if (wl_resource_get_version(res) >= 2)
        wl_output_send_done(res);
}

static void bind_wl_shm(struct wl_client *client, void *, uint32_t version, uint32_t id)
{
    struct wl_resource *res = wl_resource_create(client, &wl_shm_interface,
        static_cast<int>(version), id);
    if (!res)
        return;
    wl_resource_set_implementation(res, nullptr, nullptr, nullptr);
    wl_shm_send_format(res, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(res, WL_SHM_FORMAT_XRGB8888);
}

static bool start_mock(mock_compositor &mc, bool with_copy_capture)
{
    mc.display = wl_display_create();
    if (!mc.display)
        return false;

    /* Auto socket wayland-XXXX under XDG_RUNTIME_DIR. */
    const char *sock = wl_display_add_socket_auto(mc.display);
    if (sock == nullptr)
        return false;
    mc.sock_name = sock;

    {
        struct wl_global *g = wl_global_create(mc.display, &wl_shm_interface, 1,
            nullptr, bind_wl_shm);
        EXPECT(g != nullptr);
        if (g)
            mc.globals.push_back(g);
    }
    {
        struct wl_global *g = wl_global_create(mc.display, &wl_output_interface, 2,
            nullptr, bind_wl_output);
        EXPECT(g != nullptr);
        if (g)
            mc.globals.push_back(g);
    }

    add_global(mc, &zxdg_output_manager_v1_interface, 2);
    add_global(mc, &zwp_linux_dmabuf_v1_interface, 4);
    add_global(mc, &ext_output_image_capture_source_manager_v1_interface, 1);
    if (with_copy_capture)
        add_global(mc, &ext_image_copy_capture_manager_v1_interface, 1);
    add_global(mc, &ext_foreign_toplevel_list_v1_interface, 1);
    add_global(mc, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);

    return true;
}

static void run_server(mock_compositor *mc)
{
    while (!mc->stop.load()) {
        wl_event_loop_dispatch(wl_display_get_event_loop(mc->display), 50);
        wl_display_flush_clients(mc->display);
    }
}

struct client_state
{
    icc_registry_flags flags;
    bool capture_toplevel = false;
    struct wl_registry *registry = nullptr;
};

static void registry_global(void *data, struct wl_registry *, uint32_t,
    const char *interface, uint32_t)
{
    auto *st = static_cast<client_state *>(data);
    icc_note_global(st->flags, interface, st->capture_toplevel);
}

static void registry_global_remove(void *, struct wl_registry *, uint32_t) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static icc_registry_flags client_scan(const std::string &sock, bool capture_toplevel)
{
    client_state st;
    st.capture_toplevel = capture_toplevel;

    struct wl_display *dpy = wl_display_connect(sock.c_str());
    EXPECT(dpy != nullptr);
    if (!dpy)
        return st.flags;

    st.registry = wl_display_get_registry(dpy);
    wl_registry_add_listener(st.registry, &registry_listener, &st);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    wl_registry_destroy(st.registry);
    wl_display_disconnect(dpy);
    return st.flags;
}

static void test_complete_output_registry()
{
    mock_compositor mc;
    EXPECT(start_mock(mc, true));
    std::thread thr(run_server, &mc);

    auto flags = client_scan(mc.sock_name, false);
    const char *missing = icc_missing_requirement(flags, false, true);
    if (missing)
        std::cerr << "unexpected missing: " << missing << "\n";
    EXPECT(missing == nullptr);
    EXPECT(flags.has_copy_capture_manager);
    EXPECT(flags.has_output_image_capture);
    EXPECT(flags.output_count >= 1);

    mc.stop.store(true);
    thr.join();
    for (auto *g : mc.globals)
        wl_global_destroy(g);
    wl_display_destroy(mc.display);
}

static void test_missing_copy_capture_fails_check()
{
    mock_compositor mc;
    EXPECT(start_mock(mc, false));
    std::thread thr(run_server, &mc);

    auto flags = client_scan(mc.sock_name, false);
    const char *missing = icc_missing_requirement(flags, false, false);
    EXPECT(missing != nullptr);
    EXPECT(std::string(missing).find("ext-image-copy-capture-manager-v1") != std::string::npos);

    mc.stop.store(true);
    thr.join();
    for (auto *g : mc.globals)
        wl_global_destroy(g);
    wl_display_destroy(mc.display);
}

static void test_toplevel_mode_sees_toplevel_globals()
{
    mock_compositor mc;
    EXPECT(start_mock(mc, true));
    std::thread thr(run_server, &mc);

    auto flags = client_scan(mc.sock_name, true);
    EXPECT(flags.has_foreign_toplevel_list);
    EXPECT(flags.has_toplevel_image_capture);
    /* Output capture manager is advertised but ignored in toplevel classify. */
    EXPECT(flags.has_output_image_capture == false);
    EXPECT(icc_missing_requirement(flags, true, false) == nullptr);

    mc.stop.store(true);
    thr.join();
    for (auto *g : mc.globals)
        wl_global_destroy(g);
    wl_display_destroy(mc.display);
}

int main()
{
    if (!getenv("XDG_RUNTIME_DIR")) {
        std::cerr << "mock-wayland-icc-registry-test: SKIP (XDG_RUNTIME_DIR unset)\n";
        return 77;
    }

    test_complete_output_registry();
    test_missing_copy_capture_fails_check();
    test_toplevel_mode_sees_toplevel_globals();

    if (g_failures) {
        std::cerr << g_failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "mock-wayland-icc-registry-test: ok\n";
    return 0;
}
