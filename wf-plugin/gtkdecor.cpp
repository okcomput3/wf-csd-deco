/* This plugin is the interface between the decorator client and Wayfire. It has several functions:
 *
 * - When a new view is mapped, it notifies the decorator client via a custom protocol that a new decoration
 *   is required.
 * - When a new decoration toplevel is created, we attach it to the main view with several nodes:
 *   First, we attach the translation node, which has a child mask node, whose child is the decoration
 * surface.
 *   The translation node is responsible for setting the position of the decoration relative to the main view.
 *   The mask node cuts out the middle of the decoration so that transparent views remain transparent.
 *   The main decoration surface contains the actual decorations.
 *
 * - On each transaction involving a decorated view, the plugin adds a decoration object associated with the
 *   view to the transaction. The transaction object resizes the decoration on commit, and is ready when the
 *   decoration surface also resizes to the new size. Special care should be taken for the cases where the
 *   main view does not obey the compositor-requested size: in those cases, the decoration needs to be resized
 *   again to the final size of the main view.
 *
 *   Copyright © 2023 Ilia Bozhinov <ammen99@gmail.com>
 *   Copyright © 2026 Scott Moreau <oreaus@gmail.com>
 *
 */
#include <memory>
#include <map>
#include <wayfire/core.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/object.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/debug.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/window-manager.hpp>

#include <wayfire/toplevel.hpp>
#include <wayfire/txn/transaction-object.hpp>
#include <wayfire/txn/transaction-manager.hpp>

#include <type_traits>
#include <wayfire/util.hpp>
#include <wayfire/view.hpp>
#include <wayfire/plugins/common/util.hpp>

#include <wayfire/signal-definitions.hpp>
#include "wf-decorator-protocol.h"

#include <wayfire/unstable/wlr-surface-node.hpp>
#include <wayfire/unstable/wlr-view-events.hpp>
#include <wayfire/unstable/translation-node.hpp>

#include <algorithm>
#include <cmath>
#include <wayfire/view-transform.hpp>
#include <wayfire/output.hpp>
#include <wayfire/workarea.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/view-helpers.hpp>
#include <linux/input-event-codes.h>

void create_xdg_popup(wlr_xdg_popup *popup);

wf::decoration_margins_t deco_margins =
{
    .left   = 0,
    .right  = 0,
    .bottom = 0,
    .top    = 0,
};

using decoration_node_t = std::shared_ptr<wf::scene::wlr_surface_node_t>;

std::ostream& operator <<(std::ostream& out, const wf::dimensions_t& dims)
{
    out << dims.width << "x" << dims.height;
    return out;
}

/* ─── Wayfire scene-region ABI selector ──────────────────────────────────
 * Wayfire changed the type of scene-node damage/visibility regions across
 * the "geometry goes floating-point" refactor:
 *     recent Wayfire (git master / the 2026 builds)  ->  wf::regionf_t (double)
 *     older Wayfire  (roughly 2025 and earlier)      ->  wf::region_t  (int)
 *
 * This MUST match the Wayfire headers you build against — and those must in
 * turn match the Wayfire you run, because the compositor refuses to load a
 * plugin whose baked-in WAYFIRE_API_ABI_VERSION differs from its own.
 *
 * Default is 1 (regionf_t), for current / bleeding-edge Wayfire. If the build
 * fails with a regionf_t/region_t type error on `allowed`,
 * `schedule_instructions`, or `compute_visibility`, flip this to 0 (and make
 * sure you are building against the same Wayfire you actually run).
 *
 * Pick the right value for a given Wayfire prefix with:
 *     grep -q regionf_t <prefix>/include/wayfire/region.hpp && echo 1 || echo 0
 * ──────────────────────────────────────────────────────────────────────── */
#ifndef DECO_USE_REGIONF
#define DECO_USE_REGIONF 1
#endif

#if DECO_USE_REGIONF
using deco_region_t = wf::regionf_t;
#else
using deco_region_t = wf::region_t;
#endif

/**
 * A node which cuts out a part of its children (visually).
 */
class gtk4_mask_node_t : public wf::scene::floating_inner_node_t
{
  public:
    // The rendered part of the decoration which does not include the client buffer area
    deco_region_t allowed;

    gtk4_mask_node_t() : floating_inner_node_t(false)
    {}

    std::optional<wf::scene::input_node_t> find_node_at(const wf::pointf_t& at) override
    {
        if (allowed.contains_pointf(at))
        {
            return wf::scene::floating_inner_node_t::find_node_at(at);
        }

        return {};
    }

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *output) override
    {
        instances.push_back(std::make_unique<gtk4_mask_render_instance_t>(this, push_damage, output));
    }

    class gtk4_mask_render_instance_t : public wf::scene::render_instance_t
    {
        std::vector<wf::scene::render_instance_uptr> children;
        gtk4_mask_node_t *self;

      public:
        gtk4_mask_render_instance_t(gtk4_mask_node_t *self, wf::scene::damage_callback damage_cb,
            wf::output_t *output)
        {
            this->self = self;

            for (auto& ch : self->get_children())
            {
                if (ch->is_enabled())
                {
                    ch->gen_render_instances(children, damage_cb, output);
                }
            }
        }

        void schedule_instructions(std::vector<wf::scene::render_instruction_t>& instructions,
            const wf::render_target_t& target, deco_region_t& damage) override
        {
            auto child_damage = (damage & self->allowed);
            for (auto& ch : children)
            {
                ch->schedule_instructions(instructions, target, child_damage);
            }
        }

        void presentation_feedback(wf::output_t *output) override
        {
            for (auto& ch : children)
            {
                ch->presentation_feedback(output);
            }
        }

        void compute_visibility(wf::output_t *output, deco_region_t& visible) override
        {
            for (auto& ch : children)
            {
                ch->compute_visibility(output, visible);
            }
        }
    };
};

static const std::string gtk_decorator_prefix = "__wf_decorator:";
wl_resource *decorator_resource = NULL;
wl_listener deco_client_destroy_listener;
std::vector<std::shared_ptr<wf::scene::wlr_surface_node_t>> deco_nodes;
void ungroup_window(wl_client*, struct wl_resource*, uint32_t id, bool closing);

class gtk4_decoration_object_t : public wf::txn::transaction_object_t
{
    enum class gtk4_decoration_tx_state
    {
        // No transactions in flight
        STABLE,
        // Transaction has just started
        START,
        // The decoration client has ACKed our initial size request. However, the decorated toplevel's client
        // has not ACKed the request yet, so we do not know the actual 'final' size of the client.
        TENTATIVE,
        // Decorated toplevel has set its final size, waiting for the decoration to respond.
        WAITING_FINAL,
    };

  public:
    std::string stringify() const
    {
        std::ostringstream out;
        out << "gtk4deco(" << this << ")";
        return out.str();
    }

    void set_pending_size(wf::dimensions_t desired)
    {
        if (!toplevel)
        {
            return;
        }

        this->pending = desired;
    }

    void set_final_size(wf::dimensions_t final)
    {
        if (!toplevel)
        {
            return;
        }

        LOGD("Final size is ", final, " state is ", (int)deco_state);

        if (this->committed == final)
        {
            switch (this->deco_state)
            {
              case gtk4_decoration_tx_state::STABLE:
                break;

              case gtk4_decoration_tx_state::START:
                this->deco_state = gtk4_decoration_tx_state::WAITING_FINAL;
                if (!root_node->is_enabled())
                {
                    wf::scene::set_node_enabled(root_node, true);
                    wf::scene::set_node_enabled(target_view->get_root_node(), true);
                    wf::scene::set_node_enabled(target_view->get_root_node(), true);
                    wf::scene::update(target_view->get_root_node(), wf::scene::update_flag::REFOCUS);
                }

                break;

              case gtk4_decoration_tx_state::WAITING_FINAL:
                break;

              case gtk4_decoration_tx_state::TENTATIVE:
                this->deco_state = gtk4_decoration_tx_state::STABLE;
                wf::txn::emit_object_ready(this);
                break;
            }
        }

        this->committed = final;
        wlr_xdg_toplevel_set_size(toplevel, final.width, final.height);

        this->deco_state = gtk4_decoration_tx_state::WAITING_FINAL;
    }

