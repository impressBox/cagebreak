// Copyright 2025, impressBox. SPDX-License-Identifier: MIT
// wlr-output-management-v1: advertise outputs to clients (xdg-desktop-portal-wlr
// screencast enumeration, wlr-randr) and apply requested output configurations.
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/util/box.h>

#include "output_management.h"
#include "output.h"
#include "server.h"

static void
output_management_update(struct cg_server *server) {
	if(server->output_manager_v1 == NULL) {
		return;
	}
	struct wlr_output_configuration_v1 *config =
	    wlr_output_configuration_v1_create();
	struct cg_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
		    wlr_output_configuration_head_v1_create(config, output->wlr_output);
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
		                          &box);
		if(!wlr_box_empty(&box)) {
			head->state.x = box.x;
			head->state.y = box.y;
		}
	}
	wlr_output_manager_v1_set_configuration(server->output_manager_v1, config);
}

static void
output_management_apply(struct cg_server *server,
                        struct wlr_output_configuration_v1 *config,
                        bool test_only) {
	bool ok = true;
	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *wlr_output = head->state.output;
		wlr_output_enable(wlr_output, head->state.enabled);
		if(head->state.enabled) {
			if(head->state.mode != NULL) {
				wlr_output_set_mode(wlr_output, head->state.mode);
			} else {
				wlr_output_set_custom_mode(
				    wlr_output, head->state.custom_mode.width,
				    head->state.custom_mode.height,
				    head->state.custom_mode.refresh);
			}
		}
		if(!wlr_output_test(wlr_output)) {
			ok = false;
			wlr_output_rollback(wlr_output);
			break;
		}
		if(test_only) {
			wlr_output_rollback(wlr_output);
		} else {
			if(!wlr_output_commit(wlr_output)) {
				ok = false;
			} else if(head->state.enabled) {
				wlr_output_layout_add(server->output_layout, wlr_output,
				                      head->state.x, head->state.y);
			}
		}
	}
	if(ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
	if(!test_only) {
		output_management_update(server);
	}
}

static void
handle_apply(struct wl_listener *listener, void *data) {
	struct cg_server *server =
	    wl_container_of(listener, server, output_manager_apply);
	output_management_apply(server, data, false);
}

static void
handle_test(struct wl_listener *listener, void *data) {
	struct cg_server *server =
	    wl_container_of(listener, server, output_manager_test);
	output_management_apply(server, data, true);
}

static void
handle_layout_change(struct wl_listener *listener, void *data) {
	struct cg_server *server =
	    wl_container_of(listener, server, output_layout_change);
	output_management_update(server);
}

void
output_management_init(struct cg_server *server) {
	server->output_manager_v1 =
	    wlr_output_manager_v1_create(server->wl_display);
	if(server->output_manager_v1 == NULL) {
		return;
	}
	server->output_manager_apply.notify = handle_apply;
	wl_signal_add(&server->output_manager_v1->events.apply,
	              &server->output_manager_apply);
	server->output_manager_test.notify = handle_test;
	wl_signal_add(&server->output_manager_v1->events.test,
	              &server->output_manager_test);
	server->output_layout_change.notify = handle_layout_change;
	wl_signal_add(&server->output_layout->events.change,
	              &server->output_layout_change);
	output_management_update(server);
}
