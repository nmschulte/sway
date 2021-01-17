#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "swaybar/bar.h"
#include "swaybar/config.h"
#include "swaybar/tray/icon.h"
#include "swaybar/tray/item.h"
#include "swaybar/tray/menu.h"
#include "swaybar/tray/tray.h"
#include "background-image.h"
#include "cairo.h"
#include "list.h"
#include "log.h"
#include "pango.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"


/* MENU */

static void close_popup(struct swaybar_popup *popup);
static void open_popup_id(struct swaybar_sni *sni, int id);

static const char *menu_interface = "com.canonical.dbusmenu";

void destroy_menu(struct swaybar_menu_item *menu) {
	if (!menu) {
		return;
	}

	struct swaybar_popup *popup = menu->sni->tray->popup;
	if (popup && popup->sni == menu->sni && popup->popup_surface) {
		sway_log(SWAY_DEBUG, "closing popup due to destroy_menu");
		close_popup(popup);
	}

	free(menu->label);
	free(menu->icon_name);
	cairo_surface_destroy(menu->icon_data);
	if (menu->children) {
		for (int i = 0; i < menu->children->length; ++i) {
			destroy_menu(menu->children->items[i]);
		}
		list_free(menu->children);
	}
	free(menu);
}

static struct swaybar_menu_item **menu_find_item(struct swaybar_menu_item **root,
		int id) {
	struct swaybar_menu_item *item = *root;
	if (item->id == id) {
		return root;
	}

	if (item->children) {
		for (int i = 0; i < item->children->length; ++i) {
			struct swaybar_menu_item *child = item->children->items[i];
			struct swaybar_menu_item **res = menu_find_item(&child, id);
			if (res) {
				return res;
			}
		}
	}
	return NULL;
}

struct png_stream {
	const void *data;
	size_t left;
};

static cairo_status_t read_png_stream(void *closure, unsigned char *data,
		unsigned int length) {
	struct png_stream *png_stream = closure;
	if (length > png_stream->left) {
		return CAIRO_STATUS_READ_ERROR;
	}
	memcpy(data, png_stream->data, length);
	png_stream->data += length;
	png_stream->left -= length;
	return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *read_png(const void *data, size_t data_size) {
	struct png_stream *png_stream = malloc(sizeof(struct png_stream));
	png_stream->data = data;
	png_stream->left = data_size;
	cairo_surface_t *surface =
		cairo_image_surface_create_from_png_stream(read_png_stream, png_stream);
	free(png_stream);
	if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
		return surface;
	} else {
		cairo_surface_destroy(surface);
		return NULL;
	}
}

