#pragma once

/*
 * Shared ICC / ext-image-copy-capture registry requirements.
 *
 * Mirrors check_has_protos() / handle_global() binding rules in main.cpp so
 * unit and mock-Wayland tests can lock the compositor contract without a
 * full Hyprland session.
 */

#include <cstring>
#include <string>

struct icc_registry_flags
{
    bool has_shm = false;
    bool has_copy_capture_manager = false;
    bool has_output_image_capture = false;
    bool has_foreign_toplevel_list = false;
    bool has_toplevel_image_capture = false;
    bool has_xdg_output = false;
    bool has_dmabuf = false;
    int output_count = 0;
};

/** Which globals an ICC client binds, matching main.cpp handle_global(). */
enum class icc_global_action
{
    ignore,
    bind_output,
    bind_shm,
    bind_foreign_toplevel_list,          /* only if capture_toplevel */
    bind_toplevel_image_capture,         /* only if capture_toplevel */
    bind_output_image_capture,           /* only if !capture_toplevel */
    bind_copy_capture_manager,
    bind_xdg_output,
    bind_dmabuf,
};

inline icc_global_action icc_classify_global(const char *interface, bool capture_toplevel)
{
    if (!interface)
        return icc_global_action::ignore;
    if (std::strcmp(interface, "wl_output") == 0)
        return icc_global_action::bind_output;
    if (std::strcmp(interface, "wl_shm") == 0)
        return icc_global_action::bind_shm;
    if (std::strcmp(interface, "ext_foreign_toplevel_list_v1") == 0)
        return capture_toplevel ? icc_global_action::bind_foreign_toplevel_list
                                : icc_global_action::ignore;
    if (std::strcmp(interface, "ext_foreign_toplevel_image_capture_source_manager_v1") == 0)
        return capture_toplevel ? icc_global_action::bind_toplevel_image_capture
                                : icc_global_action::ignore;
    if (std::strcmp(interface, "ext_output_image_capture_source_manager_v1") == 0)
        return !capture_toplevel ? icc_global_action::bind_output_image_capture
                                 : icc_global_action::ignore;
    if (std::strcmp(interface, "ext_image_copy_capture_manager_v1") == 0)
        return icc_global_action::bind_copy_capture_manager;
    if (std::strcmp(interface, "zxdg_output_manager_v1") == 0)
        return icc_global_action::bind_xdg_output;
    if (std::strcmp(interface, "zwp_linux_dmabuf_v1") == 0)
        return icc_global_action::bind_dmabuf;
    return icc_global_action::ignore;
}

inline void icc_note_global(icc_registry_flags &flags, const char *interface,
    bool capture_toplevel)
{
    switch (icc_classify_global(interface, capture_toplevel)) {
    case icc_global_action::bind_output:
        ++flags.output_count;
        break;
    case icc_global_action::bind_shm:
        flags.has_shm = true;
        break;
    case icc_global_action::bind_foreign_toplevel_list:
        flags.has_foreign_toplevel_list = true;
        break;
    case icc_global_action::bind_toplevel_image_capture:
        flags.has_toplevel_image_capture = true;
        break;
    case icc_global_action::bind_output_image_capture:
        flags.has_output_image_capture = true;
        break;
    case icc_global_action::bind_copy_capture_manager:
        flags.has_copy_capture_manager = true;
        break;
    case icc_global_action::bind_xdg_output:
        flags.has_xdg_output = true;
        break;
    case icc_global_action::bind_dmabuf:
        flags.has_dmabuf = true;
        break;
    case icc_global_action::ignore:
        break;
    }
}

/**
 * Return a human-readable missing requirement, or nullptr if the registry
 * satisfies an ICC capture session (same messages as check_has_protos()).
 */
inline const char *icc_missing_requirement(const icc_registry_flags &flags,
    bool capture_toplevel, bool use_dmabuf)
{
    if (!flags.has_shm)
        return "compositor is missing wl_shm";
    if (!capture_toplevel && !flags.has_output_image_capture)
        return "compositor doesn't support ext-output-image-capture-source-manager-v1";
    if (capture_toplevel && !flags.has_foreign_toplevel_list)
        return "compositor doesn't support ext-foreign-toplevel-list-v1";
    if (capture_toplevel && !flags.has_toplevel_image_capture)
        return "compositor doesn't support ext-foreign-toplevel-image-capture-source-manager-v1";
    if (!flags.has_copy_capture_manager)
        return "compositor doesn't support ext-image-copy-capture-manager-v1";
    if (!flags.has_xdg_output)
        return "compositor doesn't support xdg-output-unstable-v1";
    if (use_dmabuf && !flags.has_dmabuf)
        return "compositor doesn't support linux-dmabuf-unstable-v1";
    if (flags.output_count <= 0)
        return "no outputs available";
    return nullptr;
}
