/*
 * Copyright © 2011 Kristian Høgsberg
 * Copyright © 2011 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <cairo.h>
#include <sys/wait.h>
#include <sys/timerfd.h>
#include <sys/epoll.h> 
#include <linux/input.h>
#include <libgen.h>
#include <time.h>

#include <wayland-client.h>
#include "window.h"
#include "../shared/cairo-util.h"
#include "../shared/config-parser.h"

#include "desktop-shell-client-protocol.h"

struct desktop {
	struct display *display;
	struct desktop_shell *shell;
	struct unlock_dialog *unlock_dialog;
	struct task unlock_task;
	struct wl_list outputs;

	struct window *grab_window;
	struct widget *grab_widget;

	enum desktop_shell_cursor grab_cursor;
};

struct surface {
	void (*configure)(void *data,
			  struct desktop_shell *desktop_shell,
			  uint32_t edges, struct window *window,
			  int32_t width, int32_t height);
};

struct panel {
	struct surface base;
	struct window *window;
	struct widget *widget;
	struct wl_list launcher_list;
	struct panel_clock *clock;
};

struct iconlayer {
	struct surface base;
	struct window *window;
	struct widget *widget;
	struct wl_list icons_list;
	cairo_surface_t *image[10];
	struct rectangle mouse_selection_absolute_size;
	struct rectangle mouse_selection_relative_size;
	int selecting;
};

struct icon {
	struct widget *widget;
	struct iconlayer *iconlayer;
	cairo_surface_t *image;
	unsigned int selected;
	struct wl_list link;	/* icons_list */
	char *text;
};

struct background {
	struct surface base;
	struct window *window;
	struct widget *widget;
};

struct output {
	struct wl_output *output;
	struct wl_list link;

	struct panel *panel;
	struct iconlayer *iconlayer;
	struct background *background;
};

struct panel_launcher {
	struct widget *widget;
	struct panel *panel;
	cairo_surface_t *icon;
	int focused, pressed;
	const char *path;
	struct wl_list link;
};

struct panel_clock {
	struct widget *widget;
	struct panel *panel;
	struct task clock_task;
	int clock_fd;
};

struct unlock_dialog {
	struct window *window;
	struct widget *widget;
	struct widget *button;
	int button_focused;
	int closing;
	struct desktop *desktop;
};

static char *key_background_image = DATADIR "/weston/pattern.png";
static char *key_background_type = "tile";
static uint32_t key_panel_color = 0xaa000000;
static uint32_t key_background_color = 0xff002244;
static char *key_launcher_icon;
static char *key_launcher_path;
static void launcher_section_done(void *data);
static int key_locking = 1;

static const struct config_key shell_config_keys[] = {
	{ "background-image", CONFIG_KEY_STRING, &key_background_image },
	{ "background-type", CONFIG_KEY_STRING, &key_background_type },
	{ "panel-color", CONFIG_KEY_UNSIGNED_INTEGER, &key_panel_color },
	{ "background-color", CONFIG_KEY_UNSIGNED_INTEGER, &key_background_color },
	{ "locking", CONFIG_KEY_BOOLEAN, &key_locking },
};

static const struct config_key launcher_config_keys[] = {
	{ "icon", CONFIG_KEY_STRING, &key_launcher_icon },
	{ "path", CONFIG_KEY_STRING, &key_launcher_path },
};

static const struct config_section config_sections[] = {
	{ "shell",
	  shell_config_keys, ARRAY_LENGTH(shell_config_keys) },
	{ "launcher",
	  launcher_config_keys, ARRAY_LENGTH(launcher_config_keys),
	  launcher_section_done }
};

static void
sigchild_handler(int s)
{
	int status;
	pid_t pid;

	while (pid = waitpid(-1, &status, WNOHANG), pid > 0)
		fprintf(stderr, "child %d exited\n", pid);
}

static void
menu_func(struct window *window, int index, void *data)
{
	printf("Selected index %d from a panel menu.\n", index);
}

static void
show_menu(struct panel *panel, struct input *input, uint32_t time)
{
	int32_t x, y;
	static const char *entries[] = {
		"Roy", "Pris", "Leon", "Zhora"
	};

	input_get_position(input, &x, &y);
	window_show_menu(window_get_display(panel->window),
			 input, time, panel->window,
			 x - 10, y - 10, menu_func, entries, 4);
}

