/*
 * Layer 6 — mock Wayland ICC frame session.
 *
 * Drives create_source → create_session → (buffer_size/shm_format/done) →
 * create_frame → attach/damage/capture → ready, without Hyprland.
 */

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-server-protocol.h>

#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-server-protocol.h"
#include "ext-image-capture-source-v1-server-protocol.h"

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "        \
                      << #cond << std::endl;                                   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

/* ---------- minimal wl_shm server ---------- */

struct shm_pool_data
{
    int fd = -1;
    size_t size = 0;
    void *data = nullptr;
};

static void shm_pool_destroy(struct wl_client *, struct wl_resource *resource)
{
    auto *pool = static_cast<shm_pool_data *>(wl_resource_get_user_data(resource));
    if (pool) {
        if (pool->data && pool->data != MAP_FAILED)
            munmap(pool->data, pool->size);
        if (pool->fd >= 0)
            close(pool->fd);
        delete pool;
    }
    wl_resource_destroy(resource);
}

static void shm_pool_resize(struct wl_client *, struct wl_resource *resource, int32_t size)
{
    auto *pool = static_cast<shm_pool_data *>(wl_resource_get_user_data(resource));
    if (!pool || size <= 0)
        return;
    void *n = mremap(pool->data, pool->size, static_cast<size_t>(size), MREMAP_MAYMOVE);
    if (n != MAP_FAILED) {
        pool->data = n;
        pool->size = static_cast<size_t>(size);
    }
}

static void shm_buffer_destroy(struct wl_client *, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface shm_buffer_impl = {
    .destroy = shm_buffer_destroy,
};

static void shm_pool_create_buffer(struct wl_client *client, struct wl_resource *resource,
    uint32_t id, int32_t, int32_t, int32_t, int32_t, uint32_t)
{
    struct wl_resource *buf = wl_resource_create(client, &wl_buffer_interface,
        wl_resource_get_version(resource), id);
    if (buf)
        wl_resource_set_implementation(buf, &shm_buffer_impl, nullptr, nullptr);
}

static const struct wl_shm_pool_interface shm_pool_impl = {
    .create_buffer = shm_pool_create_buffer,
    .destroy = shm_pool_destroy,
    .resize = shm_pool_resize,
};

static void shm_create_pool(struct wl_client *client, struct wl_resource *resource,
    uint32_t id, int32_t fd, int32_t size)
{
    auto *pool = new shm_pool_data;
    pool->fd = fd;
    pool->size = size > 0 ? static_cast<size_t>(size) : 0;
    pool->data = mmap(nullptr, pool->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_resource *res = wl_resource_create(client, &wl_shm_pool_interface,
        wl_resource_get_version(resource), id);
    if (!res) {
        if (pool->data && pool->data != MAP_FAILED)
            munmap(pool->data, pool->size);
        close(fd);
        delete pool;
        return;
    }
    wl_resource_set_implementation(res, &shm_pool_impl, pool, nullptr);
}

static void shm_release(struct wl_client *, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct wl_shm_interface shm_impl = {
    .create_pool = shm_create_pool,
    .release = shm_release,
};

static void bind_wl_shm(struct wl_client *client, void *, uint32_t version, uint32_t id)
{
    struct wl_resource *res = wl_resource_create(client, &wl_shm_interface,
        static_cast<int>(version), id);
    if (!res)
        return;
    wl_resource_set_implementation(res, &shm_impl, nullptr, nullptr);
    wl_shm_send_format(res, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(res, WL_SHM_FORMAT_XRGB8888);
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
        64, 64, 30000);
    if (wl_resource_get_version(res) >= 2)
        wl_output_send_done(res);
}

/* ---------- ICC server objects ---------- */

struct session_state;

struct frame_owner
{
    session_state *session = nullptr;
    bool captured = false;
    bool had_attach = false;
    bool had_damage = false;
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

static void frame_attach_buffer(struct wl_client *, struct wl_resource *resource,
    struct wl_resource *)
{
    auto *owner = static_cast<frame_owner *>(wl_resource_get_user_data(resource));
    if (owner)
        owner->had_attach = true;
}

static void frame_damage_buffer(struct wl_client *, struct wl_resource *resource,
    int32_t, int32_t, int32_t, int32_t)
{
    auto *owner = static_cast<frame_owner *>(wl_resource_get_user_data(resource));
    if (owner)
        owner->had_damage = true;
}

static void frame_capture(struct wl_client *, struct wl_resource *resource)
{
    auto *owner = static_cast<frame_owner *>(wl_resource_get_user_data(resource));
    if (!owner || owner->captured)
        return;
    owner->captured = true;
    ext_image_copy_capture_frame_v1_send_transform(resource, WL_OUTPUT_TRANSFORM_NORMAL);
    ext_image_copy_capture_frame_v1_send_damage(resource, 0, 0, 64, 64);
    ext_image_copy_capture_frame_v1_send_presentation_time(resource, 0, 1, 0);
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
            EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
            "duplicate frame");
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
    uint32_t id, struct wl_resource * /*source*/, uint32_t /*options*/)
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
    ext_image_copy_capture_session_v1_send_buffer_size(session, 64, 64);
    ext_image_copy_capture_session_v1_send_shm_format(session, WL_SHM_FORMAT_XRGB8888);
    ext_image_copy_capture_session_v1_send_done(session);
}

static void manager_create_pointer_cursor_session(struct wl_client *, struct wl_resource *resource,
    uint32_t, struct wl_resource *, struct wl_resource *)
{
    wl_resource_post_error(resource, 0, "cursor session not implemented in mock");
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
    uint32_t id, struct wl_resource * /*output*/)
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
    add(&wl_shm_interface, 1, bind_wl_shm);
    add(&wl_output_interface, 2, bind_wl_output);
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
    struct wl_shm *shm = nullptr;
    struct wl_output *output = nullptr;
    struct ext_output_image_capture_source_manager_v1 *output_mgr = nullptr;
    struct ext_image_copy_capture_manager_v1 *copy_mgr = nullptr;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version)
{
    auto *g = static_cast<client_globals *>(data);
    if (strcmp(interface, "wl_shm") == 0)
        g->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (strcmp(interface, "wl_output") == 0)
        g->output = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, 2));
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
    uint32_t shm_format = 0;
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

