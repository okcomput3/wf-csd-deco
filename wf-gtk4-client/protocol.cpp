#include "protocol.hpp"
#include "wf-decorator-client-protocol.h"
#include <wayland-client.h>
#include <string.h>
#include <iostream>

wl_display *display;
wf_decorator_manager *decorator_manager;

static void create_new_decoration(void*, wf_decorator_manager*, uint32_t view_id)
{
    std::cout << "create new decoration" << std::endl;
    auto window = create_deco_window(view_id);
    view_to_decor[view_id] = window;
}

static void destroy_decoration(void*, wf_decorator_manager*, uint32_t view_id)
{
    std::cout << "destroy decoration" << std::endl;
    destroy_deco_window(view_id);
}

static void title_changed(void*,
    wf_decorator_manager*, uint32_t view, const char *new_title)
{
    set_title(view_to_decor[view], new_title);
}

static void app_id_changed(void*,
    wf_decorator_manager*, uint32_t view, const char *new_app_id)
{
    set_app_id(view_to_decor[view], new_app_id);
}

static void windows_grouped(void*,
    wf_decorator_manager*, uint32_t parent, uint32_t child)
{
    windows_grouped_notify(parent, child);
}

static void window_ungrouped(void*,
    wf_decorator_manager*, uint32_t id)
{
    window_ungrouped_notify(id);
}

const wf_decorator_manager_listener decorator_listener =
{
    create_new_decoration,
    destroy_decoration,
    title_changed,
    app_id_changed,
    windows_grouped,
    window_ungrouped
};

void registry_add_object(void*, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t)
{
    std::cout << "new registry: " << interface << std::endl;
    if (strcmp(interface, wf_decorator_manager_interface.name) == 0)
    {
        std::cout << "bind it" << std::endl;
        decorator_manager =
            (wf_decorator_manager*)wl_registry_bind(registry, name, &wf_decorator_manager_interface, 1u);

        wf_decorator_manager_add_listener(decorator_manager, &decorator_listener, NULL);
        wl_registry_destroy(registry);
    }
}

void registry_remove_object(void*, struct wl_registry*, uint32_t)
{}

static struct wl_registry_listener registry_listener =
{
    &registry_add_object,
    &registry_remove_object
};

void update_borders(uint32_t id, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right)
{
    wf_decorator_manager_update_borders(decorator_manager, id, top, bottom, left, right);
}

void group_windows(uint32_t parent_id, uint32_t child_id)
{
    wf_decorator_manager_group_windows(decorator_manager, parent_id, child_id);
}

void select_window(uint32_t id)
{
    wf_decorator_manager_select_window(decorator_manager, id);
}

void ungroup_window(uint32_t id)
{
    wf_decorator_manager_ungroup_window(decorator_manager, id);
}

void start_group_drag(uint32_t id)
{
    wf_decorator_manager_start_group_drag(decorator_manager, id);
}

void close_request(uint32_t id)
{
    wf_decorator_manager_close_request(decorator_manager, id);
}

void setup_protocol(GdkDisplay *displ)
{
    if (!displ || !GDK_IS_WAYLAND_DISPLAY(displ))
    {
        g_critical("wf-gtk4-decorator: not running on a Wayland display. "
                   "Ensure WAYLAND_DISPLAY is set (and GDK_BACKEND is not forcing X11) "
                   "in the environment this was launched from.");
        return;
    }

    auto display  = gdk_wayland_display_get_wl_display(displ);
    auto registry = wl_display_get_registry(display);

    wl_registry_add_listener(registry, &registry_listener, NULL);
    /* One roundtrip binds wf_decorator_manager if the compositor plugin has
     * already created it. If it hasn't yet (e.g. on autostart, where this
     * client and the plugin come up together), we intentionally leave the
     * registry and its listener alive: GDK keeps dispatching this display in
     * the GTK main loop, so registry_add_object still fires and binds the
     * global whenever the plugin advertises it. */
    wl_display_roundtrip(display);
}