static void
panel_launcher_activate(struct panel_launcher *widget)
{
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork failed: %m\n");
		return;
	}

	if (pid)
		return;

	if (execl(widget->path, widget->path, NULL) < 0) {
		fprintf(stderr, "execl '%s' failed: %m\n", widget->path);
		exit(1);
	}
}

static void
panel_launcher_redraw_handler(struct widget *widget, void *data)
{
	struct panel_launcher *launcher = data;
	cairo_surface_t *surface;
	struct rectangle allocation;
	cairo_t *cr;

	surface = window_get_surface(launcher->panel->window);
	cr = cairo_create(surface);

	widget_get_allocation(widget, &allocation);
	if (launcher->pressed) {
		allocation.x++;
		allocation.y++;
	}

	cairo_set_source_surface(cr, launcher->icon,
				 allocation.x, allocation.y);
	cairo_paint(cr);

	if (launcher->focused) {
		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
		cairo_mask_surface(cr, launcher->icon,
				   allocation.x, allocation.y);
	}

	cairo_destroy(cr);
}

static int
panel_launcher_motion_handler(struct widget *widget, struct input *input,
			      uint32_t time, float x, float y, void *data)
{
	struct panel_launcher *launcher = data;

	widget_set_tooltip(widget, basename((char *)launcher->path), x, y);

	return CURSOR_LEFT_PTR;
}

static void
set_hex_color(cairo_t *cr, uint32_t color)
{
	cairo_set_source_rgba(cr, 
			      ((color >> 16) & 0xff) / 255.0,
			      ((color >>  8) & 0xff) / 255.0,
			      ((color >>  0) & 0xff) / 255.0,
			      ((color >> 24) & 0xff) / 255.0);
}

