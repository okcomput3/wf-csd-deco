#include <algorithm>
#include "protocol.hpp"

GtkApplication *app;

struct custom_data
{
    uint32_t id;
    GtkWidget *area;
    gulong size_allocate_signal;
};

static void activate(GtkApplication *app, gpointer)
{
    GdkDisplay *display = gdk_display_get_default();
    setup_protocol(display);

    g_application_hold(G_APPLICATION(app));
}

static void on_button_released(GtkGestureClick *gesture,
    int n_press,
    double x,
    double y,
    gpointer user_data);

static gboolean on_close_request(GtkWindow *window, gpointer data)
{
    GtkWidget *win = (GtkWidget*)data;
    auto it = std::find_if(view_to_decor.begin(), view_to_decor.end(),
        [&win] (const std::pair<uint32_t, GtkWidget*>& element)
    {
        return element.second == win;
    });

    on_button_released(NULL, 0, 0, 0, win_data[(GtkWidget*)window].get());

    if (it != view_to_decor.end())
    {
        uint32_t id = it->first;
        view_to_decor.erase(id);
    }

    win_data.erase(win);

    return false;
}

/* User clicked the decoration's titlebar close button. Ask the compositor to
 * close the underlying view; it then tears the decoration down via the
 * destroy_decoration event (which runs the cleanup above). Return TRUE so GTK
 * doesn't also destroy our decoration window out from under that flow. */
static gboolean on_deco_close_request(GtkWindow*, gpointer data)
{
    auto wdata = (window_data*)data;
    close_request(wdata->wf_id);
    return TRUE;
}

static void on_area_resized(GtkDrawingArea*, int w, int h, gpointer data)
{
    printf("size_allocate\n");
    auto cdata = (custom_data*)data;
    auto id    = cdata->id;
    auto area  = cdata->area;
    GtkNative *native = gtk_widget_get_native(area);

    double surface_x, surface_y;
    gtk_native_get_surface_transform(native, &surface_x, &surface_y);

    graphene_rect_t bounds;
    gtk_widget_compute_bounds(area, GTK_WIDGET(native), &bounds);

    double final_x = surface_x + bounds.origin.x;
    double final_y = surface_y + bounds.origin.y;
    double width   = bounds.size.width;
    double height  = bounds.size.height;

    if (final_y > 0)
    {
        update_borders(id, final_y, final_x, final_x, final_x);
    }
}

// --- Compositor-side drag-to-group (dispatched at the window level) ---
// GTK's header-bar window-move is triggered high in the widget tree, before a
// gesture on a tab button can resolve, so a per-button gesture loses the race
// and the whole window slides under the cursor while the group-drag also runs.
// Instead, a single GtkGestureDrag on the *window* in the CAPTURE phase (which
// runs before the header) inspects the widget under the press: if it's one we
// marked draggable (a tab or the app icon), we claim the sequence up front —
// which blocks the window-move — and drive the group-drag; otherwise we stay
// out of the way so the titlebar still moves the window normally.

/* Tag a widget as a drag handle for a group member and remember whether a
 * no-move tap on it should select (morph to) that member. No per-widget gesture
 * is attached; the window-level gesture below reads these tags. */
static void mark_group_draggable(GtkWidget *widget, window_data *wdata, gboolean tap_selects)
{
    g_object_set_data(G_OBJECT(widget), "wf-draggable", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(widget), "wf-member-id", GUINT_TO_POINTER(wdata->wf_id));
    g_object_set_data(G_OBJECT(widget), "wf-tap-selects", GINT_TO_POINTER(tap_selects ? 1 : 0));
    gtk_widget_set_can_target(widget, TRUE);
}