static int update_item_properties(struct swaybar_menu_item *item,
		sd_bus_message *msg, bool *changed) {
	sd_bus_message_enter_container(msg, 'a', "{sv}");
	while (!sd_bus_message_at_end(msg, 0)) {
		bool change = false;
		sd_bus_message_enter_container(msg, 'e', "sv");
		char *key, *log_value;
		sd_bus_message_read_basic(msg, 's', &key);
		if (strcmp(key, "type") == 0) {
			char *type;
			sd_bus_message_read(msg, "v", "s", &type);
			bool is_separator = strcmp(type, "separator") == 0;
			change = item->is_separator == is_separator;
			item->is_separator = is_separator;
			log_value = type;
		} else if (strcmp(key, "label") == 0) {
			char *label;
			sd_bus_message_read(msg, "v", "s", &label);
			change = item->label && strcmp(label, item->label) == 0;
			item->label = realloc(item->label, strlen(label) + 1);
			if (!item->label) {
				return -ENOMEM;
			}
			int i = 0;
			for (char *c = label; *c; ++c) {
				if (*c == '_' && !*++c) {
					break;
				}
				item->label[i++] = *c;
			}
			item->label[i] = '\0';
			log_value = label;
		} else if (strcmp(key, "enabled") == 0) {
			int enabled;
			sd_bus_message_read(msg, "v", "b", &enabled);
			change = item->enabled != enabled;
			item->enabled = enabled;
			log_value = item->enabled ? "true" : "false";
		} else if (strcmp(key, "visible") == 0) {
			int visible;
			sd_bus_message_read(msg, "v", "b", &visible);
			change = item->visible != visible;
			item->visible = visible;
			log_value = item->visible ? "true" : "false";
		} else if (strcmp(key, "icon-name") == 0) {
			char *icon_name;
			sd_bus_message_read(msg, "v", "s", &icon_name);
			change = strcmp(icon_name, item->icon_name) == 0;
			item->icon_name = strdup(icon_name);
			log_value = item->icon_name;
		} else if (strcmp(key, "icon-data") == 0) {
			const void *data;
			size_t data_size;
			sd_bus_message_enter_container(msg, 'v', "ay");
			sd_bus_message_read_array(msg, 'y', &data, &data_size);
			sd_bus_message_exit_container(msg);
			item->icon_data = read_png(data, data_size);
			change = true;
			log_value = item->icon_data ? "<success>" : "<failure>";
		} else if (strcmp(key, "toggle-type") == 0) {
			char *toggle_type;
			sd_bus_message_read(msg, "v", "s", &toggle_type);
			if (strcmp(toggle_type, "checkmark") == 0) {
				change = item->toggle_type != MENU_CHECKMARK;
				item->toggle_type = MENU_CHECKMARK;
			} else if (strcmp(toggle_type, "radio") == 0) {
				change = item->toggle_type != MENU_RADIO;
				item->toggle_type = MENU_RADIO;
			}
			log_value = toggle_type;
		} else if (strcmp(key, "toggle-state") == 0) {
			int toggle_state;
			sd_bus_message_read(msg, "v", "i", &toggle_state);
			change = item->toggle_state != toggle_state;
			item->toggle_state = toggle_state;
			log_value = item->toggle_state == 0 ? "off" :
						item->toggle_state == 1 ? "on" : "indeterminate";
		} else if (strcmp(key, "children-display") == 0) {
			char *children_display;
			sd_bus_message_read(msg, "v", "s", &children_display);
			if (strcmp(children_display, "submenu") == 0) {
				//change = true;
				if (item->children) {
					sway_log(SWAY_DEBUG, "%s%s %d free existing item->children? (%d)", item->sni->service, item->sni->menu_path, item->id, item->children->length);
					// list_free(item->children);
				}
				item->children = create_list();
				if (!item->children) {
					return -ENOMEM;
				}
			}
			log_value = children_display;
		} else {
			// Ignored: shortcut, disposition, accessible-desc
			sd_bus_message_skip(msg, "v");
			log_value = "<ignored>";
		}
		sd_bus_message_exit_container(msg);
		sway_log(SWAY_DEBUG, "%s%s %d %s = '%s' %s", item->sni->service,
				item->sni->menu_path, item->id, key, log_value, change ? "CHANGED" : "");
		*changed |= change;
	}
	return sd_bus_message_exit_container(msg);
}

static int get_layout_callback(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct swaybar_sni_slot *slot = data;
	struct swaybar_sni *sni = slot->sni;
	int id = slot->menu_id;
	wl_list_remove(&slot->link);
	free(slot);

	if (sd_bus_message_is_method_error(msg, NULL)) {
		sway_log(SWAY_ERROR, "%s%s failed to get layout: %s", sni->service,
				sni->menu_path, sd_bus_message_get_error(msg)->message);
		return sd_bus_message_get_errno(msg);
	}

	uint32_t revision;
	sd_bus_message_read_basic(msg, 'u', &revision);
	sway_log(SWAY_DEBUG, "%s%s %d GetLayout callback (revision %u)", sni->service, sni->menu_path, id, revision);

	bool changed = false;
	int ret = 0;
	struct swaybar_menu_item *parent = NULL;
	while (!sd_bus_message_at_end(msg, 1)) {
		sd_bus_message_enter_container(msg, 'r', "ia{sv}av");

		struct swaybar_menu_item *item = calloc(1, sizeof(struct swaybar_menu_item));
		if (!item) {
			ret = -ENOMEM;
			break;
		}

		item->sni = sni;
		item->parent = parent;

		// default properties
		item->enabled = true;
		item->visible = true;
		item->toggle_state = -1;

		sd_bus_message_read_basic(msg, 'i', &item->id);
		ret = update_item_properties(item, msg, &changed);
		if (ret < 0) {
			break;
		}
		if (parent) {
			list_add(parent->children, item);
		} else if (sni->menu) {
			struct swaybar_menu_item **menu_ptr = menu_find_item(&sni->menu,
					item->id);
			sway_log(SWAY_DEBUG, "%s%s call destroy_menu because menu set",
					sni->service, sni->menu_path);
			destroy_menu(*menu_ptr);
			*menu_ptr = item;
		} else {
			sni->menu = item;
		}

		sd_bus_message_enter_container(msg, 'a', "v");
		parent = item;
		while (parent && sd_bus_message_at_end(msg, 0)) {
			parent = parent->parent;
			sd_bus_message_exit_container(msg);
			sd_bus_message_exit_container(msg);
			sd_bus_message_exit_container(msg);
		}
		if (parent && parent->children) {
			sd_bus_message_enter_container(msg, 'v', "(ia{sv}av)");
		}
	}

	struct swaybar_popup *popup = sni->tray->popup;
	if (ret < 0) {
		sway_log(SWAY_ERROR, "%s%s failed to read menu layout: %s",
				sni->service, sni->menu_path, strerror(-ret));
		destroy_menu(sni->menu);
		sni->menu = NULL;
	} else if (popup->sni == sni) {
		if (popup->popup_surface) {
			sway_log(SWAY_DEBUG, "%s%s closing popup instead of redrawing",
					sni->service, sni->menu_path);
			close_popup(popup); // TODO enhancement: redraw instead of closing
		} else {
			open_popup_id(sni, 0);
		}
	}
	return ret;
}

