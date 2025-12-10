// Copyright 2020 - 2024, project-repo and the cagebreak contributors
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include <wlr/config.h>

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#if WLR_HAS_X11_BACKEND
#include <wlr/backend/x11.h>
#endif
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#if CG_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "keybinding.h"
#include "message.h"
#include "output.h"
#include "seat.h"
#include "server.h"
#include "util.h"
#include "view.h"
#include "workspace.h"
#if CG_HAS_XWAYLAND
#include "xwayland.h"
#endif

void
output_clear(struct cg_output *output) {
	struct cg_server *server = output->server;
	wlr_scene_output_destroy(output->scene_output);

	if(server->running && server->curr_output == output &&
	   wl_list_length(&server->outputs) > 1) {
		keybinding_cycle_outputs(server, false, true);
	}

	wl_list_remove(&output->link);

	message_clear(output);

	struct cg_view *view, *view_tmp;
	if(server->running) {
		for(unsigned int i = 0; i < server->nws; ++i) {

			bool first = true;
			for(struct cg_tile *tile = output->workspaces[i]->focused_tile;
			    first || output->workspaces[i]->focused_tile != tile;
			    tile = tile->next) {
				first = false;
				workspace_tile_update_view(tile, NULL);
			}
			struct cg_workspace *ws =
			    server->curr_output
			        ->workspaces[server->curr_output->curr_workspace];
			wl_list_for_each_safe(view, view_tmp, &output->workspaces[i]->views,
			                      link) {
				wl_list_remove(&view->link);
				if(wl_list_empty(&server->outputs)) {
					view->impl->destroy(view);
				} else {
					wl_list_insert(&ws->views, &view->link);
					wlr_scene_node_reparent(&view->scene_tree->node, ws->scene);
					view->workspace = ws;
					view->tile = ws->focused_tile;
					if(server->seat->focused_view == NULL) {
						seat_set_focus(server->seat, view);
					}
				}
			}
			wl_list_for_each_safe(
			    view, view_tmp, &output->workspaces[i]->unmanaged_views, link) {
				wl_list_remove(&view->link);
				if(wl_list_empty(&server->outputs)) {
					view->impl->destroy(view);
				} else {
					wl_list_insert(&ws->unmanaged_views, &view->link);
					wlr_scene_node_reparent(&view->scene_tree->node, ws->scene);
					view->workspace = ws;
					view->tile = ws->focused_tile;
				}
			}
		}
	}
}

int
output_get_num(const struct cg_output *output) {
	struct cg_output *it;
	int count = 1;
	wl_list_for_each(it, &output->server->outputs, link) {
		if(strcmp(output->name, it->name) == 0) {
			return count;
		}
		++count;
	}
	return -1;
}

struct wlr_box
output_get_layout_box(struct cg_output *output) {
	struct wlr_box box;
	wlr_output_layout_get_box(output->server->output_layout, output->wlr_output,
	                          &box);
	if(!output->destroyed && !wlr_box_empty(&box)) {
		output->layout_box.x = box.x;
		output->layout_box.y = box.y;
		output->layout_box.width = box.width;
		output->layout_box.height = box.height;
	}
	return output->layout_box;
}

static void
output_destroy(struct cg_output *output) {
	struct cg_server *server = output->server;
	char *outp_name = strdup(output->name);
	int outp_num = output_get_num(output);

	if(output->destroyed == false) {
		wl_list_remove(&output->destroy.link);
		wl_list_remove(&output->commit.link);
		wl_list_remove(&output->frame.link);
		wlr_scene_output_destroy(output->scene_output);
		output->scene_output = NULL;
	}
	output->destroyed = true;
	enum output_role role = output->role;
	if(role == OUTPUT_ROLE_PERMANENT) {
		output->wlr_output = wlr_headless_add_output(server->headless_backend,
		                                             output->layout_box.width,
		                                             output->layout_box.height);
		output->scene_output =
		    wlr_scene_output_create(server->scene, output->wlr_output);
		struct wlr_output_layout_output *lo =
		    wlr_output_layout_add(server->output_layout, output->wlr_output,
		                          output->layout_box.x, output->layout_box.y);
		wlr_scene_output_layout_add_output(server->scene_output_layout, lo,
		                                   output->scene_output);

	} else {

		output_clear(output);

		for(unsigned int i = 0; i < server->nws; ++i) {
			workspace_free(output->workspaces[i]);
		}
		free(output->workspaces);
		free(output->name);

		free(output);
	}
	if(outp_name != NULL) {
		ipc_send_event(server,
		               "{\"event_name\":\"destroy_output\",\"output\":\"%s\","
		               "\"output_id\":%d,\"permanent\":%d}",
		               outp_name, outp_num, role == OUTPUT_ROLE_PERMANENT);
		free(outp_name);
	} else {
		wlr_log(WLR_ERROR,
		        "Failed to allocate memory for output name in output_destroy");
	}
	if(wl_list_empty(&server->outputs)) {
		wl_display_terminate(server->wl_display);
	}
}