static void win_drag_begin(GtkGestureDrag *gesture,
    double x,
    double y,
    gpointer user_data)
{
    GtkWidget *window = (GtkWidget*)user_data;

    g_object_set_data(G_OBJECT(gesture), "wf-armed", GINT_TO_POINTER(0));
    g_object_set_data(G_OBJECT(gesture), "wf-drag-sent", GINT_TO_POINTER(0));

    /* Which widget is under the press? Walk up to a marked drag handle. */
    GtkWidget *picked = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);
    guint id = 0;
    int tap_sel = 0;
    gboolean found = FALSE;
    for (GtkWidget *w = picked; w != NULL; w = gtk_widget_get_parent(w))
    {
        if (g_object_get_data(G_OBJECT(w), "wf-draggable"))
        {
            id      = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "wf-member-id"));
            tap_sel = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "wf-tap-selects"));
            found   = TRUE;
            break;
        }
    }

    if (!found)
    {
        /* Empty titlebar or window content: let GTK move the window as usual. */
        return;
    }

    g_object_set_data(G_OBJECT(gesture), "wf-armed", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(gesture), "wf-id", GUINT_TO_POINTER(id));
    g_object_set_data(G_OBJECT(gesture), "wf-tap-selects", GINT_TO_POINTER(tap_sel));
    /* Own the sequence before the header's move gesture can: this is what stops
     * the whole window from being dragged. */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void win_drag_update(GtkGestureDrag *gesture,
    double dx,
    double dy,
    gpointer user_data)
{
    if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gesture), "wf-armed")))
    {
        return;
    }

    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gesture), "wf-drag-sent")))
    {
        return;
    }

    if ((dx * dx + dy * dy) < 8.0 * 8.0)
    {
        return;
    }

    guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(gesture), "wf-id"));
    g_object_set_data(G_OBJECT(gesture), "wf-drag-sent", GINT_TO_POINTER(1));
    start_group_drag(id);
}

static void win_drag_end(GtkGestureDrag *gesture,
    double dx,
    double dy,
    gpointer user_data)
{
    if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gesture), "wf-armed")))
    {
        return;
    }

    int was_drag = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gesture), "wf-drag-sent"));
    guint id     = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(gesture), "wf-id"));
    int tap_sel  = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gesture), "wf-tap-selects"));

    if (was_drag)
    {
        /* Real drag already ran; the compositor handles the release. */
        return;
    }

    if (tap_sel)
    {
        /* No-move tap on a tab -> select that member (drives the morph). */
        select_window(id);
    }
}

/* One capture-phase drag gesture per decoration window, dispatching to whichever
 * tab/icon the press started on. */
static void attach_window_group_drag(GtkWidget *window)
{
    GtkGesture *drag = gtk_gesture_drag_new();
    /* Left button only, so the middle-click ungroup gesture is unaffected. */
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(win_drag_begin), window);
    g_signal_connect(drag, "drag-update", G_CALLBACK(win_drag_update), window);
    g_signal_connect(drag, "drag-end", G_CALLBACK(win_drag_end), window);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
    gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(drag));
}

static void add_tab_button(window_data *wdata, window_data *cdata);
static void clear_group_tabs(uint32_t group_id);
static void reparent_group(uint32_t group_id, window_data *last_parent);
static void refresh_group(uint32_t group_id);
static void clear_box(GtkWidget *box);
static std::shared_ptr<window_data> lookup_wdata(uint32_t id);

/* Rebuild the tab strips for a group (and give a just-ungrouped window its
 * solo icon) on the next main-loop iteration. Doing this synchronously from
 * inside a gesture "released" handler destroys the very widgets GTK is still
 * dispatching on, which corrupts input state and makes the icons go dead. */
struct ungroup_rebuild_ctx
{
    uint32_t group_id;
    uint32_t solo_id;
};

static gboolean ungroup_rebuild_idle(gpointer data)
{
    auto *ctx = (ungroup_rebuild_ctx*)data;

    clear_group_tabs(ctx->group_id);

    auto solo = lookup_wdata(ctx->solo_id);
    if (solo)
    {
        clear_box(solo->tab_box);
        add_tab_button(solo.get(), solo.get());
    }

    refresh_group(ctx->group_id);

    delete ctx;
    return G_SOURCE_REMOVE;
}

static gboolean group_rebuild_idle(gpointer data)
{
    uint32_t group_id = GPOINTER_TO_UINT(data);
    clear_group_tabs(group_id);
    refresh_group(group_id);
    return G_SOURCE_REMOVE;
}