static void
panel_redraw_handler(struct widget *widget, void *data)
{
	cairo_surface_t *surface;
	cairo_t *cr;
	struct panel *panel = data;

	surface = window_get_surface(panel->window);
	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	set_hex_color(cr, key_panel_color);
	cairo_paint(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static int
panel_launcher_enter_handler(struct widget *widget, struct input *input,
			     float x, float y, void *data)
{
	struct panel_launcher *launcher = data;

	launcher->focused = 1;
	widget_schedule_redraw(widget);

	return CURSOR_LEFT_PTR;
}

static void
panel_launcher_leave_handler(struct widget *widget,
			     struct input *input, void *data)
{
	struct panel_launcher *launcher = data;

	launcher->focused = 0;
	widget_destroy_tooltip(widget);
	widget_schedule_redraw(widget);
}

static void
panel_launcher_button_handler(struct widget *widget,
			      struct input *input, uint32_t time,
			      uint32_t button,
			      enum wl_pointer_button_state state, void *data)
{
	struct panel_launcher *launcher;

	launcher = widget_get_user_data(widget);
	widget_schedule_redraw(widget);
	if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		panel_launcher_activate(launcher);
}

static void
clock_func(struct task *task, uint32_t events)
{
	struct panel_clock *clock =
		container_of(task, struct panel_clock, clock_task);
	uint64_t exp;

	read(clock->clock_fd, &exp, sizeof exp);
	widget_schedule_redraw(clock->widget);
}

static void
iconlayer_finish_selection(struct iconlayer *iconlayer, struct input *input)
{
	struct rectangle *relative = &iconlayer->mouse_selection_relative_size;
	struct rectangle *absolute = &iconlayer->mouse_selection_absolute_size;

	memset(relative, 0, sizeof(*relative));
	memset(absolute, 0, sizeof(*absolute));
	iconlayer->selecting = 0;
}

static void
iconlayer_start_selection(struct iconlayer *iconlayer, struct input *input)
{
	struct rectangle *relative = &iconlayer->mouse_selection_relative_size;
	struct rectangle *absolute = &iconlayer->mouse_selection_absolute_size;
	struct icon *icon, *next;

	input_get_position(input, &relative->x, &relative->y);
	input_get_position(input, &absolute->x, &absolute->y);
	input_get_position(input, &absolute->width, &absolute->height);
	relative->width = 0;
	relative->height = 0;
	iconlayer->selecting = 1;

	wl_list_for_each_safe(icon, next, &iconlayer->icons_list, link) {
		icon->selected = 0;
	}
}

static void
iconlayer_update_selection(struct iconlayer *iconlayer, struct input *input)
{
	struct rectangle *relative = &iconlayer->mouse_selection_relative_size;
	struct rectangle *absolute = &iconlayer->mouse_selection_absolute_size;
	struct rectangle selection;
	struct icon *icon, *next;

	input_get_position(input, &selection.x, &selection.y);
	relative->width = selection.x - relative->x;
	relative->height = selection.y - relative->y;

	if (relative->width == 0)
		relative->width = 1;

	if (relative->width > 0) {
		absolute->x = relative->x;
		absolute->width = selection.x;
	} else {
		absolute->x = selection.x;
		absolute->width = relative->x;
	}

	if (relative->height == 0)
		relative->height = 1;

	if (relative->height > 0) {
		absolute->y = relative->y;
		absolute->height = selection.y;
	} else {
		absolute->y = selection.y;
		absolute->height = relative->y;
	}

	/* Performance hack */
	int x_minus_icon_x = absolute->x - 96;
	int y_minus_icon_y = absolute->y - 64;

	wl_list_for_each_safe(icon, next, &iconlayer->icons_list, link) {
		struct rectangle allocation;
		widget_get_allocation(icon->widget, &allocation);
		icon->selected = (
			(x_minus_icon_x < allocation.x) &&
			(absolute->width > allocation.x) &&
			(y_minus_icon_y < allocation.y) &&
			(absolute->height > allocation.y));
	}


}

static void
iconlayer_redraw_handler(struct widget *widget, void *data)
{
	cairo_surface_t *surface;
	cairo_t *cr;
	struct iconlayer *iconlayer = data;
	struct rectangle *absolute = &iconlayer->mouse_selection_absolute_size;
	struct rectangle *relative = &iconlayer->mouse_selection_relative_size;
	surface = window_get_surface(iconlayer->window);
	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	set_hex_color(cr, 0x00FFFFFF);
	cairo_paint(cr);
	cairo_fill(cr);

	if (iconlayer->selecting) {
		if ((abs(relative->width) > 9) && (abs(relative->height) > 9)) {
			rounded_rect(cr, absolute->x, absolute->y,
				absolute->width, absolute->height, 4);
		} else {
			cairo_rectangle(cr, absolute->x, absolute->y,
				abs(relative->width), abs(relative->height));
		}

		cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
		cairo_fill(cr);
	}

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
panel_clock_redraw_handler(struct widget *widget, void *data)
{
	cairo_surface_t *surface;
	struct panel_clock *clock = data;
	cairo_t *cr;
	struct rectangle allocation;
	cairo_text_extents_t extents;
	cairo_font_extents_t font_extents;
	time_t rawtime;
	struct tm * timeinfo;
	char string[128];

	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(string, sizeof string, "%a %b %d, %I:%M %p", timeinfo);

	widget_get_allocation(widget, &allocation);
	if (allocation.width == 0)
		return;

	surface = window_get_surface(clock->panel->window);
	cr = cairo_create(surface);
	cairo_select_font_face(cr, "sans",
			       CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);
	cairo_text_extents(cr, string, &extents);
	cairo_font_extents (cr, &font_extents);
	cairo_move_to(cr, allocation.x + 5,
		      allocation.y + 3 * (allocation.height >> 2) + 1);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_show_text(cr, string);
	cairo_move_to(cr, allocation.x + 4,
		      allocation.y + 3 * (allocation.height >> 2));
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_show_text(cr, string);
	cairo_destroy(cr);
}

static int
clock_timer_reset(struct panel_clock *clock)
{
	struct itimerspec its;

	its.it_interval.tv_sec = 60;
	its.it_interval.tv_nsec = 0;
	its.it_value.tv_sec = 60;
	its.it_value.tv_nsec = 0;
	if (timerfd_settime(clock->clock_fd, 0, &its, NULL) < 0) {
		fprintf(stderr, "could not set timerfd\n: %m");
		return -1;
	}

	return 0;
}

static void
panel_add_clock(struct panel *panel)
{
	struct panel_clock *clock;
	int timerfd;

	timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (timerfd < 0) {
		fprintf(stderr, "could not create timerfd\n: %m");
		return;
	}

	clock = malloc(sizeof *clock);
	memset(clock, 0, sizeof *clock);
	clock->panel = panel;
	panel->clock = clock;
	clock->clock_fd = timerfd;

	clock->clock_task.run = clock_func;
	display_watch_fd(window_get_display(panel->window), clock->clock_fd,
			 EPOLLIN, &clock->clock_task);
	clock_timer_reset(clock);

	clock->widget = widget_add_widget(panel->widget, clock);
	widget_set_redraw_handler(clock->widget, panel_clock_redraw_handler);
}

static void
panel_button_handler(struct widget *widget,
		     struct input *input, uint32_t time,
		     uint32_t button,
		     enum wl_pointer_button_state state, void *data)
{
	struct panel *panel = data;

	if (button == BTN_RIGHT && state == WL_POINTER_BUTTON_STATE_PRESSED)
		show_menu(panel, input, time);
}

static void
iconlayer_button_handler(struct widget *widget,
		     struct input *input, uint32_t time,
		     uint32_t button,
		     enum wl_pointer_button_state state, void *data)
{
	struct iconlayer *iconlayer = data;

	if (button == BTN_LEFT) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			iconlayer_start_selection(iconlayer, input);
		} else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
			iconlayer_finish_selection(iconlayer, input);
		}
		widget_schedule_redraw(widget);
	}
}