static void update_menu(struct swaybar_sni *sni, int id) {
	sway_log(SWAY_DEBUG, "%s%s %d update_menu", sni->service, sni->menu_path, id);
	struct swaybar_sni_slot *slot = calloc(1, sizeof(struct swaybar_sni_slot));
	slot->sni = sni;

	int ret = sd_bus_call_method_async(sni->tray->bus, &slot->slot,
			sni->service, sni->menu_path, menu_interface, "GetLayout",
			get_layout_callback, slot, "iias", id, -1, NULL);
	if (ret >= 0) {
		wl_list_insert(&sni->slots, &slot->link);
	} else {
		sway_log(SWAY_ERROR, "%s%s failed to get layout: %s",
				sni->service, sni->menu_path, strerror(-ret));
		free(slot);
	}
}

static int handle_layout_updated(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct swaybar_sni *sni = data;

	uint32_t revision;
	int id;
	sd_bus_message_read(msg, "ui", &revision, &id);
	sway_log(SWAY_DEBUG, "%s%s %d layout updated (revision %u)", sni->service, sni->menu_path, id, revision);
	update_menu(sni, id);
	return 0;
}

static int handle_items_properties_updated(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	sway_log(SWAY_DEBUG, "%s%s items properties updated", sni->service, sni->menu_path);

	bool changed = false;
	// update properties
	sd_bus_message_enter_container(msg, 'a', "(ia{sv})");
	while (!sd_bus_message_at_end(msg, 0)) {
		sd_bus_message_enter_container(msg, 'r', "ia{sv}");
		int id;
		sd_bus_message_read_basic(msg, 'i', &id);
		update_item_properties(*menu_find_item(&sni->menu, id), msg, &changed);
	}

	// removed properties
	sd_bus_message_enter_container(msg, 'a', "(ias)");
	while (!sd_bus_message_at_end(msg, 0)) {
		sd_bus_message_enter_container(msg, 'r', "ia{sv}");
		int id;
		sd_bus_message_read_basic(msg, 'i', &id);
		struct swaybar_menu_item *item = *menu_find_item(&sni->menu, id);

		char **keys;
		sd_bus_message_read_strv(msg, &keys);
		if (keys) {
			for (char **key = keys; *key; ++key) {
				if (strcmp(*key, "type") == 0) {
					//changed |= item->is_separator != false;
					item->is_separator = false;
				} else if (strcmp(*key, "label") == 0) {
					//changed |= item->label != NULL;
					free(item->label);
					item->label = NULL;
				} else if (strcmp(*key, "enabled") == 0) {
					//changed |= item->enabled != true;
					item->enabled = true;
					//changed |= item->visible != true;
				} else if (strcmp(*key, "visible") == 0) {
					item->visible = true;
				} else if (strcmp(*key, "children-display") == 0) {
					//changed |= true;
					for (int i = 0; i < item->children->length; ++i) {
						destroy_menu(item->children->items[i]);
					}
					list_free(item->children);
				}
			}
		}
	}

	struct swaybar_popup *popup = sni->tray->popup;
	if (popup->sni == sni) {
		sway_log(SWAY_DEBUG, "closing popup instead of redrawing");
		close_popup(popup); // TODO enhancement: redraw instead of closing
	}

	return 0;
}

static int handle_item_activation_requested(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	sway_log(SWAY_DEBUG, "%s%s item activation requested (TODO)", sni->service, sni->menu_path);
	return 0; // TODO
}