static void on_button_released(GtkGestureClick *gesture,
    int n_press,
    double x,
    double y,
    gpointer user_data)
{
    auto wdata    = (window_data*)user_data;
    auto group_id = wdata->group.id;

    if (!group_id)
    {
        return;
    }

    /* Update the data model now, but defer widget destruction/creation to an
     * idle callback so we are not tearing down tab buttons while GTK is still
     * inside this gesture's dispatch. */
    for (auto cdata : win_data)
    {
        if (group_id == cdata.second->group.id)
        {
            if (cdata.second->group.parent)
            {
                cdata.second->group.order.erase(std::remove(cdata.second->group.order.begin(),
                    cdata.second->group.order.end(), wdata->wf_id), cdata.second->group.order.end());
                if (wdata->wf_id == cdata.second->wf_id)
                {
                    reparent_group(group_id, wdata);
                }

                break;
            }
        }
    }

    wdata->group.id = 0;

    ungroup_window(wdata->wf_id);

    auto *ctx = new ungroup_rebuild_ctx{group_id, wdata->wf_id};
    g_idle_add(ungroup_rebuild_idle, ctx);
}

static void add_tab_button(window_data *wdata, window_data *cdata)
{
    GtkWidget *button = gtk_button_new_from_icon_name(cdata->app_id.c_str());

    /* Every tab is draggable. Dragging a tab drags the member it represents
     * (cdata): onto another window to (re)group it, or onto empty space to
     * pull it back out of the group. For a solo window's own button
     * cdata == wdata, so this matches the previous behaviour. A no-move tap
     * selects that member (tap_selects = TRUE), driving the compositor morph. */
    mark_group_draggable(button, cdata, TRUE);

    GtkGesture *click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), 2);
    g_signal_connect(click_gesture, "released", G_CALLBACK(on_button_released), cdata);
    gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(click_gesture));

    gtk_box_append(GTK_BOX(wdata->tab_box), button);
}

/* Safe lookup: window_data for a wf id, or nullptr if the id is not
 * currently tracked. Avoids std::map::operator[] silently default-
 * inserting a null entry (and the crash that follows when it is
 * dereferenced) for stale ids left in a group's order list. */
static std::shared_ptr<window_data> lookup_wdata(uint32_t id)
{
    auto dit = view_to_decor.find(id);
    if (dit == view_to_decor.end())
    {
        return nullptr;
    }

    auto wit = win_data.find(dit->second);
    if (wit == win_data.end())
    {
        return nullptr;
    }

    return wit->second;
}

int get_box_children_count(GtkWidget *box)
{
    int count = 0;
    GtkWidget *child = gtk_widget_get_first_child(box);

    while (child)
    {
        count++;
        child = gtk_widget_get_next_sibling(child);
    }

    return count;
}

static void clear_box(GtkWidget *box)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL)
    {
        gtk_box_remove(GTK_BOX(box), child);
    }
}

static void clear_group_tabs(uint32_t group_id)
{
    std::vector<uint32_t> button_order;

    if (!group_id)
    {
        return;
    }

    for (auto wdata : win_data)
    {
        if ((wdata.second->group.id == group_id) && wdata.second->group.parent)
        {
            button_order = wdata.second->group.order;
            break;
        }
    }

    for (auto id : button_order)
    {
        auto wdata = lookup_wdata(id);
        if (wdata && (wdata->group.id == group_id))
        {
            clear_box(wdata->tab_box);
        }
    }
}

static void reparent_group(uint32_t group_id, window_data *last_parent)
{
    if (!group_id)
    {
        return;
    }

    for (auto wdata : win_data)
    {
        if ((wdata.second->group.id == group_id) && !wdata.second->group.parent)
        {
            wdata.second->group.order  = last_parent->group.order;
            wdata.second->group.parent = true;
            last_parent->group.parent  = false;
            last_parent->group.order.clear();
            last_parent->group.id = 0;
            break;
        }
    }
}


static void refresh_group(uint32_t group_id)
{
    std::vector<uint32_t> button_order;

    if (!group_id)
    {
        return;
    }

    for (auto wdata : win_data)
    {
        if ((wdata.second->group.id == group_id) && wdata.second->group.parent)
        {
            button_order = wdata.second->group.order;
            break;
        }
    }

    for (auto wdata : win_data)
    {
        for (auto id : button_order)
        {
            auto cdata = win_data[view_to_decor[id]];
            if ((cdata->group.id == group_id) && (wdata.second->group.id == group_id))
            {
                add_tab_button(wdata.second.get(), cdata.get());
            }
        }
    }

    /* Collapse a group that has dropped to a single member back to a solo
     * window. Scope this to THIS group only: refresh_group runs from a deferred
     * idle, and when a member is dragged from one group into another the old
     * group's rebuild fires before the new group's strips are built. Scanning
     * all windows here would see the new group's parent with just its one icon
     * and wrongly dissolve it (clearing its group id/order), so the second
     * rebuild then finds no parent and never adds the moved icon. */
    for (auto cdata : win_data)
    {
        if ((cdata.second->group.id == group_id) &&
            (get_box_children_count(cdata.second->tab_box) == 1))
        {
            clear_box(cdata.second->tab_box);
            cdata.second->group.id     = 0;
            cdata.second->group.parent = false;
            cdata.second->group.order.clear();
            add_tab_button(cdata.second.get(), cdata.second.get());
        }
    }
}