    void size_updated()
    {
        if (!toplevel)
        {
            wf::txn::emit_object_ready(this);
            return;
        }

        wlr_box box = toplevel->base->geometry;

        LOGD("Size is ", wf::dimensions(box), " state is ", (int)deco_state);

        auto vg = wf::toplevel_cast(target_view)->get_geometry();
        if (wf::dimensions(box) != committed)
        {
            LOGI(wf::dimensions(box), " != ", committed);
            committed = wf::dimensions(box);
            adjust_target_geometry();
            auto min_width = 300;
            if (target_view->get_wlr_surface() && (box.width < min_width))
            {
                if (wlr_xwayland_surface_try_from_wlr_surface(target_view->get_wlr_surface()))
                {
                    LOGD("Adjusting target on deco commit: width: ", box.width, " < ", min_width);
                    wlr_xwayland_surface_configure(wlr_xwayland_surface_try_from_wlr_surface(target_view->
                        get_wlr_surface()),
                        vg.x, vg.y, std::max(min_width - 11.0, (double)vg.width),
                        vg.height - (margin_top + margin_bottom) / 2 - 9);
                }
            }
        }

        switch (this->deco_state)
        {
          case gtk4_decoration_tx_state::STABLE:
            // Client simply committed, nothing has changed
            return;

          case gtk4_decoration_tx_state::TENTATIVE:
            // Client commits twice?
            if (use_csd && (wf::dimensions(box) != wf::dimensions(vg)))
            {
                wlr_xdg_toplevel_set_size(toplevel, vg.width, vg.height);
            }

            break;

          case gtk4_decoration_tx_state::START:
            deco_state = gtk4_decoration_tx_state::TENTATIVE;
            break;

          case gtk4_decoration_tx_state::WAITING_FINAL:
            deco_state = gtk4_decoration_tx_state::STABLE;
            wf::txn::emit_object_ready(this);
            break;
        }

        if (!root_node->is_enabled())
        {
            wf::scene::set_node_enabled(root_node, true);
            wf::scene::set_node_enabled(target_view->get_root_node(), true);
            wf::scene::set_node_enabled(target_view->get_root_node(), true);
            wf::scene::update(target_view->get_root_node(), wf::scene::update_flag::REFOCUS);
        }
    }

    void commit()
    {
        if (!toplevel)
        {
            wf::txn::emit_object_ready(this);
            return;
        }

        set_pending_size(wf::dimensions(decorated_toplevel->pending().geometry));

        deco_state = gtk4_decoration_tx_state::START;

        LOGD("Committing with ", pending, " state is ", (int)deco_state);
        recompute_mask();

        wlr_box box = toplevel->base->geometry;

        if (wf::dimensions(box) != pending)
        {
            if (!use_csd)
            {
                wlr_xdg_toplevel_set_size(toplevel, pending.width, pending.height);
            }
        } else
        {
            wf::txn::emit_object_ready(this);
            return;
        }

        committed = pending;
        size_updated();
    }

    void apply()
    {
        if (toplevel)
        {
            pending_state.merge_state(toplevel->base->surface);
        }

        deco_node->apply_state(std::move(pending_state));
        recompute_mask();
        if (on_commit.is_connected())
        {
            on_commit.emit(nullptr);
        }
    }

    void adjust_target_geometry()
    {
        auto desired = wf::dimensions(deco_node->get_bounding_box());
        auto tg = wf::dimensions(toplevel->base->geometry);
        if (!target_view->get_wlr_surface())
        {
            return;
        }

        desired.width  -= margin_left + margin_right;
        desired.height -= margin_top + margin_bottom + 2;
        desired.width   = std::max(tg.width, desired.width);
        desired.height  = std::max(tg.height, desired.height);
        desired.width   = std::max(desired.width, wf::toplevel_cast(
            target_view)->toplevel()->get_min_size().width);
        desired.height = std::max(desired.height, wf::toplevel_cast(
            target_view)->toplevel()->get_min_size().height);

        if (desired != tg)
        {
            LOGD("Adjusting target on deco commit: ", desired, " != ", tg);
            if (wlr_xwayland_surface_try_from_wlr_surface(target_view->get_wlr_surface()))
            {
                auto vg = wf::toplevel_cast(target_view)->get_geometry();
                wlr_xwayland_surface_configure(wlr_xwayland_surface_try_from_wlr_surface(target_view->
                    get_wlr_surface()),
                    vg.x, vg.y, desired.width, desired.height);
            } else
            {
                wlr_xdg_toplevel_set_size(wlr_xdg_toplevel_try_from_wlr_surface(target_view->
                    get_wlr_surface()),
                    tg.width, tg.height);
            }
        }
    }

    std::shared_ptr<wf::toplevel_t> decorated_toplevel;

  public:
    gtk4_decoration_object_t(
        wlr_xdg_toplevel *toplevel, wayfire_view target_view, decoration_node_t deco_node,
        std::weak_ptr<gtk4_mask_node_t> mask, std::shared_ptr<wf::toplevel_t> decorated_toplevel,
        std::shared_ptr<wf::scene::translation_node_t> root_node)
    {
        this->toplevel    = toplevel;
        this->deco_node   = deco_node;
        this->target_view = target_view;
        this->mask_node   = mask;
        this->decorated_toplevel = decorated_toplevel;
        this->root_node = root_node;

        on_commit.set_callback([=] (void*)
        {
            if (!target_view->is_mapped())
            {
                return;
            }

            if (toplevel)
            {
                pending_state.merge_state(toplevel->base->surface);
            }

            if ((deco_state == gtk4_decoration_tx_state::STABLE) ||
                (deco_state == gtk4_decoration_tx_state::TENTATIVE))
            {
                deco_node->apply_state(std::move(pending_state));
                recompute_mask();
            }

            size_updated();
        });

        on_deco_destroy.set_callback([=] (void*)
        {
            handle_destroy();
            if (decorator_resource)
            {
                target_view->close();
            }
        });

        on_target_destroy.set_callback([=] (void*)
        {
            handle_destroy();
        });

        on_request_move.set_callback([=] (void*)
        {
            wf::get_core().default_wm->move_request(wf::toplevel_cast(target_view));
        });

        on_request_resize.set_callback([=] (void*)
        {
            wf::get_core().default_wm->resize_request(wf::toplevel_cast(target_view));
        });

        on_request_deco_maximize.set_callback([=] (void*)
        {
            wf::get_core().default_wm->tile_request(
                wf::toplevel_cast(target_view),
                wf::toplevel_cast(target_view)->pending_tiled_edges() ?
                0 : wf::TILED_EDGES_ALL);
            handle_maximize();
        });

        on_request_target_maximize.set_callback([=] (void*)
        {
            handle_maximize();
        });

        on_request_minimize.set_callback([=] (void*)
        {
            wf::get_core().default_wm->minimize_request(wf::toplevel_cast(target_view),
                !wf::toplevel_cast(target_view)->minimized);
        });

        on_new_popup.set_callback([=] (void *data)
        {
            auto popup = (decltype(toplevel->base->popup))data;

            if (!popup)
            {
                return;
            }

            if (deco_node->get_surface() != popup->parent)
            {
                return;
            }

            popup->parent = target_view->get_wlr_surface();
            create_xdg_popup(popup);
        });

        on_request_move.connect(&toplevel->events.request_move);
        on_request_resize.connect(&toplevel->events.request_resize);
        on_request_deco_maximize.connect(&toplevel->events.request_maximize);
        target_view->connect(&on_fullscreen);
        target_view->connect(&on_view_title_changed);
        target_view->connect(&on_view_tiled);
        target_view->connect(&on_target_unmapped);
        on_request_minimize.connect(&toplevel->events.request_minimize);
        on_new_popup.connect(&wlr_xdg_surface_try_from_wlr_surface(
            deco_node->get_surface())->client->shell->events.new_popup);
        on_commit.connect(&toplevel->base->surface->events.commit);
        on_deco_destroy.connect(&toplevel->events.destroy);
        if (wlr_xdg_toplevel_try_from_wlr_surface(target_view->get_wlr_surface()))
        {
            on_request_target_maximize.connect(&wlr_xdg_toplevel_try_from_wlr_surface(target_view->
                get_wlr_surface())->events.request_maximize);
            on_target_destroy.connect(&wlr_xdg_toplevel_try_from_wlr_surface(
                target_view->get_wlr_surface())->events.destroy);
        }

        if (wf::toplevel_cast(target_view)->toplevel()->pending().fullscreen)
        {
            wf::scene::remove_child(root_node);
        } else
        {
            wf::scene::add_front(target_view->get_surface_root_node(), root_node);
        }
    }