static int
iconlayer_motion_handler(struct widget *widget,
				       struct input *input, uint32_t time,
				       float x, float y, void *data)
{
	struct iconlayer *iconlayer = data;

	if (iconlayer->selecting == 1) {
		iconlayer_update_selection(iconlayer, input);
		widget_schedule_redraw(widget);
	}

	return CURSOR_LEFT_PTR;
}

static void
icon_redraw_handler(struct widget *widget, void *data)
{
	cairo_surface_t *surface;
	cairo_t *cr;
	cairo_text_extents_t extents;
	cairo_font_extents_t font_extents;
	struct icon *icon = data;
	struct rectangle allocation;

	widget_get_allocation(widget, &allocation);

	surface = window_get_surface(icon->iconlayer->window);
	cr = cairo_create(surface);

	if (icon->selected) {
		rounded_rect(cr, allocation.x + 2, allocation.y + 2,
			allocation.x + allocation.width - 4,
			allocation.y + allocation.height - 4, 5);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
		cairo_fill(cr);
	}

	cairo_select_font_face(cr, "sans",
			       CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 13);
	cairo_text_extents(cr, "Hello world", &extents);
	cairo_font_extents (cr, &font_extents);
	cairo_move_to(cr, allocation.x + 11, allocation.y + 56);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_show_text(cr, "Hello world");
	cairo_move_to(cr, allocation.x + 10, allocation.y + 55);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_show_text(cr, "Hello world");
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);

	cairo_set_source_surface(cr, icon->image, allocation.x + 31, allocation.y + 7);
	cairo_paint(cr);
	cairo_destroy(cr);
}

static void
iconlayer_resize_handler(struct widget *widget,
		     int32_t width, int32_t height, void *data)
{
	struct iconlayer *iconlayer = data;
	int x, y, w, h;

	x = 10;
	y = 16;
	widget_set_allocation(iconlayer->widget, x, y, width, height);
	window_schedule_redraw(iconlayer->window);
}

static void
panel_resize_handler(struct widget *widget,
		     int32_t width, int32_t height, void *data)
{
	struct panel_launcher *launcher;
	struct panel *panel = data;
	int x, y, w, h;
	
	x = 10;
	y = 16;
	wl_list_for_each(launcher, &panel->launcher_list, link) {
		w = cairo_image_surface_get_width(launcher->icon);
		h = cairo_image_surface_get_height(launcher->icon);
		widget_set_allocation(launcher->widget,
				      x, y - h / 2, w + 1, h + 1);
		x += w + 10;
	}
	h=20;
	w=170;

	if (panel->clock)
		widget_set_allocation(panel->clock->widget,
				      width - w - 8, y - h / 2, w + 1, h + 1);
}

static void
panel_configure(void *data,
		struct desktop_shell *desktop_shell,
		uint32_t edges, struct window *window,
		int32_t width, int32_t height)
{
	struct surface *surface = window_get_user_data(window);
	struct panel *panel = container_of(surface, struct panel, base);

	window_schedule_resize(panel->window, width, 32);
}

static void
iconlayer_configure(void *data,
		struct desktop_shell *desktop_shell,
		uint32_t edges, struct window *window,
		int32_t width, int32_t height)
{
	struct surface *surface = window_get_user_data(window);
	struct iconlayer *iconlayer = container_of(surface, struct iconlayer, base);

	window_schedule_resize(iconlayer->window, width, height);

}