static void sni_menu_match_signal_async(struct swaybar_sni *sni, char *signal,
		sd_bus_message_handler_t callback) {
	struct swaybar_sni_slot *slot = calloc(1, sizeof(struct swaybar_sni_slot));
	int ret = sd_bus_match_signal_async(sni->tray->bus, &slot->slot, sni->service,
			sni->menu_path, menu_interface, signal, callback, NULL, sni);
	if (ret >= 0) {
		wl_list_insert(&sni->slots, &slot->link);
	} else {
		sway_log(SWAY_ERROR, "%s%s failed to subscribe to signal %s: %s",
				sni->service, sni->menu_path, signal, strerror(-ret));
		free(slot);
	}
}

static int get_icon_theme_path_callback(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct swaybar_sni_slot *slot = data;
	struct swaybar_sni *sni = slot->sni;
	wl_list_remove(&slot->link);
	free(slot);

	int ret;
	if (!sd_bus_message_is_method_error(msg, NULL)) {
		ret = sd_bus_message_enter_container(msg, 'v', NULL);
		if (ret >= 0) {
			ret = sd_bus_message_read_strv(msg, &sni->menu_icon_theme_paths);
		}
	} else {
		ret = -sd_bus_message_get_errno(msg);
	}

	if (ret < 0) {
		sway_log(SWAY_ERROR, "%s%s failed to read IconThemePath: %s",
				sni->service, sni->menu_path, strerror(-ret));
	}
	return ret;
}

static void setup_menu(struct swaybar_sni *sni) {
  sway_log(SWAY_DEBUG, "%s%s setup_menu", sni->service, sni->menu_path);
	struct swaybar_sni_slot *slot = calloc(1, sizeof(struct swaybar_sni_slot));
	slot->sni = sni;
	int ret = sd_bus_call_method_async(sni->tray->bus, &slot->slot, sni->service,
			sni->path, "org.freedesktop.DBus.Properties", "Get",
			get_icon_theme_path_callback, slot, "ss", sni->interface, "IconThemePath");
	if (ret >= 0) {
		wl_list_insert(&sni->slots, &slot->link);
	} else {
		sway_log(SWAY_ERROR, "%s%s failed to get IconThemePath: %s",
				sni->service, sni->menu_path, strerror(-ret));
		free(slot);
	}

	sni_menu_match_signal_async(sni, "ItemsPropertiesUpdated", handle_items_properties_updated);
	sni_menu_match_signal_async(sni, "LayoutUpdated", handle_layout_updated);
	sni_menu_match_signal_async(sni, "ItemActivationRequested", handle_item_activation_requested);

	update_menu(sni, 0);
}

/* POPUP */

static void destroy_popup_surface(struct swaybar_popup_surface *popup_surface) {
	if (!popup_surface) {
		return;
	}

	destroy_popup_surface(popup_surface->child);
	list_free_items_and_destroy(popup_surface->hotspots);
	xdg_popup_destroy(popup_surface->xdg_popup);
	wl_surface_destroy(popup_surface->surface);
	destroy_buffer(&popup_surface->buffers[0]);
	destroy_buffer(&popup_surface->buffers[1]);

	int id = popup_surface->item->id;
	struct swaybar_sni *sni = popup_surface->item->sni;
	sd_bus_call_method_async(sni->tray->bus, NULL, sni->service, sni->menu_path,
			menu_interface, "Event", NULL, NULL, "isvu", id, "closed", "y", 0, time(NULL));
	sway_log(SWAY_DEBUG, "%s%s %d closed", sni->service, sni->menu_path, id);

	free(popup_surface);
}

static void close_popup(struct swaybar_popup *popup) {
	if (!popup) {
		return;
	}

	//sway_log(SWAY_DEBUG, "%s%s %d close_popup", popup->sni->service, popup->sni->menu_path, popup->popup_surface->item->id);
	sway_log(SWAY_DEBUG, "%s%s close_popup", popup->sni->service, popup->sni->menu_path);
	destroy_popup_surface(popup->popup_surface);
	popup->popup_surface = NULL;
	popup->sni = NULL;
}

void destroy_popup(struct swaybar_popup *popup) {
	if (!popup) {
		return;
	}

	sway_log(SWAY_DEBUG, "%s%s %d destroy_popup", popup->sni->service, popup->sni->menu_path, popup->popup_surface->item->id);
	close_popup(popup);
	popup->tray->popup = NULL;
	xdg_wm_base_destroy(popup->wm_base);
	free(popup);
}

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct swaybar_popup *popup = data;
	if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		popup->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	// intentionally left blank
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static struct swaybar_popup *create_popup(struct swaybar_tray *tray) {
	struct swaybar_popup *popup = calloc(1, sizeof(struct swaybar_popup));
	if (!popup) {
		return NULL;
	}

	struct wl_registry *registry = wl_display_get_registry(tray->bar->display);
	wl_registry_add_listener(registry, &registry_listener, popup);
	wl_display_roundtrip(tray->bar->display);
	popup->tray = tray;
	return popup;
}