    void handle_destroy()
    {
        ungroup_window(NULL, NULL, target_view->get_id(), true);
        if (decorator_resource)
        {
            wf_decorator_manager_send_destroy_decoration(decorator_resource, target_view->get_id());
        }

        on_commit.disconnect();
        on_deco_destroy.disconnect();
        on_target_destroy.disconnect();
        on_target_unmapped.disconnect();
        on_new_popup.disconnect();
        on_request_move.disconnect();
        on_request_resize.disconnect();
        on_request_minimize.disconnect();
        on_request_deco_maximize.disconnect();
        on_request_target_maximize.disconnect();
        on_fullscreen.disconnect();
        on_view_title_changed.disconnect();
        on_view_tiled.disconnect();

        this->toplevel = nullptr;
    }

    wf::signal::connection_t<wf::view_unmapped_signal> on_target_unmapped = [=] (wf::view_unmapped_signal *ev)
    {
        handle_destroy();
    };

    void handle_maximize()
    {
        if (wf::toplevel_cast(target_view)->pending_tiled_edges() == wf::TILED_EDGES_ALL)
        {
            root_node->set_offset({-margin_left, -margin_top});
        } else if (wf::toplevel_cast(target_view)->pending_tiled_edges())
        {
            root_node->set_offset(
                {use_csd ? -(margin_left - margin_offset.x - margin_offset.x / 2 + 1) : -margin_left,
                    use_csd ? -(margin_top - margin_offset.y - margin_offset.y / 2 - 2) : -margin_top});
        } else
        {
            root_node->set_offset({use_csd ? -(margin_left - margin_offset.x) : -margin_left,
                use_csd ? -(margin_top - margin_offset.y) : -margin_top});
        }
    }

    wf::signal::connection_t<wf::view_title_changed_signal> on_view_title_changed =
        [=] (wf::view_title_changed_signal *ev)
    {
        wf_decorator_manager_send_title_changed(decorator_resource,
            target_view->get_id(), target_view->get_title().c_str());
    };

    wf::signal::connection_t<wf::view_tiled_signal> on_view_tiled = [=] (wf::view_tiled_signal *ev)
    {
        handle_maximize();
    };

    wf::signal::connection_t<wf::view_fullscreen_signal> on_fullscreen =
        [=] (wf::view_fullscreen_signal *ev)
    {
        if (ev->view != target_view)
        {
            return;
        }

        if (ev->state)
        {
            wf::scene::remove_child(root_node);
        } else
        {
            wf::scene::readd_front(target_view->get_surface_root_node(), root_node);
        }
    };

    void set_margins(int top, int bottom, int left, int right, wf::point_t offset)
    {
        this->margin_top    = top;
        this->margin_bottom = bottom;
        this->margin_left   = left;
        this->margin_right  = right;
        this->margin_offset = offset;
    }

    uint32_t group_id = 0;
    bool use_csd     = false;
    bool borders_set = false;
    wayfire_view target_view;
    wf::point_t ungroup_restore_position;
    std::shared_ptr<wf::scene::translation_node_t> root_node;

  private:
    wf::dimensions_t pending   = {0, 0};
    wf::dimensions_t committed = {0, 0};

    void recompute_mask()
    {
        auto masked = mask_node.lock();
        wf::dassert(masked != nullptr, "Masked node does not exist anymore??");

        auto bbox = deco_node->get_bounding_box();

        masked->allowed = bbox;
        wf::geometry_t cut_out = wf::geometry_t{
            .x     = bbox.x + margin_left,
            .y     = bbox.y + margin_top,
            .width = bbox.width - margin_left - margin_right,
            .height = bbox.height - margin_top - margin_bottom - 2,
        };
        masked->allowed ^= cut_out;
    }

    double margin_left   = 0;
    double margin_top    = 0;
    double margin_right  = 0;
    double margin_bottom = 0;
    wf::point_t margin_offset;

    wf::scene::surface_state_t pending_state;
    std::weak_ptr<gtk4_mask_node_t> mask_node;

    wlr_xdg_toplevel *toplevel;
    decoration_node_t deco_node;

    wf::wl_listener_wrapper on_commit, on_deco_destroy, on_target_destroy, on_new_popup;
    wf::wl_listener_wrapper on_request_move, on_request_resize, on_request_minimize;
    wf::wl_listener_wrapper on_request_deco_maximize, on_request_target_maximize;
    gtk4_decoration_tx_state deco_state = gtk4_decoration_tx_state::STABLE;
};

class gtk4_toplevel_custom_data : public wf::custom_data_t
{
  public:
    std::shared_ptr<gtk4_decoration_object_t> decoration;
    wf::point_t margin_offset;
};

/* ------------------------------------------------------------------------- *
 * Group tab morph animation.
 *
 * Adapted from the wf-group-tab (Compiz-style) plugin: uses 2D transformers
 * to scale/translate/fade live views. When a tab icon is clicked to switch
 * windows in a group, the new window fades in while morphing from the old
 * window's exact geometry to its own, and the old window scales toward the
 * new one behind it. When the animation finishes, the old window is hidden.
 * ------------------------------------------------------------------------- */

static const char *morph_transformer_name = "gtk4-deco-morph";
static const int morph_duration_ms = 300;

/* --- group / ungroup morph tunables ---------------------------------------
 * Durations are per-effect; the *_spring_k values control how much the scale
 * and translation overshoot past their target before settling (0 = none, ~1 =
 * lively, >1.5 = bouncy). The alpha ramps are the dependency-free "blur":
 * a window fades through translucency while it scales, reading as a soft,
 * motion-blurred morph rather than a hard cut. */
static const int    group_anim_ms       = 380;  /* implode-into-parent length  */
static const int    ungroup_anim_ms     = 430;  /* burst-out length            */
static const double tab_spring_k        = 0.9;   /* tab-switch overshoot        */
static const double group_spring_k      = 1.15;  /* implode overshoot           */
static const double ungroup_spring_k    = 1.7;   /* burst overshoot (dramatic)  */
static const double parent_pulse_amp    = 0.07;  /* parent "welcome" scale bump */
static const double reveal_pop_scale    = 0.90;  /* revealed member start scale */
static const double ungroup_start_alpha = 0.20;  /* leaving view initial softness */