static void
handle_output_destroy(struct wl_listener *listener, void *data) {
	struct cg_output *output = wl_container_of(listener, output, destroy);
	output_destroy(output);
}

void
handle_output_gamma_control_set_gamma(struct wl_listener *listener,
                                      void *data) {
	struct cg_server *server =
	    wl_container_of(listener, server, gamma_control_set_gamma);
	const struct wlr_gamma_control_manager_v1_set_gamma_event *event = data;

	struct wlr_gamma_control_v1 *gamma_control =
	    wlr_gamma_control_manager_v1_get_control(server->gamma_control,
	                                             event->output);

	struct wlr_output_state pending = {0};
	wlr_gamma_control_v1_apply(gamma_control, &pending);
	if(!wlr_output_test_state(event->output, &pending)) {
		wlr_gamma_control_v1_send_failed_and_destroy(gamma_control);
	} else {
		wlr_output_commit_state(event->output, &pending);
		wlr_output_schedule_frame(event->output);
	}
}

static void
handle_output_frame(struct wl_listener *listener, void *data) {
	struct cg_output *output = wl_container_of(listener, output, frame);
	if(!output->wlr_output->enabled) {
		return;
	}
	struct wlr_scene_output *scene_output =
	    wlr_scene_get_scene_output(output->server->scene, output->wlr_output);
	if(scene_output == NULL) {
		return;
	}
	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now = {0};
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

// Parse resolution string in format "WIDTHxHEIGHT" (e.g., "1280x720")
// Returns 0 on success, -1 on failure
static int
parse_resolution(const char *str, int *width, int *height) {
	if (!str || !width || !height) {
		return -1;
	}

	// Skip leading whitespace
	while (*str && isspace(*str)) {
		str++;
	}

	char *endptr;
	long w = strtol(str, &endptr, 10);
	if (endptr == str || w <= 0 || w > 7680) {
		return -1;  // Invalid width
	}

	// Skip to 'x' or 'X'
	while (*endptr && *endptr != 'x' && *endptr != 'X') {
		if (!isspace(*endptr)) {
			return -1;  // Expected 'x' separator
		}
		endptr++;
	}

	if (*endptr == '\0') {
		return -1;  // No 'x' found
	}

	endptr++;  // Skip 'x'

	// Skip whitespace after 'x'
	while (*endptr && isspace(*endptr)) {
		endptr++;
	}

	long h = strtol(endptr, &endptr, 10);
	if (endptr == str || h <= 0 || h > 4320) {
		return -1;  // Invalid height
	}

	// Check for trailing garbage
	while (*endptr) {
		if (!isspace(*endptr)) {
			return -1;  // Trailing garbage
		}
		endptr++;
	}

	*width = (int)w;
	*height = (int)h;
	return 0;
}

static int
output_set_mode(struct wlr_output *output, int width, int height,
                float refresh_rate) {
	int mhz = (int)(refresh_rate * 1000);

	if(wl_list_empty(&output->modes)) {
		wlr_log(WLR_DEBUG, "Assigning custom mode to %s", output->name);
		wlr_output_set_custom_mode(output, width, height,
		                           refresh_rate > 0 ? mhz : 0);
		return 0;
	}

	struct wlr_output_mode *mode, *best = NULL;
	wl_list_for_each(mode, &output->modes, link) {
		if(mode->width == width && mode->height == height) {
			if(mode->refresh == mhz) {
				best = mode;
				break;
			}
			if(best == NULL || mode->refresh > best->refresh) {
				best = mode;
			}
		}
	}
	if(!best) {
		wlr_log(WLR_ERROR, "Configured mode for %s not available",
		        output->name);
		wlr_log(WLR_INFO, "Picking preferred mode instead");
		best = wlr_output_preferred_mode(output);
	} else {
		wlr_log(WLR_DEBUG, "Assigning configured mode to %s", output->name);
	}
	wlr_output_set_mode(output, best);
	wlr_output_commit(output);
	if(!wlr_output_test(output)) {
		wlr_log(WLR_ERROR,
		        "Unable to assign configured mode to %s, picking arbitrary "
		        "available mode",
		        output->name);
		struct wlr_output_mode *mode;
		wl_list_for_each(mode, &output->modes, link) {
			if(mode == best) {
				continue;
			}
			wlr_output_set_mode(output, mode);
			wlr_output_commit(output);
			if(wlr_output_test(output)) {
				break;
			}
		}
		if(!wlr_output_test(output)) {
			return 1;
		}
	}
	return 0;
}

void
output_insert(struct cg_server *server, struct cg_output *output) {
	struct cg_output *it, *prev_it = NULL;
	bool first = true;
	wl_list_for_each(it, &server->outputs, link) {
		if(it->priority < output->priority) {
			if(first == true) {
				wl_list_insert(&server->outputs, &output->link);
			} else {
				wl_list_insert(it->link.prev, &output->link);
			}
			return;
		}
		first = false;
		prev_it = it;
	}
	if(prev_it == NULL) {
		wl_list_insert(&server->outputs, &output->link);
	} else {
		wl_list_insert(&prev_it->link, &output->link);
	}
}

void
output_apply_config(struct cg_server *server, struct cg_output *output,
                    struct cg_output_config *config) {
	struct wlr_output *wlr_output = output->wlr_output;

	struct wlr_box prev_box;
	prev_box.x = output->layout_box.x;
	prev_box.y = output->layout_box.y;
	prev_box.width = output->layout_box.width;
	prev_box.height = output->layout_box.height;
	bool prio_changed = false;
	if(config->role != OUTPUT_ROLE_DEFAULT) {
		output->role = config->role;
		if((output->role == OUTPUT_ROLE_PERIPHERAL) &&
		   (output->destroyed == true)) {
			output_destroy(output);
			wlr_output_destroy(wlr_output);
			return;
		}
	}

	if(config->priority != -1) {
		prio_changed = (output->priority != config->priority);
		output->priority = config->priority;
	}

	if(config->angle != -1) {
		wlr_output_set_transform(wlr_output, config->angle);
	}
	if(config->scale != -1) {
		wlr_log(WLR_INFO, "Setting output scale to %f", config->scale);
		wlr_output_set_scale(wlr_output, config->scale);
	}
	if(config->pos.x == -2) {
		if(output_set_mode(wlr_output, config->pos.width, config->pos.height,
		                   config->refresh_rate) != 0) {
			wlr_log(WLR_ERROR, "Setting output mode failed, disabling output.");
			output_clear(output);
			wl_list_insert(&server->disabled_outputs, &output->link);
			wlr_output_enable(wlr_output, false);
			wlr_output_commit(wlr_output);
			return;
		}
		
		if(wlr_box_empty(&output->layout_box)) {
			int total_width = 0;
			struct wlr_output_layout_output *l_output;
			wl_list_for_each(l_output, &server->output_layout->outputs, link) {
				if(l_output->output != wlr_output) {
					struct wlr_box box;
					wlr_output_layout_get_box(server->output_layout, 
					                          l_output->output, &box);
					if(box.x + box.width > total_width) {
						total_width = box.x + box.width;
					}
				}
			}
			
			struct wlr_output_layout_output *lo =
				wlr_output_layout_add(server->output_layout, wlr_output, 
				                      total_width, 0);
			wlr_scene_output_layout_add_output(server->scene_output_layout, lo,
			                                  output->scene_output);
		}
		
		if(output->workspaces != NULL) {
			wlr_output_layout_get_box(server->output_layout, output->wlr_output,
			                          &output->layout_box);
			
			for(unsigned int i = 0; i < output->server->nws; ++i) {
				workspace_free_tiles(output->workspaces[i]);
				
				struct cg_tile *tile = calloc(1, sizeof(struct cg_tile));
				if(!tile) {
					wlr_log(WLR_ERROR, "Failed to allocate memory for tile");
					continue;
				}
				
				tile->workspace = output->workspaces[i];
				tile->next = tile;
				tile->prev = tile;
				tile->tile.x = 0;  // Start at left edge of the output
				tile->tile.y = 0;  // Top position
				tile->tile.width = config->pos.width;
				tile->tile.height = config->pos.height;
				tile->id = server->tiles_curr_id++;
				
				output->workspaces[i]->focused_tile = tile;
				
				struct cg_view *it_view;
				wl_list_for_each(it_view, &output->workspaces[i]->views, link) {
					view_maximize(it_view, tile);
				}
			}
		}
	} else if(config->pos.x != -1) {
		if(output_set_mode(wlr_output, config->pos.width, config->pos.height,
		                   config->refresh_rate) != 0) {
			wlr_log(WLR_ERROR, "Setting output mode failed, disabling output.");
			output_clear(output);
			wl_list_insert(&server->disabled_outputs, &output->link);
			wlr_output_enable(wlr_output, false);
			wlr_output_commit(wlr_output);
			return;
		}
		if(wlr_box_empty(&output->layout_box)) {
			struct wlr_output_layout_output *lo =
			    wlr_output_layout_add(server->output_layout, wlr_output,
			                          config->pos.x, config->pos.y);
			wlr_scene_output_layout_add_output(server->scene_output_layout, lo,
			                                   output->scene_output);
		} else {
			wlr_scene_output_set_position(output->scene_output, config->pos.x,
			                              config->pos.y);
		}
		if(output->workspaces != NULL) {
			wlr_output_layout_get_box(server->output_layout, output->wlr_output,
			                          &output->layout_box);
			/* Since the size of the output may have changed, we
			 * reinitialize all workspaces with a fullscreen layout */
			if(output->layout_box.width != prev_box.width ||
			   output->layout_box.height != prev_box.height) {
				for(unsigned int i = 0; i < output->server->nws; ++i) {
					output_make_workspace_fullscreen(output, i);
				}
			}
			if(prev_box.x != output->layout_box.x ||
			   prev_box.y != output->layout_box.y) {
				for(unsigned int i = 0; i < server->nws; ++i) {
					struct cg_workspace *ws = output->workspaces[i];
					bool first = true;
					for(struct cg_tile *tile = ws->focused_tile;
					    first || output->workspaces[i]->focused_tile != tile;
					    tile = tile->next) {
						first = false;
						if(tile->view != NULL) {
							wlr_scene_node_set_position(
							    &tile->view->scene_tree->node,
							    tile->view->ox + output->layout_box.x,
							    tile->view->oy + output->layout_box.y);
						}
					}
				}
			}
		}
	} else if(wlr_box_empty(&output->layout_box)) {
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
		// The following two lines make sure that the output is "manually"
		// managed, so that its position doesn't change anymore in the
		// future.
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
		                          &output->layout_box);
		wlr_output_layout_remove(server->output_layout, output->wlr_output);
		struct wlr_output_layout_output *lo =
		    wlr_output_layout_add(server->output_layout, output->wlr_output,
		                          output->layout_box.x, output->layout_box.y);
		wlr_scene_output_layout_add_output(server->scene_output_layout, lo,
		                                   output->scene_output);

		struct wlr_output_mode *preferred_mode =
		    wlr_output_preferred_mode(wlr_output);
		if(preferred_mode) {
			wlr_output_set_mode(wlr_output, preferred_mode);
		}
	}
	/* Refuse to disable the only output */
	if(config->status == OUTPUT_DISABLE &&
	   wl_list_length(&server->outputs) > 1) {
		output_clear(output);
		wl_list_insert(&server->disabled_outputs, &output->link);
		wlr_output_enable(wlr_output, false);
		wlr_output_commit(wlr_output);
	} else {
		if(prio_changed) {
			wl_list_remove(&output->link);
			output_insert(server, output);
		}
		wlr_output_enable(wlr_output, true);
		wlr_output_commit(wlr_output);
	}

	if(output->bg != NULL) {
		wlr_scene_node_destroy(&output->bg->node);
		output->bg = NULL;
	}
	struct wlr_scene_output *scene_output =
	    wlr_scene_get_scene_output(output->server->scene, output->wlr_output);
	if(scene_output == NULL) {
		return;
	}
	output->bg = wlr_scene_rect_create(
	    &scene_output->scene->tree, output->wlr_output->width,
	    output->wlr_output->height, server->bg_color);
	wlr_scene_node_set_position(&output->bg->node, scene_output->x,
	                            scene_output->y);
	wlr_scene_node_lower_to_bottom(&output->bg->node);
}

