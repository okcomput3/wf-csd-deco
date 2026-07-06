#pragma once

#include <gdk/wayland/gdkwayland.h>
#include <gtk/gtk.h>
#include <memory>
#include <vector>
#include <string>
#include <map>

struct window_data
{
    GtkWidget *scrolled_window;
    GtkWidget *header_bar;
    GtkWidget *title_box;
    GtkWidget *tab_box;
    std::string app_id;
    bool ui_ready = false;
    struct group_data
    {
        uint32_t id;
        bool parent;
        std::vector<uint32_t> order;
    } group;
    uint32_t wf_id;
};

void setup_protocol(GdkDisplay *display);

GtkWidget *create_deco_window(uint32_t wf_id);
void destroy_deco_window(uint32_t wf_id);

void set_title(GtkWidget *window, const char *title);
void set_app_id(GtkWidget *window, const char *app_id);
void window_destroyed(GtkWidget *window);

void update_borders(uint32_t id, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right);
void group_windows(uint32_t parent_id, uint32_t child_id);
void select_window(uint32_t id);
void ungroup_window(uint32_t id);
void start_group_drag(uint32_t id);
void close_request(uint32_t id);
void windows_grouped_notify(uint32_t parent_id, uint32_t child_id);
void window_ungrouped_notify(uint32_t id);

inline std::map<uint32_t, GtkWidget*> view_to_decor;
inline std::map<GtkWidget*, std::shared_ptr<window_data>> win_data;