static void xdg_surface_handle_configure(void *data,
		struct xdg_surface *xdg_surface, uint32_t serial) {
	xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

static void xdg_popup_configure(void *data, struct xdg_popup *xdg_popup,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	// intentionally left blank
}

static void xdg_popup_done(void *data, struct xdg_popup *xdg_popup) {
	struct swaybar_popup *popup = data;
	sway_log(SWAY_DEBUG, "closing popup due to xdg_popup_done");
	close_popup(popup);
}

static const struct xdg_popup_listener xdg_popup_listener = {
	.configure = xdg_popup_configure,
	.popup_done = xdg_popup_done
};

static void show_popup_id(struct swaybar_sni *sni, int id) {
	sway_log(SWAY_DEBUG, "%s%s %d show_popup_id", sni->service, sni->menu_path, id);

	cairo_surface_t *recorder =
		cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(recorder);

	list_t *hotspots = create_list();
	struct swaybar_popup_surface *popup_surface =
		calloc(1, sizeof(struct swaybar_popup_surface));
	if (!(hotspots && popup_surface)) {
		goto error;
	}

	struct swaybar_tray *tray = sni->tray;
	struct swaybar_popup *popup = tray->popup;
	struct swaybar_output *output = popup->output;

	struct swaybar *bar = tray->bar;
	struct swaybar_config *config = bar->config;
	int padding = config->tray_padding * output->scale;

	struct swaybar_menu_item *root = *menu_find_item(&sni->menu, id);
	list_t *items = root->children;
	int height = 0;
	for (int i = 0; i < items->length; ++i) {
		struct swaybar_menu_item *item = items->items[i];

		if (!item->visible) {
			continue;
		}

		if (item->is_separator) {
			++height; // drawn later, after the width is known
		} else if (item->label) {
			cairo_move_to(cairo, 0, height + padding);

			// draw label
			if (item->enabled) {
				cairo_set_source_u32(cairo, config->colors.focused_statusline);
			} else {
				uint32_t c = config->colors.focused_statusline;
				uint32_t disabled_color = c - ((c & 0xFF) >> 1);
				cairo_set_source_u32(cairo, disabled_color);
			}
			pango_printf(cairo, config->font, output->scale, false, "%s", item->label);

			// draw icon or menu indicator if needed
			int text_height;
			get_text_size(cairo, config->font, NULL, &text_height, NULL,
					output->scale, false, "%s", item->label);
			int size = 16;
			int x = -2 * padding - size;
			int y = height + padding + (text_height - size + 1) / 2;
			cairo_set_source_u32(cairo, config->colors.focused_statusline);
			if (item->icon_name) {
				list_t *icon_search_paths = create_list();
				list_cat(icon_search_paths, tray->basedirs);
				if (sni->menu_icon_theme_paths) {
					for (char **path = sni->menu_icon_theme_paths; *path; ++path) {
						list_add(icon_search_paths, *path);
					}
				}
				if (sni->icon_theme_path) {
					list_add(icon_search_paths, sni->icon_theme_path);
				}
				int min_size, max_size;
				char *icon_path = find_icon(tray->themes, icon_search_paths,
						item->icon_name, size, config->icon_theme,
						&min_size, &max_size);
				list_free(icon_search_paths);

				if (icon_path) {
					cairo_surface_t *icon = load_background_image(icon_path);
					free(icon_path);
					cairo_surface_t *icon_scaled =
						cairo_image_surface_scale(icon, size, size);
					cairo_surface_destroy(icon);

					cairo_set_source_surface(cairo, icon_scaled, x, y);
					cairo_rectangle(cairo, x, y, size, size);
					cairo_fill(cairo);
					cairo_surface_destroy(icon_scaled);
				}
			} else if (item->icon_data) {
				cairo_surface_t *icon =
					cairo_image_surface_scale(item->icon_data, size, size);
				cairo_set_source_surface(cairo, icon, x, y);
				cairo_rectangle(cairo, x, y, size, size);
				cairo_fill(cairo);
				cairo_surface_destroy(icon);
			} else if (item->toggle_type == MENU_CHECKMARK) {
				cairo_rectangle(cairo, x, y, size, size);
				cairo_fill(cairo);
				cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
				if (item->toggle_state == 1) { // tick
					cairo_move_to(cairo, x + size*3/4, y + size*5/16);
					cairo_line_to(cairo, x + size*3/8, y + size*11/16);
					cairo_line_to(cairo, x + size/4, y + size*9/16);
					cairo_stroke(cairo);
				} else if (item->toggle_state != 0) { // horizontal line
					cairo_rectangle(cairo, x + size/4, y + size/2 - 1, size/2, 2);
					cairo_fill(cairo);
				}
				cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
			} else if (item->toggle_type == MENU_RADIO) {
				cairo_arc(cairo, x + size/2, y + size/2, size/2, 0, 7);
				cairo_fill(cairo);
				if (item->toggle_state == 1) {
					cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
					cairo_arc(cairo, x + size/2, y + size/2, size/4, 0, 7);
					cairo_fill(cairo);
					cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
				}
			} else if (item->children) { // arrowhead
				cairo_move_to(cairo, x + size/4, y + size/2);
				cairo_line_to(cairo, x + size*3/4, y + size/4);
				cairo_line_to(cairo, x + size*3/4, y + size*3/4);
				cairo_fill(cairo);
			}

			height += text_height + 2 * padding;
		} else {
			continue;
		}

		struct swaybar_popup_hotspot *hotspot =
			malloc(sizeof(struct swaybar_popup_hotspot));
		hotspot->y = height;
		hotspot->item = item;
		list_add(hotspots, hotspot);
	}

	if (height == 0) {
		goto error;
	}

	// draw separators
	double ox, w;
	cairo_recording_surface_ink_extents(recorder, &ox, NULL, &w, NULL);
	ox -= 2 * padding;
	int width = w + 4 * padding;

	cairo_set_line_width(cairo, 1);
	cairo_set_source_u32(cairo, config->colors.focused_separator);
	for (int i = 0; i < hotspots->length; ++i) {
		struct swaybar_popup_hotspot *hotspot = hotspots->items[i];
		if (hotspot->item->is_separator) {
			cairo_move_to(cairo, ox, hotspot->y - 1/2);
			cairo_line_to(cairo, ox + width, hotspot->y - 1/2);
			cairo_stroke(cairo);
		}
	}

	// draw popup surface
	popup_surface->current_buffer = get_next_buffer(tray->bar->shm,
			popup_surface->buffers, width, height);
	if (!popup_surface->current_buffer) {
		goto error;
	}
	cairo_t *shm = popup_surface->current_buffer->cairo;

	cairo_set_operator(shm, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(shm, config->colors.focused_background);
	cairo_paint(shm);

	cairo_set_operator(shm, CAIRO_OPERATOR_OVER);
	cairo_set_source_surface(shm, recorder, -ox, 0);
	cairo_paint(shm);

	// configure & position popup surface
	struct wl_surface *surface = wl_compositor_create_surface(bar->compositor);
	struct xdg_surface *xdg_surface =
		xdg_wm_base_get_xdg_surface(popup->wm_base, surface);
	struct xdg_positioner *positioner = xdg_wm_base_create_positioner(popup->wm_base);

	int x = popup->x;
	int y = popup->y;
	xdg_positioner_set_anchor_rect(positioner, x, y, 1, 1);
	xdg_positioner_set_offset(positioner, 0, 0);
	xdg_positioner_set_size(positioner, width / output->scale, height / output->scale);
	if (config->position & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) { // top bar
		xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
		xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_LEFT);
	} else {
		xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
		xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_TOP_LEFT);
	}

	struct xdg_popup *xdg_popup;
	if (!popup->popup_surface) { // top-level popup
		xdg_popup = xdg_surface_get_popup(xdg_surface, NULL, positioner);
		zwlr_layer_surface_v1_get_popup(output->layer_surface, xdg_popup);
		popup->popup_surface = popup_surface;
	} else { // nested popup
		struct swaybar_popup_surface *parent = popup->popup_surface;
		while (parent->child) {
			parent = parent->child;
		}
		xdg_popup = xdg_surface_get_popup(xdg_surface, parent->xdg_surface, positioner);
		parent->child = popup_surface;
	}
	xdg_popup_grab(xdg_popup, popup->seat, popup->serial);
	xdg_popup_add_listener(xdg_popup, &xdg_popup_listener, popup);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	wl_surface_commit(surface);

	wl_display_roundtrip(bar->display);
	xdg_positioner_destroy(positioner);

	wl_surface_set_buffer_scale(surface, output->scale);
	wl_surface_attach(surface, popup_surface->current_buffer->buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);

	popup_surface->item = root;
	popup_surface->hotspots = hotspots;
	popup_surface->xdg_popup = xdg_popup;
	popup_surface->xdg_surface = xdg_surface;
	popup_surface->surface = surface;

	sd_bus_call_method_async(sni->tray->bus, NULL, sni->service, sni->menu_path,
			menu_interface, "Event", NULL, NULL, "isvu", id, "opened", "y", 0, time(NULL));
	sway_log(SWAY_DEBUG, "%s%s %d opened", sni->service, sni->menu_path, id);

cleanup:
	cairo_surface_destroy(recorder);
	cairo_destroy(cairo);
	return;
error:
  sway_log(SWAY_DEBUG, "%s%s %d show_popup_id error (height: %d), no popup_surface", sni->service, sni->menu_path, id, height);
	list_free_items_and_destroy(hotspots);
	free(popup_surface);
	goto cleanup;
}