static struct panel *
panel_create(struct display *display)
{
	struct panel *panel;

	panel = malloc(sizeof *panel);
	memset(panel, 0, sizeof *panel);

	panel->base.configure = panel_configure;
	panel->window = window_create_custom(display);
	panel->widget = window_add_widget(panel->window, panel);
	wl_list_init(&panel->launcher_list);

	window_set_title(panel->window, "panel");
	window_set_user_data(panel->window, panel);

	widget_set_redraw_handler(panel->widget, panel_redraw_handler);
	widget_set_resize_handler(panel->widget, panel_resize_handler);
	widget_set_button_handler(panel->widget, panel_button_handler);
	
	panel_add_clock(panel);

	return panel;
}

static struct widget *
icon_create(struct iconlayer *iconlayer, void *data, unsigned int x, unsigned int y)
{
	struct icon *icon;

	icon = malloc (sizeof *icon);
	memset(icon, 0, sizeof *icon);

	icon->image = data;
	icon->widget = widget_add_widget(iconlayer->widget, icon);
	icon->image = data;
	icon->iconlayer = iconlayer;
	widget_set_allocation(icon->widget, x, y, 96, 64);

	wl_list_insert(iconlayer->icons_list.prev, &icon->link);

	widget_set_redraw_handler(icon->widget, icon_redraw_handler);

	return icon->widget;
}

static struct iconlayer *
iconlayer_create(struct desktop *desktop)
{
	struct iconlayer *iconlayer;

	iconlayer = malloc(sizeof *iconlayer);
	memset(iconlayer, 0, sizeof *iconlayer);

	iconlayer->base.configure = iconlayer_configure;
	iconlayer->window = window_create_custom(desktop->display);
	iconlayer->widget = window_add_widget(iconlayer->window, iconlayer);

	window_set_title(iconlayer->window, "iconlayer");
	window_set_custom(iconlayer->window);
	window_set_user_data(iconlayer->window, iconlayer);

	widget_set_redraw_handler(iconlayer->widget, iconlayer_redraw_handler);
	widget_set_resize_handler(iconlayer->widget, iconlayer_resize_handler);
	widget_set_button_handler(iconlayer->widget, iconlayer_button_handler);
	widget_set_motion_handler(iconlayer->widget, iconlayer_motion_handler);

	iconlayer->image[0] = cairo_image_surface_create_from_png(DATADIR "/weston/folder.png");
	iconlayer->image[1] = cairo_image_surface_create_from_png(DATADIR "/weston/image-x-generic.png");
	iconlayer->image[2] = cairo_image_surface_create_from_png(DATADIR "/weston/package-x-generic.png");
	iconlayer->image[3] = cairo_image_surface_create_from_png(DATADIR "/weston/text-html.png");
	iconlayer->image[4] = cairo_image_surface_create_from_png(DATADIR "/weston/text-x-generic.png");
	iconlayer->image[5] = cairo_image_surface_create_from_png(DATADIR "/weston/text-x-preview.png");
	iconlayer->image[6] = cairo_image_surface_create_from_png(DATADIR "/weston/user-trash.png");
	iconlayer->image[7] = cairo_image_surface_create_from_png(DATADIR "/weston/video-x-generic.png");
	iconlayer->image[8] = cairo_image_surface_create_from_png(DATADIR "/weston/x-office-document.png");

	wl_list_init(&iconlayer->icons_list);

	int q;
	for (q = 0; q < 470; q++) {
		icon_create(iconlayer, iconlayer->image[q % 9],
				(rand() & 63) * 16, (rand() & 31) * 16);
	}

	return iconlayer;
}

static void
panel_add_launcher(struct panel *panel, const char *icon, const char *path)
{
	struct panel_launcher *launcher;

	launcher = malloc(sizeof *launcher);
	memset(launcher, 0, sizeof *launcher);
	launcher->icon = cairo_image_surface_create_from_png(icon);
	launcher->path = strdup(path);
	launcher->panel = panel;
	wl_list_insert(panel->launcher_list.prev, &launcher->link);

	launcher->widget = widget_add_widget(panel->widget, launcher);
	widget_set_enter_handler(launcher->widget,
				 panel_launcher_enter_handler);
	widget_set_leave_handler(launcher->widget,
				   panel_launcher_leave_handler);
	widget_set_button_handler(launcher->widget,
				    panel_launcher_button_handler);
	widget_set_redraw_handler(launcher->widget,
				  panel_launcher_redraw_handler);
	widget_set_motion_handler(launcher->widget,
				  panel_launcher_motion_handler);
}

enum {
	BACKGROUND_SCALE,
	BACKGROUND_TILE
};