/* Client-side bookkeeping after the compositor grouped two windows:
 * update group ids/order and rebuild the tab strips. Called from the
 * windows_grouped protocol event. */
void windows_grouped_notify(uint32_t parent_id, uint32_t child_id)
{
    uint32_t group_id = 1;

    g_print("windows_grouped: parent %u child %u\n", parent_id, child_id);

    auto parent_it = view_to_decor.find(parent_id);
    auto child_it  = view_to_decor.find(child_id);
    if ((parent_it == view_to_decor.end()) || (child_it == view_to_decor.end()))
    {
        return;
    }

    auto drop_target_data = win_data[parent_it->second];
    auto drag_source_data = win_data[child_it->second];
    if (!drop_target_data || !drag_source_data)
    {
        return;
    }

    if (drag_source_data->group.id)
    {
        g_print("Drag source already grouped, ignoring\n");
        return;
    }

    if (drop_target_data->group.id)
    {
        group_id = drop_target_data->group.id;
    } else
    {
        for (auto wdata : win_data)
        {
            if (wdata.second->group.id >= group_id)
            {
                group_id = wdata.second->group.id + 1;
            }
        }

        drop_target_data->group.parent = true;
        drop_target_data->group.id     = group_id;
        drop_target_data->group.order.push_back(drop_target_data->wf_id);
    }

    drag_source_data->group.id = group_id;

    for (auto wdata : win_data)
    {
        if ((wdata.second->group.id == group_id) && wdata.second->group.parent)
        {
            wdata.second->group.order.push_back(drag_source_data->wf_id);
            break;
        }
    }

    g_idle_add(group_rebuild_idle, GUINT_TO_POINTER(group_id));
}

/* Client-side bookkeeping after the compositor ungrouped a window on its own
 * (drag-to-ungroup, or leaving one group while being dragged into another).
 * Same model update as the middle-click path in on_button_released, but the
 * compositor already performed the ungroup so we do not send it back. */
void window_ungrouped_notify(uint32_t id)
{
    auto wdata = lookup_wdata(id);
    if (!wdata)
    {
        return;
    }

    auto group_id = wdata->group.id;
    if (!group_id)
    {
        return;
    }

    g_print("window_ungrouped: id %u (group %u)\n", id, group_id);

    for (auto cdata : win_data)
    {
        if (group_id == cdata.second->group.id)
        {
            if (cdata.second->group.parent)
            {
                cdata.second->group.order.erase(std::remove(cdata.second->group.order.begin(),
                    cdata.second->group.order.end(), id), cdata.second->group.order.end());
                if (id == cdata.second->wf_id)
                {
                    reparent_group(group_id, wdata.get());
                }

                break;
            }
        }
    }

    wdata->group.id = 0;

    auto *ctx = new ungroup_rebuild_ctx{group_id, id};
    g_idle_add(ungroup_rebuild_idle, ctx);
}

static gboolean on_scroll_cb(GtkEventControllerScroll *controller,
    gdouble dx,
    gdouble dy,
    gpointer user_data)
{
    auto wdata = (window_data*)user_data;

    GtkAdjustment *h_adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(wdata->scrolled_window));

    gdouble current_value = gtk_adjustment_get_value(h_adj);
    gdouble new_value     = current_value + (dy * 10.0);

    gdouble lower = gtk_adjustment_get_lower(h_adj);
    gdouble upper = gtk_adjustment_get_upper(h_adj);
    new_value = CLAMP(new_value, lower, upper);

    auto group_id = wdata->group.id;

    if (group_id)
    {
        for (auto cdata : win_data)
        {
            if (cdata.second->group.id == group_id)
            {
                h_adj =
                    gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(cdata.second->scrolled_window));
                gtk_adjustment_set_value(h_adj, new_value);
            }
        }
    }

    return true;
}