static int about_to_show_callback(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct swaybar_sni_slot *slot = data;
	struct swaybar_sni *sni = slot->sni;
	int id = slot->menu_id;
	sway_log(SWAY_DEBUG, "%s%s %d AboutToShow callback", sni->service, sni->menu_path, id);
	wl_list_remove(&slot->link);
	free(slot);

	int need_update;
	sd_bus_message_read_basic(msg, 'b', &need_update);
	if (need_update) {
		update_menu(sni, id);
	} else {
		show_popup_id(sni, id);
	}
	return 0;
}

static void open_popup_id(struct swaybar_sni *sni, int id) {
	sway_log(SWAY_DEBUG, "%s%s %d open_popup_id", sni->service, sni->menu_path, id);
	struct swaybar_sni_slot *slot = calloc(1, sizeof(struct swaybar_sni_slot));
	slot->sni = sni;
	slot->menu_id = id;

	int ret = sd_bus_call_method_async(sni->tray->bus, &slot->slot,
			sni->service, sni->menu_path, menu_interface,
			"AboutToShow", about_to_show_callback, slot, "i", id);
	if (ret >= 0) {
		wl_list_insert(&sni->slots, &slot->link);
	} else {
		sway_log(SWAY_ERROR, "%s%s failed to send AboutToShow signal: %s",
				sni->service, sni->menu_path, strerror(-ret));
		free(slot);
	}
}