static void
background_draw(struct widget *widget, void *data)
{
	struct background *background = data;
	cairo_surface_t *surface, *image;
	cairo_pattern_t *pattern;
	cairo_matrix_t matrix;
	cairo_t *cr;
	double sx, sy;
	struct rectangle allocation;
	int type = -1;

	surface = window_get_surface(background->window);

	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.2, 1.0);
	cairo_paint(cr);

	widget_get_allocation(widget, &allocation);
	image = NULL;
	if (key_background_image)
		image = load_cairo_surface(key_background_image);

	if (strcmp(key_background_type, "scale") == 0)
		type = BACKGROUND_SCALE;
	else if (strcmp(key_background_type, "tile") == 0)
		type = BACKGROUND_TILE;
	else
		fprintf(stderr, "invalid background-type: %s\n",
			key_background_type);

	if (image && type != -1) {
		pattern = cairo_pattern_create_for_surface(image);
		switch (type) {
		case BACKGROUND_SCALE:
			sx = (double) cairo_image_surface_get_width(image) /
				allocation.width;
			sy = (double) cairo_image_surface_get_height(image) /
				allocation.height;
			cairo_matrix_init_scale(&matrix, sx, sy);
			cairo_pattern_set_matrix(pattern, &matrix);
			break;
		case BACKGROUND_TILE:
			cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
			break;
		}
		cairo_set_source(cr, pattern);
		cairo_pattern_destroy (pattern);
		cairo_surface_destroy(image);
	} else {
		set_hex_color(cr, key_background_color);
	}

	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
background_configure(void *data,
		     struct desktop_shell *desktop_shell,
		     uint32_t edges, struct window *window,
		     int32_t width, int32_t height)
{
	struct background *background =
		(struct background *) window_get_user_data(window);

	widget_schedule_resize(background->widget, width, height);
}

static void
unlock_dialog_redraw_handler(struct widget *widget, void *data)
{
	struct unlock_dialog *dialog = data;
	struct rectangle allocation;
	cairo_t *cr;
	cairo_surface_t *surface;
	cairo_pattern_t *pat;
	double cx, cy, r, f;

	surface = window_get_surface(dialog->window);
	cr = cairo_create(surface);

	widget_get_allocation(dialog->widget, &allocation);
	cairo_rectangle(cr, allocation.x, allocation.y,
			allocation.width, allocation.height);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
	cairo_fill(cr);

	cairo_translate(cr, allocation.x, allocation.y);
	if (dialog->button_focused)
		f = 1.0;
	else
		f = 0.7;

	cx = allocation.width / 2.0;
	cy = allocation.height / 2.0;
	r = (cx < cy ? cx : cy) * 0.4;
	pat = cairo_pattern_create_radial(cx, cy, r * 0.7, cx, cy, r);
	cairo_pattern_add_color_stop_rgb(pat, 0.0, 0, 0.86 * f, 0);
	cairo_pattern_add_color_stop_rgb(pat, 0.85, 0.2 * f, f, 0.2 * f);
	cairo_pattern_add_color_stop_rgb(pat, 1.0, 0, 0.86 * f, 0);
	cairo_set_source(cr, pat);
	cairo_pattern_destroy(pat);
	cairo_arc(cr, cx, cy, r, 0.0, 2.0 * M_PI);
	cairo_fill(cr);

	widget_set_allocation(dialog->button,
			      allocation.x + cx - r,
			      allocation.y + cy - r, 2 * r, 2 * r);

	cairo_destroy(cr);

	cairo_surface_destroy(surface);
}

static void
unlock_dialog_button_handler(struct widget *widget,
			     struct input *input, uint32_t time,
			     uint32_t button,
			     enum wl_pointer_button_state state, void *data)
{
	struct unlock_dialog *dialog = data;
	struct desktop *desktop = dialog->desktop;

	if (button == BTN_LEFT) {
		if (state == WL_POINTER_BUTTON_STATE_RELEASED &&
		    !dialog->closing) {
			display_defer(desktop->display, &desktop->unlock_task);
			dialog->closing = 1;
		}
	}
}

static void
unlock_dialog_keyboard_focus_handler(struct window *window,
				     struct input *device, void *data)
{
	window_schedule_redraw(window);
}

static int
unlock_dialog_widget_enter_handler(struct widget *widget,
				   struct input *input,
				   float x, float y, void *data)
{
	struct unlock_dialog *dialog = data;

	dialog->button_focused = 1;
	widget_schedule_redraw(widget);

	return CURSOR_LEFT_PTR;
}