static void show_view_node(wayfire_view v)
{
    while (!v->get_root_node()->is_enabled())
    {
        wf::scene::set_node_enabled(v->get_root_node(), true);
    }
}

static void hide_view_node(wayfire_view v)
{
    while (v->get_root_node()->is_enabled())
    {
        wf::scene::set_node_enabled(v->get_root_node(), false);
    }
}

static wayfire_view find_view_by_id(uint32_t id)
{
    for (auto& v : wf::get_core().get_all_views())
    {
        if ((v->role == wf::VIEW_ROLE_TOPLEVEL) && (v->get_id() == id))
        {
            return v;
        }
    }

    return nullptr;
}

/**
 * Scale + translation which place @view exactly inside @rect.
 * The 2D transformer scales around the center of the view's current
 * geometry, hence the correction terms. (From wf-group-tab.)
 */
static void transform_for_rect(wayfire_view view, const wf::geometry_t& rect,
    double& sx, double& sy, double& tx, double& ty)
{
    auto g = wf::toplevel_cast(view)->get_geometry();
    sx = (g.width > 0) ? ((double)rect.width / g.width) : 1.0;
    sy = (g.height > 0) ? ((double)rect.height / g.height) : 1.0;

    double x_after_scale = g.x + g.width * (1.0 - sx) / 2.0;
    double y_after_scale = g.y + g.height * (1.0 - sy) / 2.0;

    tx = rect.x - x_after_scale;
    ty = rect.y - y_after_scale;
}

/* ------------------------------ morph ------------------------------ *
 *
 * Generalized spring-morph engine. Every effect here (tab switch, group,
 * ungroup) animates at most two live views through view_2d_transformer_t.
 * Each "slot" interpolates scale + translation + alpha from a start pose to
 * an end pose using two curves:
 *   - a spring / ease-out-back curve drives scale & translation, giving the
 *     dramatic snap-together / burst-apart overshoot;
 *   - a plain smoothstep drives alpha, so opacity never leaves [0, 1].
 * A slot may instead be a "pulse": a scale bump that rises and returns to 1,
 * used to make the surviving window acknowledge a group/ungroup.
 * The soft alpha ramps are the dependency-free "blur": a view fades through
 * translucency while it scales, reading as a motion-blurred morph rather than
 * a hard cut. (True per-pixel blur would need a shader transformer.)
 * ------------------------------------------------------------------------- */

static constexpr double DECO_PI = 3.14159265358979323846;

/* Smoothstep, 0..1, no overshoot. Used for alpha. */
static double ease_smooth(double p)
{
    return p * p * (3.0 - 2.0 * p);
}

/* Ease-out-back (spring): overshoots past 1, then settles to exactly 1 at
 * p == 1. `k` scales the overshoot amount. */
static double ease_spring(double p, double k)
{
    const double c1 = 1.70158 * k;
    const double c3 = c1 + 1.0;
    const double q  = p - 1.0;
    return 1.0 + c3 * q * q * q + c1 * q * q;
}

struct anim_slot_t
{
    wayfire_view view = nullptr;
    std::shared_ptr<wf::scene::view_2d_transformer_t> tr;

    /* scale / translation: start -> end */
    double sx0 = 1, sy0 = 1, tx0 = 0, ty0 = 0;
    double sx1 = 1, sy1 = 1, tx1 = 0, ty1 = 0;
    /* alpha: start -> end */
    double a0 = 1, a1 = 1;

    bool   spring = false;    /* spring easing for scale/translation      */
    bool   pulse  = false;    /* scale bump 1 -> 1+amp -> 1 (ignores s/t)  */
    double pulse_amp = 0.0;

    bool   hide_on_finish = false;
    bool   move_on_finish = false;
    double move_x = 0, move_y = 0;
};

struct morph_state_t
{
    bool active = false;
    /* "old"/"new" names kept so existing call sites keep compiling; each is
     * simply one of the (at most two) animated slots. */
    anim_slot_t old_slot;
    anim_slot_t new_slot;
    uint32_t start_time = 0;
    int      duration_ms = morph_duration_ms;
    double   spring_k = 1.0;
    wf::output_t *output = nullptr;
};

static morph_state_t morph;
static void morph_frame();
static wf::effect_hook_t morph_hook = [] () { morph_frame(); };

/* Which view id is currently shown for each group id. The morph source is
 * looked up here instead of being inferred from scene enable state, which can
 * drift after a few switches and leave nothing to morph from (the switch then
 * snaps instead of morphing). */
static std::map<uint32_t, uint32_t> group_visible;

static void finish_slot(anim_slot_t& s)
{
    if (!s.view)
    {
        return;
    }

    s.view->damage();
    s.view->get_transformed_node()->rem_transformer(morph_transformer_name);

    if (s.move_on_finish && wf::toplevel_cast(s.view))
    {
        wf::toplevel_cast(s.view)->move(s.move_x, s.move_y);
    }

    if (s.hide_on_finish && s.view->is_mapped())
    {
        hide_view_node(s.view);
    }

    s.view->damage();
}

static void finish_morph()
{
    if (!morph.active)
    {
        return;
    }

    if (morph.output)
    {
        morph.output->render->rem_effect(&morph_hook);
    }

    finish_slot(morph.old_slot);
    finish_slot(morph.new_slot);

    morph = morph_state_t{};
}

/* Attach a fresh 2D transformer and prime it to the slot's start pose. */
static void attach_slot(anim_slot_t& s)
{
    if (!s.view)
    {
        return;
    }

    s.tr = std::make_shared<wf::scene::view_2d_transformer_t>(s.view);
    s.view->get_transformed_node()->add_transformer(
        s.tr, wf::TRANSFORMER_2D, morph_transformer_name);

    if (s.pulse)
    {
        s.tr->scale_x = s.tr->scale_y = 1.0;
        s.tr->translation_x = s.tr->translation_y = 0.0;
    } else
    {
        s.tr->scale_x = s.sx0;
        s.tr->scale_y = s.sy0;
        s.tr->translation_x = s.tx0;
        s.tr->translation_y = s.ty0;
    }

    s.tr->alpha = std::clamp(s.a0, 0.0, 1.0);
}

static void begin_morph(wf::output_t *output, int duration_ms, double spring_k)
{
    morph.output      = output;
    morph.duration_ms = duration_ms;
    morph.spring_k    = spring_k;
    morph.start_time  = wf::get_current_time();
    morph.active      = true;

    attach_slot(morph.old_slot);
    attach_slot(morph.new_slot);

    output->render->add_effect(&morph_hook, wf::OUTPUT_EFFECT_PRE);

    if (morph.old_slot.view)
    {
        morph.old_slot.view->damage();
    }

    if (morph.new_slot.view)
    {
        morph.new_slot.view->damage();
    }
}

static void apply_slot(anim_slot_t& s, double p, double spring_k)
{
    if (!s.tr)
    {
        return;
    }

    if (s.pulse)
    {
        double b = s.pulse_amp * std::sin(DECO_PI * p);
        s.tr->scale_x = 1.0 + b;
        s.tr->scale_y = 1.0 + b;
        s.tr->translation_x = 0.0;
        s.tr->translation_y = 0.0;
        s.tr->alpha = std::clamp(s.a0 + (s.a1 - s.a0) * ease_smooth(p), 0.0, 1.0);
        return;
    }

    double e_pos = s.spring ? ease_spring(p, spring_k) : ease_smooth(p);
    double e_a   = ease_smooth(p);

    s.tr->scale_x = s.sx0 + (s.sx1 - s.sx0) * e_pos;
    s.tr->scale_y = s.sy0 + (s.sy1 - s.sy0) * e_pos;
    s.tr->translation_x = s.tx0 + (s.tx1 - s.tx0) * e_pos;
    s.tr->translation_y = s.ty0 + (s.ty1 - s.ty0) * e_pos;
    s.tr->alpha = std::clamp(s.a0 + (s.a1 - s.a0) * e_a, 0.0, 1.0);
}