void open_popup(struct swaybar_sni *sni, struct swaybar_output *output,
		struct wl_seat *seat, uint32_t serial, int x, int y) {
	sway_log(SWAY_DEBUG, "%s%s open_popup", sni->service, sni->menu_path);

	struct swaybar_tray *tray = sni->tray;
	struct swaybar_popup *popup = tray->popup;
	if (!popup) {
		popup = tray->popup = create_popup(tray);
		if (!popup) {
			return;
		}
	}

	if (!sway_assert(!popup->popup_surface, "popup already open")) {
		return;
	}

	popup->sni = sni;
	popup->output = output;
	popup->seat = seat;
	popup->serial = serial;
	popup->x = x;
	popup->y = y;

	if (sni->menu) {
		open_popup_id(sni, 0);
	} else {
		setup_menu(sni);
	}
}

// input hooks

static struct swaybar_popup_surface *popup_find_surface(struct swaybar_popup *popup,
		struct wl_surface *surface) {
	struct swaybar_popup_surface *popup_surface = popup->popup_surface;
	while (popup_surface) {
		if (popup_surface->surface == surface) {
			return popup_surface;
		}
		popup_surface = popup_surface->child;
	}
	return NULL;
}

bool popup_pointer_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct swaybar_seat *seat = data;
	struct swaybar_tray *tray = seat->bar->tray;
	if (!(tray && tray->popup)) {
		return false;
	}

	struct swaybar_popup *popup = tray->popup;
	struct swaybar_popup_surface *popup_surface = popup_find_surface(popup, surface);
	if (popup_surface) {
		struct swaybar_pointer *pointer = &seat->pointer;
		pointer->current = popup->output;
		pointer->serial = serial;
		update_cursor(seat);

		popup->pointer_focus = popup_surface;
	}
	return popup_surface;
}

bool popup_pointer_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct swaybar_seat *seat = data;
	struct swaybar_tray *tray = seat->bar->tray;
	if (!(tray && tray->popup)) {
		return false;
	}

	tray->popup->pointer_focus = NULL;
	tray->popup->last_hover = NULL;
	return false;
}