struct cg_output_config *
empty_output_config(void) {
	struct cg_output_config *cfg = calloc(1, sizeof(struct cg_output_config));
	if(cfg == NULL) {
		wlr_log(WLR_ERROR, "Could not allocate output configuration.");
		return NULL;
	}

	cfg->status = OUTPUT_DEFAULT;
	cfg->role = OUTPUT_ROLE_DEFAULT;
	cfg->pos.x = -1;
	cfg->pos.y = -1;
	cfg->pos.width = -1;
	cfg->pos.height = -1;
	cfg->output_name = NULL;
	cfg->refresh_rate = 0;
	cfg->priority = -1;
	cfg->scale = -1;
	cfg->angle = -1;

	return cfg;
}

/* cfg1 has precedence over cfg2 */
struct cg_output_config *
merge_output_configs(struct cg_output_config *cfg1,
                     struct cg_output_config *cfg2) {
	struct cg_output_config *out_cfg = empty_output_config();
	if(cfg1->status == out_cfg->status) {
		out_cfg->status = cfg2->status;
	} else {
		out_cfg->status = cfg1->status;
	}
	if(cfg1->role == out_cfg->role) {
		out_cfg->role = cfg2->role;
	} else {
		out_cfg->role = cfg1->role;
	}
	if(cfg1->pos.x == out_cfg->pos.x) {
		out_cfg->pos.x = cfg2->pos.x;
		out_cfg->pos.y = cfg2->pos.y;
		out_cfg->pos.width = cfg2->pos.width;
		out_cfg->pos.height = cfg2->pos.height;
	} else {
		out_cfg->pos.x = cfg1->pos.x;
		out_cfg->pos.y = cfg1->pos.y;
		out_cfg->pos.width = cfg1->pos.width;
		out_cfg->pos.height = cfg1->pos.height;
	}
	if(cfg1->output_name == NULL) {
		if(cfg2->output_name == NULL) {
			out_cfg->output_name = NULL;
		} else {
			out_cfg->output_name = strdup(cfg2->output_name);
		}
	} else {
		out_cfg->output_name = strdup(cfg1->output_name);
	}
	if(cfg1->refresh_rate == out_cfg->refresh_rate) {
		out_cfg->refresh_rate = cfg2->refresh_rate;
	} else {
		out_cfg->refresh_rate = cfg1->refresh_rate;
	}
	if(cfg1->priority == out_cfg->priority) {
		out_cfg->priority = cfg2->priority;
	} else {
		out_cfg->priority = cfg1->priority;
	}
	if(cfg1->scale == out_cfg->scale) {
		out_cfg->scale = cfg2->scale;
	} else {
		out_cfg->scale = cfg1->scale;
	}
	if(cfg1->angle == out_cfg->angle) {
		out_cfg->angle = cfg2->angle;
	} else {
		out_cfg->angle = cfg1->angle;
	}
	return out_cfg;
}