static void morph_frame()
{
    if (!morph.active)
    {
        return;
    }

    /* Any participating view disappearing aborts the animation cleanly. */
    auto gone = [] (anim_slot_t& s) {
        return s.view && !s.view->is_mapped();
    };
    if (gone(morph.old_slot) || gone(morph.new_slot))
    {
        finish_morph();
        return;
    }

    double p = (double)(wf::get_current_time() - morph.start_time) / morph.duration_ms;
    p = std::clamp(p, 0.0, 1.0);

    if (morph.old_slot.view)
    {
        morph.old_slot.view->damage();
    }

    if (morph.new_slot.view)
    {
        morph.new_slot.view->damage();
    }

    apply_slot(morph.old_slot, p, morph.spring_k);
    apply_slot(morph.new_slot, p, morph.spring_k);

    if (morph.old_slot.view)
    {
        morph.old_slot.view->damage();
    }

    if (morph.new_slot.view)
    {
        morph.new_slot.view->damage();
    }

    if (p >= 1.0)
    {
        finish_morph();
    }
}

/* ------------------------- effect builders ------------------------- */

/* Tab switch: the old view stays opaque and springs toward the new geometry,
 * the new view springs in from the old geometry while fading up on top. */
static void start_morph(wayfire_view old_view, wayfire_view new_view)
{
    finish_morph();

    if (!old_view || !new_view || (old_view == new_view))
    {
        return;
    }

    auto output = new_view->get_output() ? new_view->get_output() : old_view->get_output();
    if (!output)
    {
        /* Can't animate without an output; fall back to instant switch. */
        hide_view_node(old_view);
        return;
    }

    auto og = wf::toplevel_cast(old_view)->get_geometry();
    auto ng = wf::toplevel_cast(new_view)->get_geometry();

    anim_slot_t o;
    o.view = old_view;
    transform_for_rect(old_view, ng, o.sx1, o.sy1, o.tx1, o.ty1);
    o.a0 = 1.0; o.a1 = 1.0;
    o.spring = true;
    o.hide_on_finish = true;

    anim_slot_t n;
    n.view = new_view;
    transform_for_rect(new_view, og, n.sx0, n.sy0, n.tx0, n.ty0);
    n.a0 = 0.0; n.a1 = 1.0;
    n.spring = true;

    morph.old_slot = o;
    morph.new_slot = n;
    begin_morph(output, morph_duration_ms, tab_spring_k);
}

/* Group: the child implodes into and dissolves onto the parent, which pulses
 * to acknowledge it. The child is stacked on the parent and hidden when the
 * animation finishes (the move/hide is deferred to finish_slot). */
static void start_group_anim(wayfire_view child, wayfire_view parent,
    const wf::geometry_t& parent_geom)
{
    finish_morph();

    if (!child || !parent || (child == parent))
    {
        return;
    }

    auto output = child->get_output() ? child->get_output() : parent->get_output();
    if (!output)
    {
        return;
    }

    anim_slot_t c;
    c.view = child;
    transform_for_rect(child, parent_geom, c.sx1, c.sy1, c.tx1, c.ty1);
    c.a0 = 1.0; c.a1 = 0.0;               /* dissolve as it is absorbed */
    c.spring = true;
    c.hide_on_finish = true;
    c.move_on_finish = true;
    c.move_x = parent_geom.x;
    c.move_y = parent_geom.y;

    anim_slot_t p;
    p.view = parent;
    p.pulse = true;
    p.pulse_amp = parent_pulse_amp;

    morph.old_slot = c;
    morph.new_slot = p;
    begin_morph(output, group_anim_ms, group_spring_k);
}

/* Ungroup: the leaving view bursts out from where it sat in the group to its
 * restore geometry, sharpening from a soft dissolve with a big overshoot; a
 * newly revealed group member (if any) pops in behind it. */
static void start_ungroup_anim(wayfire_view leaving, const wf::geometry_t& grouped_geom,
    wayfire_view revealed)
{
    finish_morph();

    if (!leaving)
    {
        return;
    }

    auto output = leaving->get_output();
    if (!output && revealed)
    {
        output = revealed->get_output();
    }

    if (!output)
    {
        return;
    }

    anim_slot_t v;
    v.view = leaving;
    transform_for_rect(leaving, grouped_geom, v.sx0, v.sy0, v.tx0, v.ty0);
    v.a0 = ungroup_start_alpha; v.a1 = 1.0;   /* sharpen from soft */
    v.spring = true;

    anim_slot_t r;
    if (revealed && (revealed != leaving))
    {
        r.view = revealed;
        r.sx0 = reveal_pop_scale; r.sy0 = reveal_pop_scale;
        r.sx1 = 1.0; r.sy1 = 1.0;
        r.a0 = 0.0; r.a1 = 1.0;
        r.spring = true;
    }

    morph.old_slot = r;   /* may be an empty slot (no reveal) */
    morph.new_slot = v;
    begin_morph(output, ungroup_anim_ms, ungroup_spring_k);
}

/* --------------------------- group drag ---------------------------- *
 * Compositor-side drag-to-group. GTK drag-and-drop is unreliable on the
 * decoration surfaces (the button's internal gestures and the wayland DnD
 * grab fight over the pointer), so instead the client just tells us when a
 * drag begins on a titlebar icon. We dim the source window, switch the
 * cursor, and on button release group it with whatever toplevel is under
 * the cursor — anywhere on the target window works, not just its titlebar.
 * The client is notified via the windows_grouped event so it can rebuild
 * its tab strips. */

struct group_drag_t
{
    bool active = false;
    wayfire_view source = nullptr;
    std::shared_ptr<wf::scene::view_2d_transformer_t> dim;
};

static group_drag_t gdrag;
static const char *drag_transformer_name = "gtk4-deco-drag";

static void cancel_group_drag()
{
    if (!gdrag.active)
    {
        return;
    }

    if (gdrag.source)
    {
        gdrag.source->damage();
        gdrag.source->get_transformed_node()->rem_transformer(drag_transformer_name);
        gdrag.source->damage();
    }

    wf::get_core().set_cursor("default");
    gdrag = group_drag_t{};
}

/* Hard reset. Cancels any in-flight morph/drag and removes every transformer
 * this plugin can add from every toplevel, so the view tree is back to a
 * known-clean state. Called at the start of each tab switch so a click always
 * works, even if a previous animation left a stuck transformer (which is what
 * makes a window go invisible / tiny / "not morph"). */
static void reset_all_deco_transforms()
{
    finish_morph();
    cancel_group_drag();

    for (auto& v : wf::get_core().get_all_views())
    {
        if (v->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            continue;
        }

        auto node = v->get_transformed_node();
        node->rem_transformer(morph_transformer_name);
        node->rem_transformer(drag_transformer_name);
        v->damage();
    }
}

void do_start_group_drag(wl_client*, struct wl_resource*, uint32_t id)
{
    cancel_group_drag();

    auto view = find_view_by_id(id);
    if (!view)
    {
        return;
    }

    auto data = wf::toplevel_cast(view)->toplevel()->get_data<gtk4_toplevel_custom_data>();
    if (!data || !data->decoration)
    {
        /* Only decorated windows can be dragged. Grouped windows are allowed:
         * dropping over another window regroups them, dropping over empty
         * space pulls them back out of their group. */
        return;
    }

    gdrag.active = true;
    gdrag.source = view;
    gdrag.dim    = std::make_shared<wf::scene::view_2d_transformer_t>(view);
    view->get_transformed_node()->add_transformer(
        gdrag.dim, wf::TRANSFORMER_2D, drag_transformer_name);
    view->damage();
    gdrag.dim->alpha = 0.85;
    view->damage();

    wf::get_core().set_cursor("grabbing");
    LOGI("Group drag started for view ", id);
}