GtkWidget *create_deco_window(uint32_t wf_id)
{
    auto window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(window), 300, 300);
    auto area = gtk_drawing_area_new();
    gtk_window_set_child(GTK_WINDOW(window), area);
    gtk_window_set_title(GTK_WINDOW(window), ("__wf_decorator:" + std::to_string(wf_id)).c_str());
    auto cdata = (custom_data*)malloc(sizeof(custom_data));
    cdata->id   = wf_id;
    cdata->area = area;

    auto wdata = std::make_shared<window_data>();

    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    GtkWidget *tab_box   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(title_box, true);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_EXTERNAL,
        GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), tab_box);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scrolled_window), 115);
    /* Stop the scroller from claiming horizontal drags for kinetic/touch
     * panning. Once a group's tab strip overflows its width the scroller
     * becomes scrollable and its internal pan gesture starts winning pointer
     * drags over each tab's drag-to-(un)group gesture -- which is why a solo
     * window (strip fits, not scrollable) drags out fine, but a multi-tab
     * strip only ever registered the click and morphed instead of dragging.
     * Wheel scrolling is handled separately in on_scroll_cb, so tab scrolling
     * is unaffected. */
    gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(scrolled_window), FALSE);
    gtk_box_prepend(GTK_BOX(title_box), scrolled_window);

    GtkEventController *controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(controller, "scroll", G_CALLBACK(on_scroll_cb), wdata.get());
    gtk_widget_add_controller(tab_box, controller);

    wdata->scrolled_window = scrolled_window;
    wdata->title_box  = title_box;
    wdata->header_bar = header;
    wdata->tab_box    = tab_box;
    wdata->wf_id     = wf_id;
    win_data[window] = wdata;

    cdata->size_allocate_signal = g_signal_connect(area, "resize", G_CALLBACK(on_area_resized), cdata);

    /* Single capture-phase drag gesture for the whole window; it decides per
     * press whether a tab/icon (group-drag) or empty titlebar (window-move)
     * was grabbed. Must be on the window so it runs before the header move. */
    attach_window_group_drag(window);

    /* Titlebar close button -> ask the compositor to close the real view. */
    g_signal_connect(window, "close-request", G_CALLBACK(on_deco_close_request), wdata.get());

    gtk_window_present(GTK_WINDOW(window));

    return window;
}

void destroy_deco_window(uint32_t wf_id)
{
    auto window = view_to_decor[wf_id];
    if (window)
    {
        on_close_request(GTK_WINDOW(window), win_data[window].get());
    }
}

void set_title(GtkWidget *window, const char *title)
{
    gtk_window_set_title(GTK_WINDOW(window), title);
}

void set_app_id(GtkWidget *window, const char *app_id)
{
    auto wdata = win_data[window];
    if (!wdata)
    {
        return;
    }

    wdata->app_id = app_id;

    /* Only build the header UI once. app_id_changed can fire more than once
     * per window; rebuilding here would stack duplicate draggable icons and
     * duplicate tab buttons, whose extra drag/click gestures then fight each
     * other and the icons stop responding. */
    if (wdata->ui_ready)
    {
        return;
    }

    wdata->ui_ready = true;

    GtkWidget *image = gtk_image_new_from_icon_name(app_id);
    gtk_widget_set_can_target(image, TRUE);
    /* The far-left headerbar app icon is draggable too — this is the icon
     * people naturally try to grab. It represents the window itself, so a tap
     * on it should do nothing (tap_selects = FALSE); only dragging it acts. */
    mark_group_draggable(image, wdata.get(), FALSE);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(wdata->header_bar), image);

    add_tab_button(wdata.get(), wdata.get());

    gtk_header_bar_pack_start(GTK_HEADER_BAR(wdata->header_bar), wdata->title_box);
}

int main(int argc, char **argv)
{
    int status;

    /* This decorator speaks a Wayland-only protocol (wf_decorator_manager), so
     * it must use GDK's Wayland backend. On autostart the environment often has
     * DISPLAY (XWayland) set, which can make GTK open the X11 backend instead;
     * it then connects to a display where our protocol does not exist and no
     * window is ever decorated. Manual launches work only because they set
     * WAYLAND_DISPLAY. Pin the backend so the launch context can't change it.
     * Must be called before the display is opened (before g_application_run). */
    gdk_set_allowed_backends("wayland");

    app = gtk_application_new("org.wf.sample-decorator", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}