bool popup_pointer_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time_, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct swaybar_seat *seat = data;
	struct swaybar_tray *tray = seat->bar->tray;
	if (!(tray && tray->popup && tray->popup->pointer_focus)) {
		return false;
	}

	struct swaybar_popup *popup = tray->popup;
	double y = seat->pointer.y * popup->output->scale;
	struct swaybar_popup_hotspot **hotspots_start =
		(struct swaybar_popup_hotspot **)popup->pointer_focus->hotspots->items;
	struct swaybar_popup_hotspot **hotspot_ptr = hotspots_start;
	int step = 1;
	if (popup->last_hover) { // calculate whether pointer went up or down
		hotspot_ptr = popup->last_hover;
		step = y < (*hotspot_ptr)->y ? -1 : 1;
		hotspot_ptr += step;
	}

	for (; hotspot_ptr >= hotspots_start; hotspot_ptr += step) {
		if ((step == 1) == (y < (*hotspot_ptr)->y)) {
			break;
		}
	}

	hotspot_ptr += step == -1;
	struct swaybar_menu_item *item = (*hotspot_ptr)->item;
	if (hotspot_ptr != popup->last_hover && item->enabled && !item->is_separator) {
		struct swaybar_sni *sni = item->sni;
		sd_bus_call_method_async(tray->bus, NULL, sni->service,
				sni->menu_path, menu_interface, "Event", NULL, NULL, "isvu",
				item->id, "hovered", "y", 0, time(NULL));
		sway_log(SWAY_DEBUG, "%s%s %d hovered", sni->service, sni->menu_path, item->id);
	}
	popup->last_hover = hotspot_ptr;

	return true;
}

bool popup_pointer_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time_, uint32_t button, uint32_t state) {
	sway_log(SWAY_DEBUG, "button: %d - state: %d", button, state);
	struct swaybar_seat *seat = data;
	struct swaybar_tray *tray = seat->bar->tray;
	if (!(tray && tray->popup)) {
		return false;
	}

	struct swaybar_popup *popup = tray->popup;
	float y = seat->pointer.y * popup->output->scale;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		// intentionally left blank
	} else if (popup->popup_surface) {
		if (!popup->pointer_focus) {
			sway_log(SWAY_DEBUG, "closing unfocused open popup");
			close_popup(popup);
		} else if (button == BTN_LEFT) {
			list_t *hotspots = popup->pointer_focus->hotspots;
			for (int i = 0; i < hotspots->length; ++i) {
				struct swaybar_popup_hotspot *hotspot = hotspots->items[i];
				if (y < hotspot->y) {
					struct swaybar_menu_item *item = hotspot->item;

					if (!item->enabled || item->is_separator) {
						break;
					}

					struct swaybar_sni *sni = popup->sni;
					if (item->children) {
						destroy_popup_surface(popup->pointer_focus->child);
						popup->pointer_focus->child = NULL;
						popup->serial = serial;
						popup->x = 0;
						if (tray->bar->config->position & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) { // top bar
							popup->y = i ? ((struct swaybar_popup_hotspot *)hotspots->items[i-1])->y : 0;
						} else {
							popup->y = hotspot->y;
						}
						popup->y /= popup->output->scale;
						open_popup_id(sni, item->id);
					} else {
						sd_bus_call_method_async(tray->bus, NULL, sni->service,
								sni->menu_path, menu_interface, "Event", NULL, NULL,
								"isvu", item->id, "clicked", "y", 0, time(NULL));
						sway_log(SWAY_DEBUG, "%s%s %d popup clicked id %d",
								sni->service, sni->menu_path, popup->popup_surface->item->id, item->id);
						close_popup(popup);
					}
					break;
				}
			}
		}
	}
	return popup->pointer_focus;
}

bool popup_pointer_axis(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	struct swaybar_seat *seat = data;
	struct swaybar_tray *tray = seat->bar->tray;
	if (!(tray && tray->popup)) {
		return false;
	}
	return tray->popup->pointer_focus;
}

bool popup_touch_down(void *data, struct wl_touch *wl_touch, uint32_t serial,
		uint32_t time, struct wl_surface *surface, int32_t id, wl_fixed_t _x,
		wl_fixed_t _y) {
	struct swaybar_seat *seat = data;
	struct swaybar_tray *tray = seat->bar->tray;
	if (!(tray && tray->popup)) {
		return false;
	}

	struct swaybar_popup_surface *popup_surface = popup_find_surface(tray->popup, surface);
	return popup_surface;
}