void
output_configure(struct cg_server *server, struct cg_output *output) {
	struct cg_output_config *tot_config = empty_output_config();
	struct cg_output_config *config;
	wl_list_for_each(config, &server->output_config, link) {
		if(strcmp(config->output_name, output->name) == 0) {
			if(tot_config == NULL) {
				return;
			}
			struct cg_output_config *prev_config = tot_config;
			tot_config = merge_output_configs(config, tot_config);
			if(prev_config->output_name != NULL) {
				free(prev_config->output_name);
			}
			free(prev_config);
		}
	}
	if(tot_config != NULL) {
		output_apply_config(server, output, tot_config);
	}
	free(tot_config->output_name);
	free(tot_config);
}

static void
handle_output_commit(struct wl_listener *listener, void *data) {
	struct cg_output *output = wl_container_of(listener, output, commit);
	struct wlr_output_event_commit *event = data;

	if(!output->wlr_output->enabled || output->workspaces == NULL) {
		return;
	}

	if(event->state->committed &
	   (WLR_OUTPUT_STATE_TRANSFORM | WLR_OUTPUT_STATE_SCALE |
	    WLR_OUTPUT_STATE_MODE)) {
		struct cg_view *view;
		wl_list_for_each(
		    view, &output->workspaces[output->curr_workspace]->views, link) {
			if(view_is_visible(view)) {
				view_maximize(view, view->tile);
			}
		}
	}
}

