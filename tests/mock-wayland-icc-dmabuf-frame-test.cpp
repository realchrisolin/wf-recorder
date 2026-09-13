/*
 * Layer 7 — mock Wayland ICC DMA-BUF frame session.
 *
 * Session advertises dmabuf_format; client creates a linux-dmabuf wl_buffer
 * (memfd stand-in), attaches, captures, and expects ready — no Hyprland/GBM.
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-server-protocol.h>

#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-server-protocol.h"
#include "ext-image-capture-source-v1-server-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "        \
                      << #cond << std::endl;                                   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static constexpr uint32_t kWidth = 64;
static constexpr uint32_t kHeight = 64;
static constexpr uint32_t kFormat = DRM_FORMAT_XRGB8888;

/* ---------- linux-dmabuf server (accept memfd as stand-in) ---------- */

struct params_state
{
    int fd = -1;
    uint32_t stride = 0;
    bool used = false;
};

static void buffer_destroy(struct wl_client *, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface buffer_impl = {
    .destroy = buffer_destroy,
};

static void params_resource_destroy(struct wl_resource *resource)
{
    auto *st = static_cast<params_state *>(wl_resource_get_user_data(resource));
    if (st) {
        if (st->fd >= 0)
            close(st->fd);
        delete st;
        wl_resource_set_user_data(resource, nullptr);
    }
}

static void params_destroy(struct wl_client *, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void params_add(struct wl_client *, struct wl_resource *resource, int32_t fd,
    uint32_t plane_idx, uint32_t /*offset*/, uint32_t stride, uint32_t, uint32_t)
{
    auto *st = static_cast<params_state *>(wl_resource_get_user_data(resource));
    if (!st || plane_idx != 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    if (st->fd >= 0)
        close(st->fd);
    st->fd = fd;
    st->stride = stride;
}

static void params_create(struct wl_client *, struct wl_resource *resource,
    int32_t, int32_t, uint32_t, uint32_t)
{
    /* Prefer create_immed in the client; still accept create. */
    auto *st = static_cast<params_state *>(wl_resource_get_user_data(resource));
    if (!st || st->used || st->fd < 0) {
        zwp_linux_buffer_params_v1_send_failed(resource);
        return;
    }
    st->used = true;
    /* Async create would need a new_id; client uses create_immed instead. */
    zwp_linux_buffer_params_v1_send_failed(resource);
}

static void params_create_immed(struct wl_client *client, struct wl_resource *resource,
    uint32_t buffer_id, int32_t width, int32_t height, uint32_t format, uint32_t)
{
    auto *st = static_cast<params_state *>(wl_resource_get_user_data(resource));
    if (!st || st->used || st->fd < 0 || width <= 0 || height <= 0 || format != kFormat) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
            "incomplete dmabuf params");
        return;
    }
    st->used = true;
    struct wl_resource *buf = wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
    if (!buf) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
            "failed to create buffer");
        return;
    }
    wl_resource_set_implementation(buf, &buffer_impl, nullptr, nullptr);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

static void dmabuf_destroy(struct wl_client *, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void dmabuf_create_params(struct wl_client *client, struct wl_resource *resource,
    uint32_t params_id)
{
    auto *st = new params_state;
    struct wl_resource *params = wl_resource_create(client,
        &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(resource), params_id);
    if (!params) {
        delete st;
        return;
    }
    wl_resource_set_implementation(params, &params_impl, st, params_resource_destroy);
}

static void dmabuf_get_default_feedback(struct wl_client *, struct wl_resource *resource,
    uint32_t)
{
    wl_resource_post_error(resource, 0, "feedback not implemented in mock");
}

static void dmabuf_get_surface_feedback(struct wl_client *, struct wl_resource *resource,
    uint32_t, struct wl_resource *)
{
    wl_resource_post_error(resource, 0, "feedback not implemented in mock");
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
    .get_default_feedback = dmabuf_get_default_feedback,
    .get_surface_feedback = dmabuf_get_surface_feedback,
};

static void bind_dmabuf(struct wl_client *client, void *, uint32_t version, uint32_t id)
{
    struct wl_resource *res = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface,
        static_cast<int>(version), id);
    if (!res)
        return;
    wl_resource_set_implementation(res, &dmabuf_impl, nullptr, nullptr);
    zwp_linux_dmabuf_v1_send_format(res, kFormat);
    if (version >= 3) {
        const uint64_t mod = DRM_FORMAT_MOD_LINEAR;
        zwp_linux_dmabuf_v1_send_modifier(res, kFormat,
            static_cast<uint32_t>(mod >> 32), static_cast<uint32_t>(mod & 0xffffffffu));
    }
}