static void sess_shm_format(void *data, struct ext_image_copy_capture_session_v1 *, uint32_t format)
{
    static_cast<session_client *>(data)->shm_format = format;
}

static void sess_dmabuf_device(void *, struct ext_image_copy_capture_session_v1 *, struct wl_array *) {}
static void sess_dmabuf_format(void *, struct ext_image_copy_capture_session_v1 *, uint32_t, struct wl_array *) {}
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

static int create_shm_fd(size_t size)
{
    char name[] = "/tmp/wf-icc-mock-XXXXXX";
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

static void test_shm_frame_ready_path()
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

    EXPECT(g.shm && g.output && g.output_mgr && g.copy_mgr);

    auto *source = ext_output_image_capture_source_manager_v1_create_source(g.output_mgr, g.output);
    EXPECT(source != nullptr);

    session_client sc{};
    auto *session = ext_image_copy_capture_manager_v1_create_session(g.copy_mgr, source, 0);
    EXPECT(session != nullptr);
    ext_image_copy_capture_session_v1_add_listener(session, &session_listener, &sc);

    for (int i = 0; i < 50 && !sc.done; ++i)
        wl_display_dispatch(dpy);
    EXPECT(sc.done);
    EXPECT(sc.width == 64 && sc.height == 64);
    EXPECT(sc.shm_format == WL_SHM_FORMAT_XRGB8888);

    const int stride = static_cast<int>(sc.width) * 4;
    const size_t bytes = static_cast<size_t>(stride) * sc.height;
    int fd = create_shm_fd(bytes);
    EXPECT(fd >= 0);
    auto *pool = wl_shm_create_pool(g.shm, fd, static_cast<int32_t>(bytes));
    close(fd);
    auto *buffer = wl_shm_pool_create_buffer(pool, 0, static_cast<int>(sc.width),
        static_cast<int>(sc.height), stride, WL_SHM_FORMAT_XRGB8888);

    auto *frame = ext_image_copy_capture_session_v1_create_frame(session);
    EXPECT(frame != nullptr);
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, &sc);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0,
        static_cast<int32_t>(sc.width), static_cast<int32_t>(sc.height));
    ext_image_copy_capture_frame_v1_capture(frame);

    for (int i = 0; i < 50 && !sc.ready && !sc.failed; ++i)
        wl_display_dispatch(dpy);

    EXPECT(sc.ready);
    EXPECT(!sc.failed);

    ext_image_copy_capture_frame_v1_destroy(frame);
    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    ext_image_copy_capture_session_v1_destroy(session);
    ext_image_capture_source_v1_destroy(source);
    wl_registry_destroy(reg);
    wl_display_disconnect(dpy);

    mc.stop.store(true);
    thr.join();
    for (auto *gl : mc.globals)
        wl_global_destroy(gl);
    wl_display_destroy(mc.display);

    std::cout << "shm frame ready path: ok\n";
}

int main()
{
    if (!getenv("XDG_RUNTIME_DIR")) {
        std::cerr << "mock-wayland-icc-frame-session-test: SKIP (XDG_RUNTIME_DIR unset)\n";
        return 77;
    }

    test_shm_frame_ready_path();

    if (g_failures) {
        std::cerr << g_failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "mock-wayland-icc-frame-session-test: ok\n";
    return 0;
}