void
output_make_workspace_fullscreen(struct cg_output *output, int ws) {
	struct cg_server *server = output->server;
	struct cg_view *current_view = output->workspaces[ws]->focused_tile->view;

	if(current_view == NULL) {
		struct cg_view *it = NULL;
		wl_list_for_each(it, &output->workspaces[ws]->views, link) {
			if(view_is_visible(it)) {
				current_view = it;
				break;
			}
		}
	}

	workspace_free_tiles(output->workspaces[ws]);
	if(full_screen_workspace_tiles(server->output_layout,
	                               output->workspaces[ws],
	                               &server->tiles_curr_id) != 0) {
		wlr_log(WLR_ERROR, "Failed to allocate space for fullscreen workspace");
		return;
	}

	struct cg_view *it_view;
	wl_list_for_each(it_view, &output->workspaces[ws]->views, link) {
		it_view->tile = output->workspaces[ws]->focused_tile;
	}

	workspace_tile_update_view(output->workspaces[ws]->focused_tile,
	                           current_view);
	if((ws == output->curr_workspace) && (output == server->curr_output)) {
		seat_set_focus(server->seat, current_view);
	}
}

void
handle_new_output(struct wl_listener *listener, void *data) {
	struct cg_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	wlr_output_enable(wlr_output, true);

	if(!wlr_output_init_render(wlr_output, server->allocator,
	                           server->renderer)) {
		wlr_log(WLR_ERROR, "Failed to initialize output rendering");
		return;
	}

	const char *display_mode = getenv("IMPRESSBOX_DISPLAY_MODE");
	char mode = 'D';  // Default

	if(display_mode != NULL && strlen(display_mode) > 0) {
		mode = toupper(display_mode[0]);
		// Validate: only V, H, or D are valid
		if(mode != 'V' && mode != 'H' && mode != 'D') {
			mode = 'D';  // Invalid value, default to D
		}
	}

	if(mode == 'V') {
		server->extended_mode = true;
		server->extended_horizontal = false;
	} else if(mode == 'H') {
		server->extended_mode = true;
		server->extended_horizontal = true;
	} else {
		server->extended_mode = false;
		server->extended_horizontal = false;
	}

	if (server->extended_mode) {
		const char *name = wlr_output->name;
		wlr_log(WLR_DEBUG, "EXTENDED MODE ACTIVE for output %s", name);
	
		bool is_hdma1 = strcasecmp(name, "HDMA1") == 0 ||
						strcasecmp(name, "HDMI-A-1") == 0;
		bool is_hdma2 = strcasecmp(name, "HDMA2") == 0 ||
						strcasecmp(name, "HDMI-A-2") == 0;
	
		if (!is_hdma1 && !is_hdma2) {
			wlr_log(WLR_DEBUG, "EXTENDED: ignoring non-wall output %s", name);
			wlr_output_enable(wlr_output, false);
			wlr_output_commit(wlr_output);
			return;
		}
	
		// Parse environment variables for resolutions and refresh rate
		// Only parse once (on first output)
		static bool resolution_parsed = false;
		if (!resolution_parsed) {
			// Parse refresh rate (default 60.0)
			const char *hz_str = getenv("IMPRESSBOX_DM_HZ");
			if (hz_str) {
				char *endptr;
				float hz = strtof(hz_str, &endptr);
				if (endptr != hz_str && hz > 0.0f && hz <= 240.0f) {
					server->extended_resolution.refresh_rate = hz;
				} else {
					wlr_log(WLR_ERROR, "EXTENDED: Invalid IMPRESSBOX_DM_HZ value '%s', using default 60.0", hz_str);
					server->extended_resolution.refresh_rate = 60.0f;
				}
			} else {
				server->extended_resolution.refresh_rate = 60.0f;
			}

			// Parse HDMI1 resolution
			const char *hdmi1_str = getenv("IMPRESSBOX_DM_HDMI1");
			if (hdmi1_str) {
				if (parse_resolution(hdmi1_str, &server->extended_resolution.hdmi1_width,
				                     &server->extended_resolution.hdmi1_height) == 0) {
					server->extended_resolution.hdmi1_configured = true;
					wlr_log(WLR_INFO, "EXTENDED: HDMI1 resolution set to %dx%d",
					        server->extended_resolution.hdmi1_width,
					        server->extended_resolution.hdmi1_height);
				} else {
					wlr_log(WLR_ERROR, "EXTENDED: Invalid IMPRESSBOX_DM_HDMI1 format '%s', will use preferred mode", hdmi1_str);
					server->extended_resolution.hdmi1_configured = false;
				}
			} else {
				server->extended_resolution.hdmi1_configured = false;
				wlr_log(WLR_INFO, "EXTENDED: IMPRESSBOX_DM_HDMI1 not set, will use preferred mode");
			}

			// Parse HDMI2 resolution
			const char *hdmi2_str = getenv("IMPRESSBOX_DM_HDMI2");
			if (hdmi2_str) {
				if (parse_resolution(hdmi2_str, &server->extended_resolution.hdmi2_width,
				                     &server->extended_resolution.hdmi2_height) == 0) {
					server->extended_resolution.hdmi2_configured = true;
					wlr_log(WLR_INFO, "EXTENDED: HDMI2 resolution set to %dx%d",
					        server->extended_resolution.hdmi2_width,
					        server->extended_resolution.hdmi2_height);
				} else {
					wlr_log(WLR_ERROR, "EXTENDED: Invalid IMPRESSBOX_DM_HDMI2 format '%s', will use preferred mode", hdmi2_str);
					server->extended_resolution.hdmi2_configured = false;
				}
			} else {
				server->extended_resolution.hdmi2_configured = false;
				wlr_log(WLR_INFO, "EXTENDED: IMPRESSBOX_DM_HDMI2 not set, will use preferred mode");
			}

			resolution_parsed = true;
		}

		// Determine resolution for this output
		int output_width, output_height;
		if (is_hdma1) {
			if (server->extended_resolution.hdmi1_configured) {
				output_width = server->extended_resolution.hdmi1_width;
				output_height = server->extended_resolution.hdmi1_height;
			} else {
				// Use preferred mode
				struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr_output);
				if (preferred) {
					output_width = preferred->width;
					output_height = preferred->height;
					// Store for later use
					server->extended_resolution.hdmi1_width = output_width;
					server->extended_resolution.hdmi1_height = output_height;
					wlr_log(WLR_INFO, "EXTENDED: HDMI1 using preferred mode %dx%d",
					        output_width, output_height);
				} else {
					wlr_log(WLR_ERROR, "EXTENDED: No preferred mode available for %s", name);
			wlr_output_enable(wlr_output, false);
			wlr_output_commit(wlr_output);
			return;
		}
			}
		} else {  // HDMI2
			if (server->extended_resolution.hdmi2_configured) {
				output_width = server->extended_resolution.hdmi2_width;
				output_height = server->extended_resolution.hdmi2_height;
			} else {
				// Use preferred mode
				struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr_output);
				if (preferred) {
					output_width = preferred->width;
					output_height = preferred->height;
					// Store for later use
					server->extended_resolution.hdmi2_width = output_width;
					server->extended_resolution.hdmi2_height = output_height;
					wlr_log(WLR_INFO, "EXTENDED: HDMI2 using preferred mode %dx%d",
					        output_width, output_height);
				} else {
					wlr_log(WLR_ERROR, "EXTENDED: No preferred mode available for %s", name);
					wlr_output_enable(wlr_output, false);
					wlr_output_commit(wlr_output);
					return;
				}
			}
		}
	
		// Set output mode
		if (output_set_mode(wlr_output, output_width, output_height,
		                    server->extended_resolution.refresh_rate) != 0) {
			wlr_log(WLR_ERROR, "EXTENDED: failed to set mode %dx%d@%.2fHz for %s",
			        output_width, output_height,
			        server->extended_resolution.refresh_rate, name);
			wlr_output_enable(wlr_output, false);
			wlr_output_commit(wlr_output);
			return;
		}
		
	
		static int next_x = 0;  // For horizontal: first at x=0, second at x=1680
		static int next_y = 0; // first output at y=0, second at y=1050
	
		struct cg_output *output = calloc(1, sizeof(struct cg_output));
		if (!output) {
			wlr_log(WLR_ERROR, "EXTENDED: failed to allocate output");
			return;
		}

		output->server = server;
		output->wlr_output = wlr_output;
		output->name = strdup(name);
		output->destroyed = false;
		//output->workspaces = NULL; // we’ll handle workspaces later
		wl_list_init(&output->messages);
	
		output->scene_output = wlr_scene_output_create(server->scene, wlr_output);

		if (!output->scene_output) {
			wlr_log(WLR_ERROR, "EXTENDED: failed to create scene output");
			free(output->name);
			free(output);
			return;
		}

		// Create the global wall scene once (shared by both outputs)
		if (!server->extended_wall_scene) {
			server->extended_wall_scene = wlr_scene_tree_create(&server->scene->tree);
			if (!server->extended_wall_scene) {
				wlr_log(WLR_ERROR, "EXTENDED: failed to create wall scene");
				return;
			}
			wlr_log(WLR_DEBUG, "EXTENDED: created global wall scene for spanning views");
		}

		// Calculate wall dimensions based on configured resolutions
		// Only create/update wall background when we have both resolutions
		static struct wlr_scene_rect *wall_bg = NULL;
		if (server->extended_resolution.hdmi1_width > 0 && server->extended_resolution.hdmi1_height > 0 &&
		    server->extended_resolution.hdmi2_width > 0 && server->extended_resolution.hdmi2_height > 0) {
			int wall_width, wall_height;
			if (server->extended_horizontal) {
				// Horizontal: outputs side-by-side
				wall_width = server->extended_resolution.hdmi1_width + server->extended_resolution.hdmi2_width;
				// Use maximum height (in case they differ)
				wall_height = server->extended_resolution.hdmi1_height > server->extended_resolution.hdmi2_height ?
				              server->extended_resolution.hdmi1_height : server->extended_resolution.hdmi2_height;
			} else {
				// Vertical: outputs stacked
				// Use maximum width (in case they differ)
				wall_width = server->extended_resolution.hdmi1_width > server->extended_resolution.hdmi2_width ?
				             server->extended_resolution.hdmi1_width : server->extended_resolution.hdmi2_width;
				wall_height = server->extended_resolution.hdmi1_height + server->extended_resolution.hdmi2_height;
			}

			// Create or update wall background (destroy old one if exists)
			if (wall_bg) {
				wlr_scene_node_destroy(&wall_bg->node);
			}
			wall_bg = wlr_scene_rect_create(
				&server->scene->tree,
				wall_width, wall_height,
				server->bg_color);
			if (wall_bg) {
			wlr_scene_node_set_position(&wall_bg->node, 0, 0);
			wlr_scene_node_lower_to_bottom(&wall_bg->node);
				wlr_log(WLR_DEBUG, "EXTENDED: wall background %dx%d", wall_width, wall_height);
			}
		}
		
		// Reset positioning counters for this output setup
		// Reset positioning counters when mode changes
		static bool last_horizontal = false;
		static bool first_call = true;

		if (first_call || (last_horizontal != server->extended_horizontal)) {
			next_x = 0;
			next_y = 0;
			last_horizontal = server->extended_horizontal;
			first_call = false;
		}

		int pos_x, pos_y;
		if (server->extended_horizontal) {
			pos_x = next_x;
			pos_y = 0;
			wlr_log(WLR_DEBUG, "EXTENDED: %s positioning - next_x=%d, output_width=%d, pos_x=%d", 
			        name, next_x, output_width, pos_x);
			next_x += output_width;  // Increment by current output's width
		} else {
			pos_x = 0;
			pos_y = next_y;
			wlr_log(WLR_DEBUG, "EXTENDED: %s positioning - next_y=%d, output_height=%d, pos_y=%d", 
			        name, next_y, output_height, pos_y);
			next_y += output_height;  // Increment by current output's height
		}

		wlr_log(WLR_DEBUG, "EXTENDED: %s calling wlr_output_layout_add with pos_x=%d, pos_y=%d", 
		        name, pos_x, pos_y);
		struct wlr_output_layout_output *lo =
			wlr_output_layout_add(server->output_layout, wlr_output, pos_x, pos_y);
	
		wlr_scene_output_layout_add_output(server->scene_output_layout, lo,
										   output->scene_output);
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
								  &output->layout_box);
	
		output_insert(server, output);
		if (server->curr_output == NULL) {
			server->curr_output = output;
		}
	
		output->destroy.notify = handle_output_destroy;
		wl_signal_add(&wlr_output->events.destroy, &output->destroy);
		output->frame.notify = handle_output_frame;
		wl_signal_add(&wlr_output->events.frame, &output->frame);
		output->commit.notify = handle_output_commit;
		wl_signal_add(&wlr_output->events.commit, &output->commit);
	
		wlr_output_enable(wlr_output, true);
		wlr_output_commit(wlr_output);
	
		wlr_log(WLR_DEBUG, "EXTENDED: %s placed at (%d,%d)", name,
				output->layout_box.x, output->layout_box.y);

		output->bg = wlr_scene_rect_create(
			&server->scene->tree,
			1, 1,  // Tiny 1x1 pixel rect, won't be visible
			server->bg_color);

		wlr_scene_node_set_position(&output->bg->node, 0, 0);
		wlr_scene_node_lower_to_bottom(&output->bg->node);

		output->workspaces =
		malloc(server->nws * sizeof(struct cg_workspace *));
		if (!output->workspaces) {
			wlr_log(WLR_ERROR, "EXTENDED: failed to allocate workspaces");
			return;
		}

		for (unsigned int i = 0; i < server->nws; ++i) {
			output->workspaces[i] = full_screen_workspace(output);
			if (!output->workspaces[i]) {
				wlr_log(WLR_ERROR, "EXTENDED: failed to create workspace %u", i);
				continue;
			}
			output->workspaces[i]->num = i;
			wl_list_init(&output->workspaces[i]->views);
			wl_list_init(&output->workspaces[i]->unmanaged_views);
		}

		// Focus the first workspace
		workspace_focus(output, 0);
		
		return;
	} else {
    	wlr_log(WLR_INFO, "EXTENDED MODE = FALSE");
	}

	struct cg_output *ito;
	bool reinit = false;
	struct cg_output *output = NULL;
	wl_list_for_each(ito, &server->outputs, link) {
		if((strcmp(ito->name, wlr_output->name) == 0) && (ito->destroyed)) {
			reinit = true;
			output = ito;
		}
	}
	if(reinit) {
		wlr_output_layout_remove(server->output_layout, output->wlr_output);
		wlr_scene_output_destroy(output->scene_output);
	} else {
		output = calloc(1, sizeof(struct cg_output));
	}
	if(!output) {
		wlr_log(WLR_ERROR, "Failed to allocate output");
		return;
	}
	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);

	output->wlr_output = wlr_output;
	output->destroyed = false;

	if(!reinit) {
		output->server = server;
		output->name = strdup(wlr_output->name);

		struct cg_output_priorities *it;
		int prio = -1;
		wl_list_for_each(it, &server->output_priorities, link) {
			if(strcmp(output->name, it->ident) == 0) {
				prio = it->priority;
			}
		}
		output->priority = prio;
		wlr_output_set_transform(wlr_output, WL_OUTPUT_TRANSFORM_NORMAL);
		output->workspaces = NULL;

		wl_list_init(&output->messages);

		if(!wlr_xcursor_manager_load(server->seat->xcursor_manager,
		                             wlr_output->scale)) {
			wlr_log(WLR_ERROR,
			        "Cannot load XCursor theme for output '%s' with scale %f",
			        output->name, wlr_output->scale);
		}

		output_insert(server, output);
		// This is duplicated here only as a cue for the static analysis tool
		output->destroyed = false;
		output_configure(server, output);
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
		                          &output->layout_box);

		output->workspaces =
		    malloc(server->nws * sizeof(struct cg_workspace *));
		for(unsigned int i = 0; i < server->nws; ++i) {
			output->workspaces[i] = full_screen_workspace(output);
			if(!output->workspaces[i]) {
				wlr_log(WLR_ERROR, "Failed to allocate workspaces for output");
				return;
			}
			output->workspaces[i]->num = i;
			wl_list_init(&output->workspaces[i]->views);
			wl_list_init(&output->workspaces[i]->unmanaged_views);
		}

		wlr_scene_node_raise_to_top(&output->workspaces[0]->scene->node);
		workspace_focus(output, 0);

		/* We are the first output. Set the current output to this one. */
		if(server->curr_output == NULL) {
			server->curr_output = output;
		}
	} else {
		struct wlr_output_layout_output *lo =
		    wlr_output_layout_add(server->output_layout, wlr_output,
		                          output->layout_box.x, output->layout_box.y);
		wlr_scene_output_layout_add_output(server->scene_output_layout, lo,
		                                   output->scene_output);

		struct wlr_output_mode *preferred_mode =
		    wlr_output_preferred_mode(wlr_output);
		if(preferred_mode) {
			wlr_output_set_mode(wlr_output, preferred_mode);
		}
		wlr_output_set_transform(wlr_output, WL_OUTPUT_TRANSFORM_NORMAL);
		wlr_scene_output_set_position(
		    output->scene_output, output->layout_box.x, output->layout_box.y);
		wlr_output_enable(wlr_output, true);
		wlr_output_commit(wlr_output);
		output_configure(server, output);
		output_get_layout_box(output);
	}

	wlr_cursor_set_xcursor(server->seat->cursor, server->seat->xcursor_manager,
	                       DEFAULT_XCURSOR);
	wlr_cursor_warp(server->seat->cursor, NULL, 0, 0);

	output->destroy.notify = handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	output->frame.notify = handle_output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->commit.notify = handle_output_commit;
	wl_signal_add(&wlr_output->events.commit, &output->commit);

	ipc_send_event(server,
	               "{\"event_name\":\"new_output\",\"output\":\"%s\",\"output_"
	               "id\":%d,\"priority\":%d,\"restart\":%d}",
	               output->name, output_get_num(output), output->priority,
	               reinit);
}