static void bind_wl_output(struct wl_client *client, void *, uint32_t version, uint32_t id)
{
    struct wl_resource *res = wl_resource_create(client, &wl_output_interface,
        static_cast<int>(version), id);
    if (!res)
        return;
    wl_resource_set_implementation(res, nullptr, nullptr, nullptr);
    wl_output_send_geometry(res, 0, 0, 300, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN,
        "mock", "Mock-1", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
        static_cast<int32_t>(kWidth), static_cast<int32_t>(kHeight), 60000);
    if (wl_resource_get_version(res) >= 2)
        wl_output_send_done(res);
}

/* ---------- ICC session / frame (DMA path) ---------- */

struct session_state;

struct frame_owner
{
    session_state *session = nullptr;
    bool captured = false;
};

struct session_state
{
    struct wl_resource *frame = nullptr;
};

static void frame_resource_destroy(struct wl_resource *resource)
{
    auto *owner = static_cast<frame_owner *>(wl_resource_get_user_data(resource));
    if (owner) {
        if (owner->session && owner->session->frame == resource)
            owner->session->frame = nullptr;
        delete owner;
    }
}

static void frame_destroy(struct wl_client *, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void frame_attach_buffer(struct wl_client *, struct wl_resource *, struct wl_resource *) {}
static void frame_damage_buffer(struct wl_client *, struct wl_resource *, int32_t, int32_t, int32_t, int32_t) {}

static void frame_capture(struct wl_client *, struct wl_resource *resource)
{
    auto *owner = static_cast<frame_owner *>(wl_resource_get_user_data(resource));
    if (!owner || owner->captured)
        return;
    owner->captured = true;
    ext_image_copy_capture_frame_v1_send_transform(resource, WL_OUTPUT_TRANSFORM_NORMAL);
    ext_image_copy_capture_frame_v1_send_damage(resource, 0, 0,
        static_cast<int32_t>(kWidth), static_cast<int32_t>(kHeight));
    ext_image_copy_capture_frame_v1_send_presentation_time(resource, 0, 2, 0);
    ext_image_copy_capture_frame_v1_send_ready(resource);
}

static const struct ext_image_copy_capture_frame_v1_interface frame_impl = {
    .destroy = frame_destroy,
    .attach_buffer = frame_attach_buffer,
    .damage_buffer = frame_damage_buffer,
    .capture = frame_capture,
};

static void session_create_frame(struct wl_client *client, struct wl_resource *resource,
    uint32_t id)
{
    auto *sess = static_cast<session_state *>(wl_resource_get_user_data(resource));
    if (sess && sess->frame) {
        wl_resource_post_error(resource,
            EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME, "duplicate frame");
        return;
    }
    auto *owner = new frame_owner;
    owner->session = sess;
    struct wl_resource *frame = wl_resource_create(client,
        &ext_image_copy_capture_frame_v1_interface, wl_resource_get_version(resource), id);
    if (!frame) {
        delete owner;
        return;
    }
    wl_resource_set_implementation(frame, &frame_impl, owner, frame_resource_destroy);
    if (sess)
        sess->frame = frame;
}

static void session_destroy(struct wl_client *, struct wl_resource *resource)
{
    delete static_cast<session_state *>(wl_resource_get_user_data(resource));
    wl_resource_set_user_data(resource, nullptr);
    wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_session_v1_interface session_impl = {
    .create_frame = session_create_frame,
    .destroy = session_destroy,
};

static void manager_create_session(struct wl_client *client, struct wl_resource *resource,
    uint32_t id, struct wl_resource *, uint32_t)
{
    auto *sess = new session_state;
    struct wl_resource *session = wl_resource_create(client,
        &ext_image_copy_capture_session_v1_interface, wl_resource_get_version(resource), id);
    if (!session) {
        delete sess;
        return;
    }
    wl_resource_set_implementation(session, &session_impl, sess,
        [](struct wl_resource *res) {
            delete static_cast<session_state *>(wl_resource_get_user_data(res));
        });

    ext_image_copy_capture_session_v1_send_buffer_size(session, kWidth, kHeight);

    struct wl_array device;
    wl_array_init(&device);
    /* Empty device node list is acceptable for this mock. */
    ext_image_copy_capture_session_v1_send_dmabuf_device(session, &device);
    wl_array_release(&device);

    struct wl_array modifiers;
    wl_array_init(&modifiers);
    auto *mod = static_cast<uint64_t *>(wl_array_add(&modifiers, sizeof(uint64_t)));
    if (mod)
        *mod = DRM_FORMAT_MOD_LINEAR;
    ext_image_copy_capture_session_v1_send_dmabuf_format(session, kFormat, &modifiers);
    wl_array_release(&modifiers);

    ext_image_copy_capture_session_v1_send_done(session);
}

static void manager_create_pointer_cursor_session(struct wl_client *, struct wl_resource *resource,
    uint32_t, struct wl_resource *, struct wl_resource *)
{
    wl_resource_post_error(resource, 0, "cursor session not implemented");
}

static void manager_destroy(struct wl_client *, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_manager_v1_interface manager_impl = {
    .create_session = manager_create_session,
    .create_pointer_cursor_session = manager_create_pointer_cursor_session,
    .destroy = manager_destroy,
};

static void source_destroy(struct wl_client *, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct ext_image_capture_source_v1_interface source_impl = {
    .destroy = source_destroy,
};

static void output_mgr_create_source(struct wl_client *client, struct wl_resource *resource,
    uint32_t id, struct wl_resource *)
{
    struct wl_resource *src = wl_resource_create(client, &ext_image_capture_source_v1_interface,
        wl_resource_get_version(resource), id);
    if (src)
        wl_resource_set_implementation(src, &source_impl, nullptr, nullptr);
}

static void output_mgr_destroy(struct wl_client *, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface output_mgr_impl = {
    .create_source = output_mgr_create_source,
    .destroy = output_mgr_destroy,
};

static void bind_copy_manager(struct wl_client *client, void *, uint32_t version, uint32_t id)
{
    struct wl_resource *res = wl_resource_create(client,
        &ext_image_copy_capture_manager_v1_interface, static_cast<int>(version), id);
    if (res)
        wl_resource_set_implementation(res, &manager_impl, nullptr, nullptr);
}

static void bind_output_mgr(struct wl_client *client, void *, uint32_t version, uint32_t id)
{
    struct wl_resource *res = wl_resource_create(client,
        &ext_output_image_capture_source_manager_v1_interface, static_cast<int>(version), id);
    if (res)
        wl_resource_set_implementation(res, &output_mgr_impl, nullptr, nullptr);
}

/* ---------- compositor shell ---------- */

struct mock_compositor
{
    struct wl_display *display = nullptr;
    std::vector<struct wl_global *> globals;
    std::atomic<bool> stop{false};
    std::string sock_name;
};

static bool start_mock(mock_compositor &mc)
{
    mc.display = wl_display_create();
    if (!mc.display)
        return false;
    const char *sock = wl_display_add_socket_auto(mc.display);
    if (!sock)
        return false;
    mc.sock_name = sock;

    auto add = [&](const wl_interface *iface, int ver, wl_global_bind_func_t bind) {
        struct wl_global *g = wl_global_create(mc.display, iface, ver, nullptr, bind);
        EXPECT(g != nullptr);
        if (g)
            mc.globals.push_back(g);
    };
    add(&wl_output_interface, 2, bind_wl_output);
    add(&zwp_linux_dmabuf_v1_interface, 4, bind_dmabuf);
    add(&ext_output_image_capture_source_manager_v1_interface, 1, bind_output_mgr);
    add(&ext_image_copy_capture_manager_v1_interface, 1, bind_copy_manager);
    return true;
}

static void run_server(mock_compositor *mc)
{
    while (!mc->stop.load()) {
        wl_event_loop_dispatch(wl_display_get_event_loop(mc->display), 20);
        wl_display_flush_clients(mc->display);
    }
}

/* ---------- client ---------- */

struct client_globals
{
    struct wl_output *output = nullptr;
    struct zwp_linux_dmabuf_v1 *dmabuf = nullptr;
    struct ext_output_image_capture_source_manager_v1 *output_mgr = nullptr;
    struct ext_image_copy_capture_manager_v1 *copy_mgr = nullptr;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version)
{
    auto *g = static_cast<client_globals *>(data);
    if (strcmp(interface, "wl_output") == 0)
        g->output = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, 2));
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
        g->dmabuf = static_cast<zwp_linux_dmabuf_v1 *>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, version < 4 ? version : 4));
    else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
        g->output_mgr = static_cast<ext_output_image_capture_source_manager_v1 *>(
            wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, version));
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
        g->copy_mgr = static_cast<ext_image_copy_capture_manager_v1 *>(
            wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, version));
}