static void
unlock_dialog_widget_leave_handler(struct widget *widget,
				   struct input *input, void *data)
{
	struct unlock_dialog *dialog = data;

	dialog->button_focused = 0;
	widget_schedule_redraw(widget);
}

static struct unlock_dialog *
unlock_dialog_create(struct desktop *desktop)
{
	struct display *display = desktop->display;
	struct unlock_dialog *dialog;

	dialog = malloc(sizeof *dialog);
	if (!dialog)
		return NULL;
	memset(dialog, 0, sizeof *dialog);

	dialog->window = window_create_custom(display);
	dialog->widget = frame_create(dialog->window, dialog);
	window_set_title(dialog->window, "Unlock your desktop");

	window_set_user_data(dialog->window, dialog);
	window_set_keyboard_focus_handler(dialog->window,
					  unlock_dialog_keyboard_focus_handler);
	dialog->button = widget_add_widget(dialog->widget, dialog);
	widget_set_redraw_handler(dialog->widget,
				  unlock_dialog_redraw_handler);
	widget_set_enter_handler(dialog->button,
				 unlock_dialog_widget_enter_handler);
	widget_set_leave_handler(dialog->button,
				 unlock_dialog_widget_leave_handler);
	widget_set_button_handler(dialog->button,
				  unlock_dialog_button_handler);

	desktop_shell_set_lock_surface(desktop->shell,
				       window_get_wl_surface(dialog->window));

	window_schedule_resize(dialog->window, 260, 230);

	return dialog;
}

static void
unlock_dialog_destroy(struct unlock_dialog *dialog)
{
	window_destroy(dialog->window);
	free(dialog);
}

static void
unlock_dialog_finish(struct task *task, uint32_t events)
{
	struct desktop *desktop =
		container_of(task, struct desktop, unlock_task);

	desktop_shell_unlock(desktop->shell);
	unlock_dialog_destroy(desktop->unlock_dialog);
	desktop->unlock_dialog = NULL;
}

static void
desktop_shell_configure(void *data,
			struct desktop_shell *desktop_shell,
			uint32_t edges,
			struct wl_surface *surface,
			int32_t width, int32_t height)
{
	struct window *window = wl_surface_get_user_data(surface);
	struct surface *s = window_get_user_data(window);

	s->configure(data, desktop_shell, edges, window, width, height);
}

static void
desktop_shell_prepare_lock_surface(void *data,
				   struct desktop_shell *desktop_shell)
{
	struct desktop *desktop = data;

	if (!key_locking) {
		desktop_shell_unlock(desktop->shell);
		return;
	}

	if (!desktop->unlock_dialog) {
		desktop->unlock_dialog = unlock_dialog_create(desktop);
		desktop->unlock_dialog->desktop = desktop;
	}
}

static void
desktop_shell_grab_cursor(void *data,
			  struct desktop_shell *desktop_shell,
			  uint32_t cursor)
{
	struct desktop *desktop = data;

	switch (cursor) {
	case DESKTOP_SHELL_CURSOR_BUSY:
		desktop->grab_cursor = CURSOR_WATCH;
		break;
	case DESKTOP_SHELL_CURSOR_MOVE:
		desktop->grab_cursor = CURSOR_DRAGGING;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_TOP:
		desktop->grab_cursor = CURSOR_TOP;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_BOTTOM:
		desktop->grab_cursor = CURSOR_BOTTOM;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_LEFT:
		desktop->grab_cursor = CURSOR_LEFT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_RIGHT:
		desktop->grab_cursor = CURSOR_RIGHT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_TOP_LEFT:
		desktop->grab_cursor = CURSOR_TOP_LEFT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_TOP_RIGHT:
		desktop->grab_cursor = CURSOR_TOP_RIGHT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_BOTTOM_LEFT:
		desktop->grab_cursor = CURSOR_BOTTOM_LEFT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_BOTTOM_RIGHT:
		desktop->grab_cursor = CURSOR_BOTTOM_RIGHT;
		break;
	case DESKTOP_SHELL_CURSOR_ARROW:
	default:
		desktop->grab_cursor = CURSOR_LEFT_PTR;
	}
}

static const struct desktop_shell_listener listener = {
	desktop_shell_configure,
	desktop_shell_prepare_lock_surface,
	desktop_shell_grab_cursor
};

