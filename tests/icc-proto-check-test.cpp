/*
 * Layer 5a — unit tests for ICC registry binding / requirement rules.
 */

#include "icc-proto-check.hpp"

#include <iostream>
#include <string>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "        \
                      << #cond << std::endl;                                   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static void test_classify_output_mode()
{
    EXPECT(icc_classify_global("wl_output", false) == icc_global_action::bind_output);
    EXPECT(icc_classify_global("wl_shm", false) == icc_global_action::bind_shm);
    EXPECT(icc_classify_global("ext_image_copy_capture_manager_v1", false) ==
        icc_global_action::bind_copy_capture_manager);
    EXPECT(icc_classify_global("ext_output_image_capture_source_manager_v1", false) ==
        icc_global_action::bind_output_image_capture);
    /* Toplevel-only globals ignored in output mode. */
    EXPECT(icc_classify_global("ext_foreign_toplevel_list_v1", false) ==
        icc_global_action::ignore);
    EXPECT(icc_classify_global("ext_foreign_toplevel_image_capture_source_manager_v1", false) ==
        icc_global_action::ignore);
    EXPECT(icc_classify_global("wl_compositor", false) == icc_global_action::ignore);
}

static void test_classify_toplevel_mode()
{
    EXPECT(icc_classify_global("ext_foreign_toplevel_list_v1", true) ==
        icc_global_action::bind_foreign_toplevel_list);
    EXPECT(icc_classify_global("ext_foreign_toplevel_image_capture_source_manager_v1", true) ==
        icc_global_action::bind_toplevel_image_capture);
    /* Output capture manager ignored when capturing a toplevel. */
    EXPECT(icc_classify_global("ext_output_image_capture_source_manager_v1", true) ==
        icc_global_action::ignore);
    EXPECT(icc_classify_global("ext_image_copy_capture_manager_v1", true) ==
        icc_global_action::bind_copy_capture_manager);
}

static icc_registry_flags full_output_flags()
{
    icc_registry_flags f;
    icc_note_global(f, "wl_shm", false);
    icc_note_global(f, "wl_output", false);
    icc_note_global(f, "ext_image_copy_capture_manager_v1", false);
    icc_note_global(f, "ext_output_image_capture_source_manager_v1", false);
    icc_note_global(f, "zxdg_output_manager_v1", false);
    icc_note_global(f, "zwp_linux_dmabuf_v1", false);
    return f;
}

static void test_output_mode_complete()
{
    auto f = full_output_flags();
    EXPECT(f.output_count == 1);
    EXPECT(icc_missing_requirement(f, false, true) == nullptr);
    EXPECT(icc_missing_requirement(f, false, false) == nullptr);
}

static void test_output_mode_missing_copy_capture()
{
    auto f = full_output_flags();
    f.has_copy_capture_manager = false;
    const char *m = icc_missing_requirement(f, false, false);
    EXPECT(m != nullptr);
    EXPECT(std::string(m).find("ext-image-copy-capture-manager-v1") != std::string::npos);
}

static void test_output_mode_missing_output_capture()
{
    auto f = full_output_flags();
    f.has_output_image_capture = false;
    const char *m = icc_missing_requirement(f, false, false);
    EXPECT(m != nullptr);
    EXPECT(std::string(m).find("ext-output-image-capture-source-manager-v1") != std::string::npos);
}

static void test_output_mode_no_outputs()
{
    auto f = full_output_flags();
    f.output_count = 0;
    const char *m = icc_missing_requirement(f, false, false);
    EXPECT(m != nullptr);
    EXPECT(std::string(m).find("no outputs") != std::string::npos);
}

static void test_dmabuf_required()
{
    auto f = full_output_flags();
    f.has_dmabuf = false;
    EXPECT(icc_missing_requirement(f, false, false) == nullptr);
    const char *m = icc_missing_requirement(f, false, true);
    EXPECT(m != nullptr);
    EXPECT(std::string(m).find("linux-dmabuf") != std::string::npos);
}

static void test_toplevel_mode_requirements()
{
    icc_registry_flags f;
    icc_note_global(f, "wl_shm", true);
    icc_note_global(f, "wl_output", true);
    icc_note_global(f, "ext_image_copy_capture_manager_v1", true);
    icc_note_global(f, "zxdg_output_manager_v1", true);
    /* Missing toplevel pieces. */
    EXPECT(icc_missing_requirement(f, true, false) != nullptr);

    icc_note_global(f, "ext_foreign_toplevel_list_v1", true);
    EXPECT(icc_missing_requirement(f, true, false) != nullptr);

    icc_note_global(f, "ext_foreign_toplevel_image_capture_source_manager_v1", true);
    EXPECT(icc_missing_requirement(f, true, false) == nullptr);

    /* Output capture manager must not be required in toplevel mode. */
    EXPECT(f.has_output_image_capture == false);
}

static void test_multi_output_count()
{
    icc_registry_flags f;
    icc_note_global(f, "wl_output", false);
    icc_note_global(f, "wl_output", false);
    EXPECT(f.output_count == 2);
}

int main()
{
    test_classify_output_mode();
    test_classify_toplevel_mode();
    test_output_mode_complete();
    test_output_mode_missing_copy_capture();
    test_output_mode_missing_output_capture();
    test_output_mode_no_outputs();
    test_dmabuf_required();
    test_toplevel_mode_requirements();
    test_multi_output_count();

    if (g_failures) {
        std::cerr << g_failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "icc-proto-check-test: ok\n";
    return 0;
}