void do_update_borders(wl_client*, struct wl_resource*, uint32_t id, uint32_t top, uint32_t bottom,
    uint32_t left, uint32_t right)
{
    wayfire_view target = nullptr;
    for (auto& v : wf::get_core().get_all_views())
    {
        if (v->get_id() == id)
        {
            target = v;
            break;
        }
    }

    if (!target)
    {
        return;
    }

    auto data = wf::toplevel_cast(target)->toplevel()->get_data_safe<gtk4_toplevel_custom_data>();
    if (!data->decoration)
    {
        return;
    }

    LOGD("do_update_borders: ", top, ", ", bottom, ", ", left, ", ", right);

    int t = top, l = left;
    bool use_csd = data->decoration->use_csd;
    LOGI(use_csd);

    deco_margins.top = top - bottom + 1;
    data->decoration->set_margins(top, bottom, left, right, data->margin_offset);
    data->decoration->root_node->set_offset({use_csd ? -(l - data->margin_offset.x) : -l,
        use_csd ? -(t - data->margin_offset.y) : -t});
    wf::get_core().tx_manager->schedule_object(wf::toplevel_cast(data->decoration->target_view)->toplevel());
}

void do_group_windows(wl_client*, struct wl_resource*, uint32_t parent_id, uint32_t child_id)
{
    finish_morph();

    uint32_t group_id = 1;
    wayfire_view parent = nullptr, child = nullptr;
    for (auto& v : wf::get_core().get_all_views())
    {
        if (v->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            continue;
        }

        if (v->get_id() == parent_id)
        {
            parent = v;
        }

        if (v->get_id() == child_id)
        {
            child = v;
        }

        auto data = wf::toplevel_cast(v)->toplevel()->get_data<gtk4_toplevel_custom_data>();
        if (data)
        {
            if (data->decoration->group_id >= group_id)
            {
                group_id = data->decoration->group_id + 1;
            }
        }
    }

    if (!parent || !child)
    {
        return;
    }

    auto parent_data = wf::toplevel_cast(parent)->toplevel()->get_data<gtk4_toplevel_custom_data>();
    auto child_data  = wf::toplevel_cast(child)->toplevel()->get_data<gtk4_toplevel_custom_data>();

    if (!parent_data || !child_data)
    {
        return;
    }

    if (!parent_data->decoration->group_id)
    {
        parent_data->decoration->group_id = child_data->decoration->group_id = group_id;
    } else
    {
        child_data->decoration->group_id = parent_data->decoration->group_id;
    }

    while (!parent->get_root_node()->is_enabled())
    {
        wf::scene::set_node_enabled(parent->get_root_node(), true);
    }

    /* Record where each window should return to on ungroup. The child stays
     * enabled for now: start_group_anim animates it imploding into the parent
     * and only stacks (move) + hides it once the animation completes. */
    auto cg = wf::toplevel_cast(child)->get_geometry();
    child_data->decoration->ungroup_restore_position = {cg.x, cg.y};
    auto vg = wf::toplevel_cast(parent)->get_geometry();
    parent_data->decoration->ungroup_restore_position = {vg.x, vg.y};

    group_visible[parent_data->decoration->group_id] = parent->get_id();
    start_group_anim(child, parent, vg);
}

void do_select_window(wl_client*, struct wl_resource*, uint32_t select_id)
{
    wayfire_view view = nullptr;
    for (auto& v : wf::get_core().get_all_views())
    {
        if (v->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            continue;
        }

        if (v->get_id() == select_id)
        {
            view = v;
            break;
        }
    }

    if (!view)
    {
        return;
    }

    /* Reset to a clean state on every click so the switch always works,
     * regardless of what a previous morph/drag left behind. */
    reset_all_deco_transforms();

    auto view_data = wf::toplevel_cast(view)->toplevel()->get_data<gtk4_toplevel_custom_data>();

    if (!view_data)
    {
        return;
    }

    auto group_id = view_data->decoration->group_id;

    /* The morph source is the window that was visible in this group before
     * this click. Prefer the explicitly tracked one; fall back to scanning
     * enabled nodes (first switch, or if the tracked window went away). */
    wayfire_view old_visible = nullptr;
    if (group_id)
    {
        auto it = group_visible.find(group_id);
        if ((it != group_visible.end()) && (it->second != select_id))
        {
            auto candidate = find_view_by_id(it->second);
            if (candidate && candidate->is_mapped())
            {
                auto cdata = wf::toplevel_cast(candidate)->toplevel()->get_data<gtk4_toplevel_custom_data>();
                if (cdata && cdata->decoration && (cdata->decoration->group_id == group_id))
                {
                    old_visible = candidate;
                }
            }
        }

        if (!old_visible)
        {
            for (auto& v : wf::get_core().get_all_views())
            {
                if ((v->role != wf::VIEW_ROLE_TOPLEVEL) || (v == view))
                {
                    continue;
                }

                auto data = wf::toplevel_cast(v)->toplevel()->get_data<gtk4_toplevel_custom_data>();
                if (data && data->decoration && (data->decoration->group_id == group_id) &&
                    v->get_root_node()->is_enabled())
                {
                    old_visible = v;
                    break;
                }
            }
        }
    }

    show_view_node(view);
    wf::get_core().default_wm->focus_raise_view(view);

    if (!group_id)
    {
        return;
    }

    /* Record the new visible window for this group. */
    group_visible[group_id] = select_id;

    /* Hide every other member immediately, except the previously visible
     * window: that one stays on screen for the morph animation and is
     * hidden when the morph finishes. */
    for (auto& v : wf::get_core().get_all_views())
    {
        if ((v->role != wf::VIEW_ROLE_TOPLEVEL) || (v == view) || (v == old_visible))
        {
            continue;
        }

        auto data = wf::toplevel_cast(v)->toplevel()->get_data<gtk4_toplevel_custom_data>();
        if (data)
        {
            if (data->decoration->group_id == group_id)
            {
                while (v->get_root_node()->is_enabled())
                {
                    wf::scene::set_node_enabled(v->get_root_node(), false);
                }
            }
        }
    }

    if (old_visible)
    {
        /* Make sure the source is on-screen for the animation, then morph.
         * finish_morph() hides it again when the animation completes. */
        show_view_node(old_visible);
        start_morph(old_visible, view);
    }
}