static struct background *
background_create(struct desktop *desktop)
{
	struct background *background;

	background = malloc(sizeof *background);
	memset(background, 0, sizeof *background);

	background->base.configure = background_configure;
	background->window = window_create_custom(desktop->display);
	background->widget = window_add_widget(background->window, background);
	window_set_user_data(background->window, background);
	widget_set_redraw_handler(background->widget, background_draw);

	return background;
}

static int
grab_surface_enter_handler(struct widget *widget, struct input *input,
			   float x, float y, void *data)
{
	struct desktop *desktop = data;

	return desktop->grab_cursor;
}

static void
grab_surface_create(struct desktop *desktop)
{
	struct wl_surface *s;

	desktop->grab_window = window_create(desktop->display);
	window_set_user_data(desktop->grab_window, desktop);

	s = window_get_wl_surface(desktop->grab_window);
	desktop_shell_set_grab_surface(desktop->shell, s);

	desktop->grab_widget =
		window_add_widget(desktop->grab_window, desktop);
	/* We set the allocation to 1x1 at 0,0 so the fake enter event
	 * at 0,0 will go to this widget. */
	widget_set_allocation(desktop->grab_widget, 0, 0, 1, 1);

	widget_set_enter_handler(desktop->grab_widget,
				 grab_surface_enter_handler);
}

static void
create_output(struct desktop *desktop, uint32_t id)
{
	struct output *output;

	output = calloc(1, sizeof *output);
	if (!output)
		return;

	output->output = wl_display_bind(display_get_display(desktop->display),
					 id, &wl_output_interface);

	wl_list_insert(&desktop->outputs, &output->link);
}

static void
global_handler(struct wl_display *display, uint32_t id,
	       const char *interface, uint32_t version, void *data)
{
	struct desktop *desktop = data;

	if (!strcmp(interface, "desktop_shell")) {
		desktop->shell =
			wl_display_bind(display, id, &desktop_shell_interface);
		desktop_shell_add_listener(desktop->shell, &listener, desktop);
	} else if (!strcmp(interface, "wl_output")) {
		create_output(desktop, id);
	}
}

static void
launcher_section_done(void *data)
{
	struct desktop *desktop = data;
	struct output *output;

	if (key_launcher_icon == NULL || key_launcher_path == NULL) {
		fprintf(stderr, "invalid launcher section\n");
		return;
	}

	wl_list_for_each(output, &desktop->outputs, link) {
		panel_add_launcher(output->panel,
				   key_launcher_icon, key_launcher_path);
	}

	free(key_launcher_icon);
	key_launcher_icon = NULL;
	free(key_launcher_path);
	key_launcher_path = NULL;
}

static void
add_default_launcher(struct desktop *desktop)
{
	struct output *output;

	wl_list_for_each(output, &desktop->outputs, link)
		panel_add_launcher(output->panel,
				   DATADIR "/weston/terminal.png",
				   BINDIR "/weston-terminal");
}

int main(int argc, char *argv[])
{
	struct desktop desktop = { 0 };
	char *config_file;
	struct output *output;
	int ret;

	desktop.unlock_task.run = unlock_dialog_finish;
	wl_list_init(&desktop.outputs);

	desktop.display = display_create(argc, argv);
	if (desktop.display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	display_set_user_data(desktop.display, &desktop);
	wl_display_add_global_listener(display_get_display(desktop.display),
				       global_handler, &desktop);

	wl_list_for_each(output, &desktop.outputs, link) {
		struct wl_surface *surface;

		output->panel = panel_create(desktop.display);
		surface = window_get_wl_surface(output->panel->window);
		desktop_shell_set_panel(desktop.shell,
					output->output, surface);

		output->background = background_create(&desktop);
		surface = window_get_wl_surface(output->background->window);
		desktop_shell_set_background(desktop.shell,
					     output->output, surface);

		output->iconlayer = iconlayer_create(&desktop);
		surface = window_get_wl_surface(output->iconlayer->window);
		desktop_shell_set_iconlayer(desktop.shell,
					     output->output, surface);
	}

	grab_surface_create(&desktop);

	config_file = config_file_path("weston.ini");
	ret = parse_config_file(config_file,
				config_sections, ARRAY_LENGTH(config_sections),
				&desktop);
	free(config_file);
	if (ret < 0)
		add_default_launcher(&desktop);

	signal(SIGCHLD, sigchild_handler);

	display_run(desktop.display);

	return 0;
}