static void registry_global_remove(void *, struct wl_registry *, uint32_t) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

struct session_client
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dmabuf_format = 0;
    bool got_dmabuf_device = false;
    bool done = false;
    bool ready = false;
    bool failed = false;
};

static void sess_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *,
    uint32_t width, uint32_t height)
{
    auto *s = static_cast<session_client *>(data);
    s->width = width;
    s->height = height;
}
static void sess_shm_format(void *, struct ext_image_copy_capture_session_v1 *, uint32_t) {}
static void sess_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *, struct wl_array *)
{
    static_cast<session_client *>(data)->got_dmabuf_device = true;
}
static void sess_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *,
    uint32_t format, struct wl_array *)
{
    static_cast<session_client *>(data)->dmabuf_format = format;
}
static void sess_done(void *data, struct ext_image_copy_capture_session_v1 *)
{
    static_cast<session_client *>(data)->done = true;
}
static void sess_stopped(void *, struct ext_image_copy_capture_session_v1 *) {}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = sess_buffer_size,
    .shm_format = sess_shm_format,
    .dmabuf_device = sess_dmabuf_device,
    .dmabuf_format = sess_dmabuf_format,
    .done = sess_done,
    .stopped = sess_stopped,
};

static void frame_transform(void *, struct ext_image_copy_capture_frame_v1 *, uint32_t) {}
static void frame_damage(void *, struct ext_image_copy_capture_frame_v1 *, int32_t, int32_t, int32_t, int32_t) {}
static void frame_presentation_time(void *, struct ext_image_copy_capture_frame_v1 *, uint32_t, uint32_t, uint32_t) {}
static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *)
{
    static_cast<session_client *>(data)->ready = true;
}
static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *, uint32_t)
{
    static_cast<session_client *>(data)->failed = true;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static int create_memfd(size_t size)
{
    char name[] = "/tmp/wf-icc-dmabuf-XXXXXX";
    int fd = mkostemp(name, O_CLOEXEC);
    if (fd < 0)
        return -1;
    unlink(name);
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void test_dmabuf_frame_ready_path()
{
    mock_compositor mc;
    EXPECT(start_mock(mc));
    std::thread thr(run_server, &mc);

    struct wl_display *dpy = wl_display_connect(mc.sock_name.c_str());
    EXPECT(dpy != nullptr);
    client_globals g{};
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &registry_listener, &g);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    EXPECT(g.output && g.dmabuf && g.output_mgr && g.copy_mgr);

    auto *source = ext_output_image_capture_source_manager_v1_create_source(g.output_mgr, g.output);
    EXPECT(source != nullptr);

    session_client sc{};
    auto *session = ext_image_copy_capture_manager_v1_create_session(g.copy_mgr, source, 0);
    EXPECT(session != nullptr);
    ext_image_copy_capture_session_v1_add_listener(session, &session_listener, &sc);
    for (int i = 0; i < 50 && !sc.done; ++i)
        wl_display_dispatch(dpy);

    EXPECT(sc.done);
    EXPECT(sc.got_dmabuf_device);
    EXPECT(sc.width == kWidth && sc.height == kHeight);
    EXPECT(sc.dmabuf_format == kFormat);

    const uint32_t stride = kWidth * 4;
    const size_t bytes = static_cast<size_t>(stride) * kHeight;
    int fd = create_memfd(bytes);
    EXPECT(fd >= 0);

    auto *params = zwp_linux_dmabuf_v1_create_params(g.dmabuf);
    EXPECT(params != nullptr);
    const uint64_t mod = DRM_FORMAT_MOD_LINEAR;
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride,
        static_cast<uint32_t>(mod >> 32), static_cast<uint32_t>(mod & 0xffffffffu));
    close(fd);
    auto *buffer = zwp_linux_buffer_params_v1_create_immed(params,
        static_cast<int32_t>(kWidth), static_cast<int32_t>(kHeight), kFormat, 0);
    EXPECT(buffer != nullptr);
    zwp_linux_buffer_params_v1_destroy(params);
    wl_display_roundtrip(dpy);

    auto *frame = ext_image_copy_capture_session_v1_create_frame(session);
    EXPECT(frame != nullptr);
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, &sc);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0,
        static_cast<int32_t>(kWidth), static_cast<int32_t>(kHeight));
    ext_image_copy_capture_frame_v1_capture(frame);

    for (int i = 0; i < 50 && !sc.ready && !sc.failed; ++i)
        wl_display_dispatch(dpy);

    EXPECT(sc.ready);
    EXPECT(!sc.failed);

    ext_image_copy_capture_frame_v1_destroy(frame);
    wl_buffer_destroy(buffer);
    ext_image_copy_capture_session_v1_destroy(session);
    ext_image_capture_source_v1_destroy(source);
    wl_registry_destroy(reg);
    wl_display_disconnect(dpy);

    mc.stop.store(true);
    thr.join();
    for (auto *gl : mc.globals)
        wl_global_destroy(gl);
    wl_display_destroy(mc.display);

    std::cout << "dmabuf frame ready path: ok\n";
}

int main()
{
    if (!getenv("XDG_RUNTIME_DIR")) {
        std::cerr << "mock-wayland-icc-dmabuf-frame-test: SKIP (XDG_RUNTIME_DIR unset)\n";
        return 77;
    }

    test_dmabuf_frame_ready_path();

    if (g_failures) {
        std::cerr << g_failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "mock-wayland-icc-dmabuf-frame-test: ok\n";
    return 0;
}