void ungroup_window(wl_client*, struct wl_resource*, uint32_t id, bool closing)
{
    /* Ungrouping (also called on view destruction) invalidates any in-flight
     * morph involving this view. */
    if (morph.active &&
        ((morph.old_slot.view && (morph.old_slot.view->get_id() == id)) ||
         (morph.new_slot.view && (morph.new_slot.view->get_id() == id))))
    {
        finish_morph();
    }

    wayfire_view view = find_view_by_id(id);
    if (!view)
    {
        return;
    }

    auto view_data = wf::toplevel_cast(view)->toplevel()->get_data<gtk4_toplevel_custom_data>();

    if (!view_data)
    {
        return;
    }

    auto rg = view_data->decoration->ungroup_restore_position;
    view_data->decoration->ungroup_restore_position = {0, 0};

    auto group_id = view_data->decoration->group_id;
    view_data->decoration->group_id = 0;

    if (!group_id)
    {
        return;
    }

    /* Geometry the view occupied while stacked in the group — the animation
     * bursts out from here to the restore position. Captured before the move. */
    auto grouped_geom = wf::toplevel_cast(view)->get_geometry();

    if (!closing)
    {
        wf::toplevel_cast(view)->move(rg.x, rg.y);
    }

    while (!view->get_root_node()->is_enabled())
    {
        wf::scene::set_node_enabled(view->get_root_node(), true);
    }

    /* Scan the rest of the (old) group: is any member already visible, and if
     * not, which hidden member (most recently focused) should be revealed? */
    bool any_visible = false;
    wayfire_view unhide_me = nullptr;
    auto last_group_focused_timestamp = 0;
    for (auto& v : wf::get_core().get_all_views())
    {
        if ((v->role != wf::VIEW_ROLE_TOPLEVEL) || (v == view))
        {
            continue;
        }

        auto data = wf::toplevel_cast(v)->toplevel()->get_data<gtk4_toplevel_custom_data>();
        if (data && (data->decoration->group_id == group_id))
        {
            if (v->get_root_node()->is_enabled())
            {
                any_visible = true;
            } else if (wf::get_focus_timestamp(v) > last_group_focused_timestamp)
            {
                last_group_focused_timestamp = wf::get_focus_timestamp(v);
                unhide_me = v;
            }
        }
    }

    wayfire_view revealed = nullptr;
    if (!any_visible && unhide_me)
    {
        while (!unhide_me->get_root_node()->is_enabled())
        {
            wf::scene::set_node_enabled(unhide_me->get_root_node(), true);
        }

        revealed = unhide_me;
    }

    /* Destroyed views can't be animated; only play the burst on a live ungroup. */
    if (!closing)
    {
        start_ungroup_anim(view, grouped_geom, revealed);
    }
}

void do_ungroup_window(wl_client*, struct wl_resource*, uint32_t id)
{
    ungroup_window(NULL, NULL, id, false);
}

/* Client asks the compositor to close a view (e.g. a titlebar close button). */
void do_close_request(wl_client*, struct wl_resource*, uint32_t id)
{
    auto view = find_view_by_id(id);
    if (view)
    {
        view->close();
    }
}

const struct wf_decorator_manager_interface decorator_implementation =
{
    .update_borders   = do_update_borders,
    .group_windows    = do_group_windows,
    .select_window    = do_select_window,
    .ungroup_window   = do_ungroup_window,
    .start_group_drag = do_start_group_drag,
    .close_request    = do_close_request
};

void unbind_decorator(wl_resource*)
{
    LOGD("Unbinding wf-decorator");
    decorator_resource = NULL;
}

static void handle_deco_client_destroy(struct wl_listener *listener, void *data)
{
    cancel_group_drag();
    unbind_decorator(NULL);
    for (auto & node : deco_nodes)
    {
        wf::scene::remove_child(node);
    }

    for (auto & output : wf::get_core().output_layout->get_outputs())
    {
        output->render->damage_whole();
    }

    deco_nodes.clear();
}

void bind_decorator(wl_client *client, void*, uint32_t, uint32_t id)
{
    LOGI("Binding wf-decorator");
    auto resource = wl_resource_create(client, &wf_decorator_manager_interface, 1, id);

    /* TODO: track active clients */
    wl_resource_set_implementation(resource, &decorator_implementation, NULL, NULL);
    decorator_resource = resource;
    deco_client_destroy_listener.notify = handle_deco_client_destroy;
    wl_client_add_destroy_listener(client, &deco_client_destroy_listener);
    for (auto & view : wf::get_core().get_all_views())
    {
        if (!wf::toplevel_cast(view) || !view->is_mapped())
        {
            continue;
        }

        wlr_server_decoration_manager_set_default_mode(
            wf::get_core().protocols.decorator_manager,
            WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);
        wf_decorator_manager_send_create_new_decoration(decorator_resource, view->get_id());
    }
}

class gtk4_decoration_plugin : public wf::plugin_interface_t
{
  public:
    wl_global *decorator_global;

    /* Completes a compositor-side group drag: on button release, group the
     * dragged window with the toplevel under the cursor. This is a raw
     * input signal, so it fires regardless of grabs. */
    wf::signal::connection_t<wf::input_event_signal<wlr_pointer_button_event>> on_pointer_button =
        [=] (wf::input_event_signal<wlr_pointer_button_event> *ev)
    {
        if (!gdrag.active || !ev->event)
        {
            return;
        }

        if ((ev->event->state != WL_POINTER_BUTTON_STATE_RELEASED) ||
            (ev->event->button != BTN_LEFT))
        {
            return;
        }

        auto source = gdrag.source;
        cancel_group_drag();

        if (!source || !source->is_mapped())
        {
            return;
        }

        auto sdata = wf::toplevel_cast(source)->toplevel()->get_data<gtk4_toplevel_custom_data>();
        uint32_t src_group = (sdata && sdata->decoration) ? sdata->decoration->group_id : 0;

        /* Find a decorated toplevel (other than the source) under the cursor. */
        wayfire_view target = nullptr;
        uint32_t tgt_group  = 0;
        auto cursor = wf::get_core().get_cursor_position();
        auto isec   = wf::get_core().scene()->find_node_at(cursor);
        if (isec)
        {
            auto v = wf::node_to_view(isec->node.get());
            if (v && (v != source) && (v->role == wf::VIEW_ROLE_TOPLEVEL))
            {
                auto tdata = wf::toplevel_cast(v)->toplevel()->get_data<gtk4_toplevel_custom_data>();
                if (tdata && tdata->decoration)
                {
                    target    = v;
                    tgt_group = tdata->decoration->group_id;
                }
            }
        }

        /* Two ungrouped windows dropped together -> start a new group. */
        if (target && (src_group == 0) && (tgt_group == 0))
        {
            LOGI("Group drag: grouping ", source->get_id(), " onto ", target->get_id());
            do_group_windows(NULL, NULL, target->get_id(), source->get_id());
            if (decorator_resource)
            {
                wf_decorator_manager_send_windows_grouped(decorator_resource,
                    target->get_id(), source->get_id());
            }

            return;
        }

        /* Dropped over a window in a *different* group (this also covers
         * dropping onto an ungrouped window while the source is grouped, and
         * dropping a grouped source onto another group). Move the source into
         * the target's group, leaving its old group first. */
        if (target && (tgt_group != src_group))
        {
            if (src_group)
            {
                LOGI("Group drag: moving ", source->get_id(), " out of group ", src_group);
                ungroup_window(NULL, NULL, source->get_id(), false);
                if (decorator_resource)
                {
                    wf_decorator_manager_send_window_ungrouped(decorator_resource, source->get_id());
                }
            }

            LOGI("Group drag: grouping ", source->get_id(), " onto ", target->get_id());
            do_group_windows(NULL, NULL, target->get_id(), source->get_id());
            if (decorator_resource)
            {
                wf_decorator_manager_send_windows_grouped(decorator_resource,
                    target->get_id(), source->get_id());
            }

            return;
        }

        /* Everything else means the source was NOT dropped onto a different
         * group: empty space, a non-decorated surface, or back onto its own
         * group's window. If the source is grouped, this is a "drag out" ->
         * pull it out of its group. A lone (ungrouped) window dropped on
         * nothing is a no-op. */
        if (src_group)
        {
            LOGI("Group drag: dragging ", source->get_id(), " out of group ", src_group);
            ungroup_window(NULL, NULL, source->get_id(), false);
            if (decorator_resource)
            {
                wf_decorator_manager_send_window_ungrouped(decorator_resource, source->get_id());
            }
        }
    };

