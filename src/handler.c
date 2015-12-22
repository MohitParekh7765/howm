#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xproto.h>

#include "client.h"
#include "handler.h"
#include "helper.h"
#include "howm.h"
#include "layout.h"
#include "monitor.h"
#include "types.h"
#include "workspace.h"
#include "xcb_help.h"

/**
 * @file handler.c
 *
 * @author Harvey Hunt
 *
 * @date 2015
 *
 * @brief Handle the X events generated by clients that howm is managing.
 */

static void enter_event(xcb_generic_event_t *ev);
static void destroy_event(xcb_generic_event_t *ev);
static void button_press_event(xcb_generic_event_t *ev);
static void map_event(xcb_generic_event_t *ev);
static void configure_event(xcb_generic_event_t *ev);
static void unmap_event(xcb_generic_event_t *ev);
static void client_message_event(xcb_generic_event_t *ev);
static void unhandled_event(xcb_generic_event_t *ev);

/**
 * @brief Process a button press.
 *
 * @param ev The button press event.
 */
static void button_press_event(xcb_generic_event_t *ev)
{
	/* FIXME: be->event doesn't seem to match with any windows managed by howm.*/
	xcb_button_press_event_t *be = (xcb_button_press_event_t *)ev;

	log_info("Button %d pressed at (%d, %d)", be->detail, be->event_x, be->event_y);
	if (conf.focus_mouse_click && be->detail == XCB_BUTTON_INDEX_1)
		focus_window(be->event);

	if (conf.focus_mouse_click) {
		xcb_allow_events(dpy, XCB_ALLOW_REPLAY_POINTER, be->time);
		xcb_flush(dpy);
	}
}

/**
 * @brief Handles mapping requests.
 *
 * When an X window wishes to be displayed, it send a mapping request. This
 * function processes that mapping request and inserts the new client (created
 * from the map requesting window) into the list of clients for the current
 * workspace.
 *
 * @param ev A mapping request event.
 */
static void map_event(xcb_generic_event_t *ev)
{
	xcb_window_t transient = 0;
	xcb_get_geometry_reply_t *geom;
	xcb_get_window_attributes_reply_t *wa;
	xcb_map_request_event_t *me = (xcb_map_request_event_t *)ev;
	xcb_ewmh_get_atoms_reply_t type;
	unsigned int i;
	client_t *c;

	wa = xcb_get_window_attributes_reply(dpy, xcb_get_window_attributes(dpy, me->window), NULL);
	if (!wa || wa->override_redirect || find_client_by_win(me->window)) {
		free(wa);
		return;
	}
	free(wa);

	log_info("Mapping request for window <0x%x>", me->window);

	c = create_client(me->window);

	if (xcb_ewmh_get_wm_window_type_reply(ewmh,
				xcb_ewmh_get_wm_window_type(ewmh, me->window),
				&type, NULL) == 1) {
		for (i = 0; i < type.atoms_len; i++) {
			xcb_atom_t a = type.atoms[i];

			if (a == ewmh->_NET_WM_WINDOW_TYPE_DOCK
				|| a == ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR) {
				xcb_map_window(dpy, c->win);
				remove_client(c, false);
				return;
			} else if (a == ewmh->_NET_WM_WINDOW_TYPE_NOTIFICATION
				|| a == ewmh->_NET_WM_WINDOW_TYPE_DROPDOWN_MENU
				|| a == ewmh->_NET_WM_WINDOW_TYPE_SPLASH
				|| a == ewmh->_NET_WM_WINDOW_TYPE_POPUP_MENU
				|| a == ewmh->_NET_WM_WINDOW_TYPE_TOOLTIP
				|| a == ewmh->_NET_WM_WINDOW_TYPE_DIALOG) {
				c->is_floating = true;
			}
		}
	}

	/* Assume that transient windows MUST float. */
	xcb_icccm_get_wm_transient_for_reply(dpy, xcb_icccm_get_wm_transient_for_unchecked(dpy, me->window), &transient, NULL);
	c->is_transient = transient ? true : false;
	if (c->is_transient)
		c->is_floating = true;

	geom = xcb_get_geometry_reply(dpy, xcb_get_geometry_unchecked(dpy, me->window), NULL);
	if (geom) {
		log_info("Mapped client's initial geom is %ux%u+%d+%d", geom->width, geom->height, geom->x, geom->y);
		if (c->is_floating) {
			c->rect.width = geom->width > 1 ? geom->width : conf.float_spawn_width;
			c->rect.height = geom->height > 1 ? geom->height : conf.float_spawn_height;
			c->rect.x = conf.center_floating ? (mon->rect.width / 2) - (c->rect.width / 2) : geom->x;
			c->rect.y = conf.center_floating ? (mon->rect.height - mon->ws->bar_height - c->rect.height) / 2 : geom->y;
		}
		free(geom);
	}

	arrange_windows();
	xcb_map_window(dpy, c->win);
	update_focused_client(c);
	grab_buttons(c);
}

/**
 * @brief The handler for destroy events.
 *
 * Used when a window sends a destroy event, signalling that it wants to be
 * unmapped. The client that the window belongs to is then removed from the
 * client list for its repective workspace.
 *
 * @param ev The destroy event.
 */
static void destroy_event(xcb_generic_event_t *ev)
{
	xcb_destroy_notify_event_t *de = (xcb_destroy_notify_event_t *)ev;
	client_t *c = find_client_by_win(de->window);

	if (!c)
		return;
	log_info("Client <%p> wants to be destroyed", c);
	remove_client(c, true);
	arrange_windows();
}