    wf::signal::connection_t<wf::view_geometry_changed_signal> on_view_geometry_changed =
        [=] (wf::view_geometry_changed_signal *ev)
    {
        if ((ev->view->role != wf::VIEW_ROLE_TOPLEVEL) || !ev->view->is_mapped())
        {
            return;
        }

        auto data = wf::toplevel_cast(ev->view)->toplevel()->get_data<gtk4_toplevel_custom_data>();

        if (!data || !data->decoration)
        {
            return;
        }

        auto group_id = data->decoration->group_id;

        if (!group_id)
        {
            return;
        }

        auto vg = wf::toplevel_cast(ev->view)->get_geometry();

        for (auto& v : wf::get_core().get_all_views())
        {
            if ((v->role != wf::VIEW_ROLE_TOPLEVEL) || (v == ev->view))
            {
                continue;
            }

            auto cdata = wf::toplevel_cast(v)->toplevel()->get_data<gtk4_toplevel_custom_data>();
            if (!cdata || !cdata->decoration)
            {
                continue;
            }

            if (cdata->decoration->group_id != group_id)
            {
                continue;
            }

            wf::toplevel_cast(v)->move(vg.x, vg.y);
        }
    };

    void init_decor(wayfire_view view, wlr_surface *surface)
    {
        LOGD("Got decorator view ", view->get_title());

        auto id_str = std::string(view->get_title()).substr(gtk_decorator_prefix.length());
        auto id     = std::stoul(id_str.c_str());

        wayfire_toplevel_view target;
        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->get_id() == id)
            {
                target = toplevel_cast(v);
                break;
            }
        }

        auto deco_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(surface);

        if (!target)
        {
            LOGD("View is gone already?");
            view->close();
            return;
        }

        if (!target->toplevel())
        {
            LOGD("View does not support toplevel interface?");
            view->close();
            return;
        }

        if (!surface)
        {
            LOGD("Premap wlr_surface is null?");
            return;
        }

        if (!deco_toplevel)
        {
            LOGD("View is not an xdg_toplevel?");
            return;
        }

        auto data = target->toplevel()->get_data_safe<gtk4_toplevel_custom_data>();

        auto decoration_root_node = std::make_shared<wf::scene::translation_node_t>();
        auto mask_node = std::make_shared<gtk4_mask_node_t>();
        decoration_root_node->set_children_list({mask_node});

        auto deco_surf = std::make_shared<wf::scene::wlr_surface_node_t>(surface, false);
        deco_nodes.push_back(deco_surf);
        data->decoration = std::make_shared<gtk4_decoration_object_t>(
            deco_toplevel, target, deco_surf, mask_node,
            target->toplevel(), decoration_root_node);
        data->decoration->use_csd = !target->should_be_decorated();
        mask_node->set_children_list({deco_surf});

        target->toplevel()->connect(&on_object_ready);
        // Trigger a new transaction to set margins
        wf::get_core().tx_manager->schedule_object(target->toplevel());

        wf_decorator_manager_send_title_changed(decorator_resource, id, target->get_title().c_str());
        wf_decorator_manager_send_app_id_changed(decorator_resource, id, target->get_app_id().c_str());
        wf::scene::set_node_enabled(decoration_root_node, false);
        do_update_borders(NULL, NULL, target->get_id(), 0, 0, 0, 0);
        auto vg = target->get_geometry();
        wlr_xdg_toplevel_set_size(deco_toplevel, vg.width + 1, vg.height + 1);
    }

    wf::signal::connection_t<wf::view_pre_map_signal> on_pre_map = [=] (wf::view_pre_map_signal *ev)
    {
        if (!decorator_resource)
        {
            return;
        }

        if (ev->view->get_app_id() == "org.wf.sample-decorator")
        {
            ev->override_implementation = true;
            init_decor(ev->view, ev->surface);
            wlr_server_decoration_manager_set_default_mode(
                wf::get_core().protocols.decorator_manager,
                WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
            return;
        }
    };

    wf::signal::connection_t<wf::view_mapped_signal> on_mapped = [=] (wf::view_mapped_signal *ev)
    {
        if (!decorator_resource)
        {
            return;
        }

        if (ev->view->get_app_id() == "org.wf.sample-decorator")
        {
            return;
        }

        if (ev->view->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            LOGD("Not a toplevel");
            return;
        }

        if (wf::toplevel_cast(ev->view)->toplevel()->get_data<gtk4_toplevel_custom_data>())
        {
            LOGD("Already has decoration");
            return;
        }

        LOGD("Need decoration for ", ev->view);
        if (decorator_resource)
        {
            wlr_server_decoration_manager_set_default_mode(
                wf::get_core().protocols.decorator_manager,
                WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);
            wf_decorator_manager_send_create_new_decoration(decorator_resource, ev->view->get_id());

            auto data = wf::toplevel_cast(ev->view)->toplevel()->get_data_safe<gtk4_toplevel_custom_data>();

            auto bg = ev->view->get_bounding_box();
            auto vg = wf::toplevel_cast(ev->view)->get_geometry();
            LOGI(bg);
            LOGI(vg);
            data->margin_offset.x = vg.x - bg.x;
            data->margin_offset.y = vg.y - bg.y;
            LOGD("margin_offsets: ", data->margin_offset.x, ",", data->margin_offset.y);
            wf::scene::set_node_enabled(ev->view->get_root_node(), false);
            wf::scene::set_node_enabled(ev->view->get_root_node(), false);
        }
    };

    wf::signal::connection_t<wf::txn::new_transaction_signal> on_new_tx = [=] (
        wf::txn::new_transaction_signal *ev)
    {
        if (!decorator_resource)
        {
            return;
        }

        auto objs = ev->tx->get_objects();
        for (auto& obj : objs)
        {
            if (auto toplevel = std::dynamic_pointer_cast<wf::toplevel_t>(obj))
            {
                // First check whether the toplevel already has decoration
                // In that case, we should just set the correct margins
                if (auto deco = toplevel->get_data<gtk4_toplevel_custom_data>())
                {
                    // One thing here: we hardcode the deco_margins. Ideally, these should come from a
                    // protocol
                    toplevel->pending().margins =
                        toplevel->pending().fullscreen ? wf::decoration_margins_t{0, 0, 0, 0} : deco_margins;
                    ev->tx->add_object(deco->decoration);
                }
            }
        }
    };

    wf::signal::connection_t<wf::txn::object_ready_signal> on_object_ready =
        [=] (wf::txn::object_ready_signal *ev)
    {
        if (!decorator_resource)
        {
            return;
        }

        auto toplvl = dynamic_cast<wf::toplevel_t*>(ev->self);
        auto deco   = toplvl->get_data_safe<gtk4_toplevel_custom_data>();
        wf::dassert(deco != nullptr, "obj ready for non-decorated toplevel??");
        if (!deco->decoration || !deco->decoration->target_view->get_wlr_surface())
        {
            return;
        }

        if (wlr_xwayland_surface_try_from_wlr_surface(deco->decoration->target_view->get_wlr_surface()))
        {
            deco->decoration->set_final_size(wf::dimensions(toplvl->pending().geometry));
        } else
        {
            deco->decoration->set_final_size(wf::dimensions(toplvl->committed().geometry));
        }
    };

  public:
    void init() override
    {
        decorator_global = wl_global_create(wf::get_core().display,
            &wf_decorator_manager_interface,
            1, NULL, bind_decorator);

        wf::get_core().connect(&on_mapped);
        wf::get_core().connect(&on_pre_map);
        wf::get_core().connect(&on_view_geometry_changed);
        wf::get_core().connect(&on_pointer_button);
        wf::get_core().tx_manager->connect(&on_new_tx);
    }

    void fini() override
    {
        cancel_group_drag();
        finish_morph();
    }
};

DECLARE_WAYFIRE_PLUGIN(gtk4_decoration_plugin);