/**
 * @brief The event that occurs when the mouse pointer enters a window.
 *
 * @param ev The enter event.
 */
static void enter_event(xcb_generic_event_t *ev)
{
	xcb_enter_notify_event_t *ee = (xcb_enter_notify_event_t *)ev;
	/* TODO: Maybe this needs to go into a motion event, as we might not be
	 * able to focus another monitor without there being a window there?
	 */
	xcb_point_t point = {ee->root_x, ee->root_y};

	log_debug("Enter event for window <0x%x>", ee->event);

	focus_monitor(point_to_monitor(point));

	if (conf.focus_mouse && mon->ws->layout != ZOOM)
		focus_window(ee->event);
}

/**
 * @brief Deal with a window's request to change its geometry.
 *
 * @param ev The event sent from the window.
 */
static void configure_event(xcb_generic_event_t *ev)
{
	xcb_configure_request_event_t *ce = (xcb_configure_request_event_t *)ev;
	uint32_t vals[7] = {0}, i = 0;

	log_info("Received configure request for window <0x%x>", ce->window);

	/* TODO: Need to test whether gaps etc need to be taken into account
	 * here. */
	if (XCB_CONFIG_WINDOW_X & ce->value_mask)
		vals[i++] = ce->x;
	if (XCB_CONFIG_WINDOW_Y & ce->value_mask)
		vals[i++] = ce->y + (conf.bar_bottom ? 0 : mon->ws->bar_height);
	if (XCB_CONFIG_WINDOW_WIDTH & ce->value_mask)
		vals[i++] = (ce->width < mon->rect.width - conf.border_px) ? ce->width : mon->rect.width - conf.border_px;
	if (XCB_CONFIG_WINDOW_HEIGHT & ce->value_mask)
		vals[i++] = (ce->height < mon->rect.height - conf.border_px) ? ce->height : mon->rect.height - conf.border_px;
	if (XCB_CONFIG_WINDOW_BORDER_WIDTH & ce->value_mask)
		vals[i++] = ce->border_width;
	if (XCB_CONFIG_WINDOW_SIBLING & ce->value_mask)
		vals[i++] = ce->sibling;
	if (XCB_CONFIG_WINDOW_STACK_MODE & ce->value_mask)
		vals[i++] = ce->stack_mode;
	xcb_configure_window(dpy, ce->window, ce->value_mask, vals);
	arrange_windows();
}

/**
 * @brief Remove clients that wish to be unmapped.
 *
 * @param ev An event letting us know which client should be unmapped.
 */
static void unmap_event(xcb_generic_event_t *ev)
{
	xcb_unmap_notify_event_t *ue = (xcb_unmap_notify_event_t *)ev;
	client_t *c = find_client_by_win(ue->window);

	if (!c)
		return;
	log_info("Received unmap request for client <%p>", c);

	if (ue->event != screen->root) {
		remove_client(c, true);
		arrange_windows();
	}
	howm_info();
}

/**
 * @brief Handle messages sent by the client to alter its state.
 *
 * @param ev The client message as a generic event.
 */
static void client_message_event(xcb_generic_event_t *ev)
{
	xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
	client_t *c = find_client_by_win(cm->window);

	if (c && cm->type == ewmh->_NET_WM_STATE) {
		ewmh_process_wm_state(c, (xcb_atom_t) cm->data.data32[1], cm->data.data32[0]);
		if (cm->data.data32[2])
			ewmh_process_wm_state(c, (xcb_atom_t) cm->data.data32[2], cm->data.data32[0]);
	} else if (c && cm->type == ewmh->_NET_CLOSE_WINDOW) {
		log_info("_NET_CLOSE_WINDOW: Removing client <%p>", c);
		remove_client(c, true);
		arrange_windows();
	} else if (c && cm->type == ewmh->_NET_ACTIVE_WINDOW) {
		log_info("_NET_ACTIVE_WINDOW: Focusing client <%p>", c);
		update_focused_client(find_client_by_win(cm->window));
	} else if (c && cm->type == ewmh->_NET_CURRENT_DESKTOP
			&& cm->data.data32[0] < mon->workspace_cnt) {
		log_info("_NET_CURRENT_DESKTOP: Changing to workspace <%d>", cm->data.data32[0]);
		change_ws(index_to_workspace(mon, cm->data.data32[0]));
	} else {
		log_debug("Unhandled client message: %d", cm->type);
	}
}

static void unhandled_event(xcb_generic_event_t *ev)
{
	/* If we have a LOG_LEVEL higher than LOG_DEBUG, then we will
	 * get compiler warnings about ev not being used. */
	UNUSED(ev);
	log_debug("Unhandled event: %d", ev->response_type & ~0x80);
}

void handle_event(xcb_generic_event_t *ev)
{
	switch (ev->response_type & ~0x80) {
	case XCB_BUTTON_PRESS:
		button_press_event(ev);
		break;
	case XCB_MAP_REQUEST:
		map_event(ev);
		break;
	case XCB_DESTROY_NOTIFY:
		destroy_event(ev);
		break;
	case XCB_ENTER_NOTIFY:
		enter_event(ev);
		break;
	case XCB_CONFIGURE_NOTIFY:
		configure_event(ev);
		break;
	case XCB_UNMAP_NOTIFY:
		unmap_event(ev);
		break;
	case XCB_CLIENT_MESSAGE:
		client_message_event(ev);
		break;
	default:
		unhandled_event(ev);
		break;
	}
}
