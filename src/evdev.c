/*
 * Copyright © 2010 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <unistd.h>
#include <fcntl.h>
#include <mtdev.h>

#include "compositor.h"
#include "evdev.h"
#include "evdev-private.h"
#include "launcher-util.h"
#include "log.h"

static void
evdev_led_update(struct weston_seat *seat_base, enum weston_led leds)
{
	static const struct {
		enum weston_led weston;
		int evdev;
	} map[] = {
		{ LED_NUM_LOCK, LED_NUML },
		{ LED_CAPS_LOCK, LED_CAPSL },
		{ LED_SCROLL_LOCK, LED_SCROLLL },
	};
	struct evdev_seat *seat = (struct evdev_seat *) seat_base;
	struct evdev_input_device *device;
	struct input_event ev[ARRAY_LENGTH(map)];
	unsigned int i;

	memset(ev, 0, sizeof(ev));
	for (i = 0; i < ARRAY_LENGTH(map); i++) {
		ev[i].type = EV_LED;
		ev[i].code = map[i].evdev;
		ev[i].value = !!(leds & map[i].weston);
	}

	wl_list_for_each(device, &seat->devices_list, link) {
		if (device->caps & EVDEV_KEYBOARD)
			write(device->fd, ev, sizeof ev);
	}
}

static inline void
evdev_process_key(struct evdev_input_device *device,
                        struct input_event *e, int time)
{
	if (e->value == 2)
		return;

	switch (e->code) {
	case BTN_LEFT:
	case BTN_RIGHT:
	case BTN_MIDDLE:
	case BTN_SIDE:
	case BTN_EXTRA:
	case BTN_FORWARD:
	case BTN_BACK:
	case BTN_TASK:
		notify_button(&device->master->base.seat,
			      time, e->code,
			      e->value ? WL_POINTER_BUTTON_STATE_PRESSED :
					 WL_POINTER_BUTTON_STATE_RELEASED);
		break;

	default:
		notify_key(&device->master->base.seat,
			   time, e->code,
			   e->value ? WL_KEYBOARD_KEY_STATE_PRESSED :
				      WL_KEYBOARD_KEY_STATE_RELEASED,
			   STATE_UPDATE_AUTOMATIC);
		break;
	}
}

static void
evdev_process_touch(struct evdev_input_device *device,
		    struct input_event *e)
{
	const int screen_width = device->output->current->width;
	const int screen_height = device->output->current->height;

	switch (e->code) {
	case ABS_MT_SLOT:
		device->mt.slot = e->value;
		break;
	case ABS_MT_TRACKING_ID:
		if (e->value >= 0)
			device->pending_events |= EVDEV_ABSOLUTE_MT_DOWN;
		else
			device->pending_events |= EVDEV_ABSOLUTE_MT_UP;
		break;
	case ABS_MT_POSITION_X:
		device->mt.x[device->mt.slot] =
			(e->value - device->abs.min_x) * screen_width /
			(device->abs.max_x - device->abs.min_x) +
			device->output->x;
		device->pending_events |= EVDEV_ABSOLUTE_MT_MOTION;
		break;
	case ABS_MT_POSITION_Y:
		device->mt.y[device->mt.slot] =
			(e->value - device->abs.min_y) * screen_height /
			(device->abs.max_y - device->abs.min_y) +
			device->output->y;
		device->pending_events |= EVDEV_ABSOLUTE_MT_MOTION;
		break;
	}
}

static inline void
evdev_process_absolute_motion(struct evdev_input_device *device,
			      struct input_event *e)
{
	const int screen_width = device->output->current->width;
	const int screen_height = device->output->current->height;

	switch (e->code) {
	case ABS_X:
		device->abs.x =
			(e->value - device->abs.min_x) * screen_width /
			(device->abs.max_x - device->abs.min_x) +
			device->output->x;
		device->pending_events |= EVDEV_ABSOLUTE_MOTION;
		break;
	case ABS_Y:
		device->abs.y =
			(e->value - device->abs.min_y) * screen_height /
			(device->abs.max_y - device->abs.min_y) +
			device->output->y;
		device->pending_events |= EVDEV_ABSOLUTE_MOTION;
		break;
	}
}

static inline void
evdev_process_relative(struct evdev_input_device *device,
		       struct input_event *e, uint32_t time)
{
	switch (e->code) {
	case REL_X:
		device->rel.dx += wl_fixed_from_int(e->value);
		device->pending_events |= EVDEV_RELATIVE_MOTION;
		break;
	case REL_Y:
		device->rel.dy += wl_fixed_from_int(e->value);
		device->pending_events |= EVDEV_RELATIVE_MOTION;
		break;
	case REL_WHEEL:
		notify_axis(&device->master->base.seat,
			      time,
			      WL_POINTER_AXIS_VERTICAL_SCROLL,
			      wl_fixed_from_int(e->value));
		break;
	case REL_HWHEEL:
		notify_axis(&device->master->base.seat,
			      time,
			      WL_POINTER_AXIS_HORIZONTAL_SCROLL,
			      wl_fixed_from_int(e->value));
		break;
	}
}

static inline void
evdev_process_absolute(struct evdev_input_device *device,
		       struct input_event *e)
{
	if (device->is_mt) {
		evdev_process_touch(device, e);
	} else {
		evdev_process_absolute_motion(device, e);
	}
}

static int
is_motion_event(struct input_event *e)
{
	switch (e->type) {
	case EV_REL:
		switch (e->code) {
		case REL_X:
		case REL_Y:
			return 1;
		}
	case EV_ABS:
		switch (e->code) {
		case ABS_X:
		case ABS_Y:
		case ABS_MT_POSITION_X:
		case ABS_MT_POSITION_Y:
			return 1;
		}
	}

	return 0;
}

static void
evdev_flush_motion(struct evdev_input_device *device, uint32_t time)
{
	struct weston_seat *master = &device->master->base;

	if (!device->pending_events)
		return;

	if (device->pending_events & EVDEV_RELATIVE_MOTION) {
		notify_motion(&master->seat, time,
			      master->seat.pointer->x + device->rel.dx,
			      master->seat.pointer->y + device->rel.dy);
		device->pending_events &= ~EVDEV_RELATIVE_MOTION;
		device->rel.dx = 0;
		device->rel.dy = 0;
	}
	if (device->pending_events & EVDEV_ABSOLUTE_MT_DOWN) {
		notify_touch(&master->seat, time,
			     device->mt.slot,
			     wl_fixed_from_int(device->mt.x[device->mt.slot]),
			     wl_fixed_from_int(device->mt.y[device->mt.slot]),
			     WL_TOUCH_DOWN);
		device->pending_events &= ~EVDEV_ABSOLUTE_MT_DOWN;
		device->pending_events &= ~EVDEV_ABSOLUTE_MT_MOTION;
	}
	if (device->pending_events & EVDEV_ABSOLUTE_MT_MOTION) {
		notify_touch(&master->seat, time,
			     device->mt.slot,
			     wl_fixed_from_int(device->mt.x[device->mt.slot]),
			     wl_fixed_from_int(device->mt.y[device->mt.slot]),
			     WL_TOUCH_MOTION);
		device->pending_events &= ~EVDEV_ABSOLUTE_MT_DOWN;
		device->pending_events &= ~EVDEV_ABSOLUTE_MT_MOTION;
	}
	if (device->pending_events & EVDEV_ABSOLUTE_MT_UP) {
		notify_touch(&master->seat, time, device->mt.slot, 0, 0,
			     WL_TOUCH_UP);
		device->pending_events &= ~EVDEV_ABSOLUTE_MT_UP;
	}
	if (device->pending_events & EVDEV_ABSOLUTE_MOTION) {
		notify_motion(&master->seat, time,
			      wl_fixed_from_int(device->abs.x),
			      wl_fixed_from_int(device->abs.y));
		device->pending_events &= ~EVDEV_ABSOLUTE_MOTION;
	}
}

static void
fallback_process(struct evdev_dispatch *dispatch,
		 struct evdev_input_device *device,
		 struct input_event *event,
		 uint32_t time)
{
	switch (event->type) {
	case EV_REL:
		evdev_process_relative(device, event, time);
		break;
	case EV_ABS:
		evdev_process_absolute(device, event);
		break;
	case EV_KEY:
		evdev_process_key(device, event, time);
		break;
	}
}

static void
fallback_destroy(struct evdev_dispatch *dispatch)
{
	free(dispatch);
}

struct evdev_dispatch_interface fallback_interface = {
	fallback_process,
	fallback_destroy
};

static struct evdev_dispatch *
fallback_dispatch_create(void)
{
	struct evdev_dispatch *dispatch = malloc(sizeof *dispatch);
	if (dispatch == NULL)
		return NULL;

	dispatch->interface = &fallback_interface;

	return dispatch;
}

static void
evdev_process_events(struct evdev_input_device *device,
		     struct input_event *ev, int count)
{
	struct evdev_dispatch *dispatch = device->dispatch;
	struct input_event *e, *end;
	uint32_t time = 0;

	device->pending_events = 0;

	e = ev;
	end = e + count;
	for (e = ev; e < end; e++) {
		time = e->time.tv_sec * 1000 + e->time.tv_usec / 1000;

		/* we try to minimize the amount of notifications to be
		 * forwarded to the compositor, so we accumulate motion
		 * events and send as a bunch */
		if (!is_motion_event(e))
			evdev_flush_motion(device, time);

		dispatch->interface->process(dispatch, device, e, time);
	}

	evdev_flush_motion(device, time);
}

static int
evdev_input_device_data(int fd, uint32_t mask, void *data)
{
	struct weston_compositor *ec;
	struct evdev_input_device *device = data;
	struct input_event ev[32];
	int len;

	ec = device->master->base.compositor;
	if (!ec->focus)
		return 1;

	/* If the compositor is repainting, this function is called only once
	 * per frame and we have to process all the events available on the
	 * fd, otherwise there will be input lag. */
	do {
		if (device->mtdev)
			len = mtdev_get(device->mtdev, fd, ev,
					ARRAY_LENGTH(ev)) *
				sizeof (struct input_event);
		else
			len = read(fd, &ev, sizeof ev);

		if (len < 0 || len % sizeof ev[0] != 0) {
			/* FIXME: call device_removed when errno is ENODEV. */
			return 1;
		}

		evdev_process_events(device, ev, len / sizeof ev[0]);

	} while (len > 0);

	return 1;
}

static int
evdev_configure_device(struct evdev_input_device *device)
{
	struct input_absinfo absinfo;
	unsigned long ev_bits[NBITS(EV_MAX)];
	unsigned long abs_bits[NBITS(ABS_MAX)];
	unsigned long rel_bits[NBITS(REL_MAX)];
	unsigned long key_bits[NBITS(KEY_MAX)];
	int has_key, has_abs;
	unsigned int i;

	has_key = 0;
	has_abs = 0;
	device->caps = 0;

	ioctl(device->fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
	if (TEST_BIT(ev_bits, EV_ABS)) {
		has_abs = 1;

		ioctl(device->fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)),
		      abs_bits);
		if (TEST_BIT(abs_bits, ABS_X)) {
			ioctl(device->fd, EVIOCGABS(ABS_X), &absinfo);
			device->abs.min_x = absinfo.minimum;
			device->abs.max_x = absinfo.maximum;
			device->caps |= EVDEV_MOTION_ABS;
		}
		if (TEST_BIT(abs_bits, ABS_Y)) {
			ioctl(device->fd, EVIOCGABS(ABS_Y), &absinfo);
			device->abs.min_y = absinfo.minimum;
			device->abs.max_y = absinfo.maximum;
			device->caps |= EVDEV_MOTION_ABS;
		}
		if (TEST_BIT(abs_bits, ABS_MT_SLOT)) {
			device->is_mt = 1;
			device->mt.slot = 0;
			device->caps |= EVDEV_TOUCH;
		}
	}
	if (TEST_BIT(ev_bits, EV_REL)) {
		ioctl(device->fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)),
		      rel_bits);
		if (TEST_BIT(rel_bits, REL_X) || TEST_BIT(rel_bits, REL_Y))
			device->caps |= EVDEV_MOTION_REL;
	}
	if (TEST_BIT(ev_bits, EV_KEY)) {
		has_key = 1;
		ioctl(device->fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)),
		      key_bits);
		if (TEST_BIT(key_bits, BTN_TOOL_FINGER) &&
		    !TEST_BIT(key_bits, BTN_TOOL_PEN) &&
		    has_abs)
			device->dispatch = evdev_touchpad_create(device);
		for (i = KEY_ESC; i < KEY_MAX; i++) {
			if (i >= BTN_MISC && i < KEY_OK)
				continue;
			if (TEST_BIT(key_bits, i)) {
				device->caps |= EVDEV_KEYBOARD;
				break;
			}
		}
		for (i = BTN_MISC; i < KEY_OK; i++) {
			if (TEST_BIT(key_bits, i)) {
				device->caps |= EVDEV_BUTTON;
				break;
			}
		}
	}
	if (TEST_BIT(ev_bits, EV_LED)) {
		device->caps |= EVDEV_KEYBOARD;
	}

	/* This rule tries to catch accelerometer devices and opt out. We may
	 * want to adjust the protocol later adding a proper event for dealing
	 * with accelerometers and implement here accordingly */
	if (has_abs && !has_key)
		return -1;

	if ((device->caps &
	     (EVDEV_MOTION_ABS | EVDEV_MOTION_REL | EVDEV_BUTTON)))
		weston_seat_init_pointer(&device->master->base);
	if ((device->caps & EVDEV_KEYBOARD))
		weston_seat_init_keyboard(&device->master->base, NULL);
	if ((device->caps & EVDEV_TOUCH))
		weston_seat_init_touch(&device->master->base);

	return 0;
}

static struct evdev_input_device *
evdev_input_device_create(struct evdev_seat *master,
			  struct wl_display *display, const char *path)
{
	struct evdev_input_device *device;
	struct weston_compositor *ec;

	device = malloc(sizeof *device);
	if (device == NULL)
		return NULL;

	ec = master->base.compositor;
	device->output =
		container_of(ec->output_list.next, struct weston_output, link);

	device->master = master;
	device->is_mt = 0;
	device->mtdev = NULL;
	device->devnode = strdup(path);
	device->mt.slot = -1;
	device->rel.dx = 0;
	device->rel.dy = 0;
	device->dispatch = NULL;

	/* Use non-blocking mode so that we can loop on read on
	 * evdev_input_device_data() until all events on the fd are
	 * read.  mtdev_get() also expects this. */
	device->fd = weston_launcher_open(ec, path, O_RDWR | O_NONBLOCK);
	if (device->fd < 0)
		goto err0;

	if (evdev_configure_device(device) == -1)
		goto err1;

	/* If the dispatch was not set up use the fallback. */
	if (device->dispatch == NULL)
		device->dispatch = fallback_dispatch_create();
	if (device->dispatch == NULL)
		goto err1;


	if (device->is_mt) {
		device->mtdev = mtdev_new_open(device->fd);
		if (!device->mtdev)
			weston_log("mtdev failed to open for %s\n", path);
	}

	device->source = wl_event_loop_add_fd(ec->input_loop, device->fd,
					      WL_EVENT_READABLE,
					      evdev_input_device_data, device);
	if (device->source == NULL)
		goto err2;

	wl_list_insert(master->devices_list.prev, &device->link);

	return device;

err2:
	device->dispatch->interface->destroy(device->dispatch);
err1:
	close(device->fd);
err0:
	free(device->devnode);
	free(device);
	return NULL;
}

static const char default_seat[] = "seat0";

static void
device_added(struct udev_device *udev_device, struct evdev_seat *master)
{
	struct weston_compositor *c;
	const char *devnode;
	const char *device_seat;

	device_seat = udev_device_get_property_value(udev_device, "ID_SEAT");
	if (!device_seat)
		device_seat = default_seat;

	if (strcmp(device_seat, master->seat_id))
		return;

	c = master->base.compositor;
	devnode = udev_device_get_devnode(udev_device);
	evdev_input_device_create(master, c->wl_display, devnode);
}

static void
device_removed(struct evdev_input_device *device)
{
	struct evdev_dispatch *dispatch;

	dispatch = device->dispatch;
	if (dispatch)
		dispatch->interface->destroy(dispatch);

	wl_event_source_remove(device->source);
	wl_list_remove(&device->link);
	if (device->mtdev)
		mtdev_close_delete(device->mtdev);
	close(device->fd);
	free(device->devnode);
	free(device);
}

static void
evdev_notify_keyboard_focus(struct evdev_seat *seat)
{
	struct evdev_input_device *device;
	struct wl_array keys;
	unsigned int i, set;
	char evdev_keys[(KEY_CNT + 7) / 8], all_keys[(KEY_CNT + 7) / 8];
	uint32_t *k;
	int ret;

	memset(all_keys, 0, sizeof all_keys);
	wl_list_for_each(device, &seat->devices_list, link) {
		memset(evdev_keys, 0, sizeof evdev_keys);
		ret = ioctl(device->fd,
			    EVIOCGKEY(sizeof evdev_keys), evdev_keys);
		if (ret < 0) {
			weston_log("failed to get keys for device %s\n",
				device->devnode);
			continue;
		}
		for (i = 0; i < ARRAY_LENGTH(evdev_keys); i++)
			all_keys[i] |= evdev_keys[i];
	}

	wl_array_init(&keys);
	for (i = 0; i < KEY_CNT; i++) {
		set = all_keys[i >> 3] & (1 << (i & 7));
		if (set) {
			k = wl_array_add(&keys, sizeof *k);
			*k = i;
		}
	}

	notify_keyboard_focus_in(&seat->base.seat, &keys,
				 STATE_UPDATE_AUTOMATIC);

	wl_array_release(&keys);
}

void
evdev_add_devices(struct udev *udev, struct weston_seat *seat_base)
{
	struct evdev_seat *seat = (struct evdev_seat *) seat_base;
	struct udev_enumerate *e;
	struct udev_list_entry *entry;
	struct udev_device *device;
	const char *path, *sysname;

	e = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(e, "input");
	udev_enumerate_scan_devices(e);
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		path = udev_list_entry_get_name(entry);
		device = udev_device_new_from_syspath(udev, path);

		sysname = udev_device_get_sysname(device);
		if (strncmp("event", sysname, 5) != 0) {
			udev_device_unref(device);
			continue;
		}

		weston_log("udev evdev: %s %s\n", path, sysname);

		device_added(device, seat);

		udev_device_unref(device);
	}
	udev_enumerate_unref(e);

	evdev_notify_keyboard_focus(seat);

	if (wl_list_empty(&seat->devices_list)) {
		weston_log(
			"warning: no input devices on entering Weston. "
			"Possible causes:\n"
			"\t- no permissions to read /dev/input/event*\n"
			"\t- seats misconfigured "
			"(Weston backend option 'seat', "
			"udev device property ID_SEAT)\n");
	}
}

static int
evdev_udev_handler(int fd, uint32_t mask, void *data)
{
	struct evdev_seat *master = data;
	struct udev_device *udev_device;
	struct evdev_input_device *device, *next;
	const char *action;
	const char *devnode;

	udev_device = udev_monitor_receive_device(master->udev_monitor);
	if (!udev_device)
		return 1;

	action = udev_device_get_action(udev_device);
	if (action) {
		if (strncmp("event", udev_device_get_sysname(udev_device), 5) != 0)
			return 0;

		if (!strcmp(action, "add")) {
			device_added(udev_device, master);
		}
		else if (!strcmp(action, "remove")) {
			devnode = udev_device_get_devnode(udev_device);
			wl_list_for_each_safe(device, next,
					      &master->devices_list, link)
				if (!strcmp(device->devnode, devnode)) {
					device_removed(device);
					break;
				}
		}
	}
	udev_device_unref(udev_device);

	return 0;
}

int
evdev_enable_udev_monitor(struct udev *udev, struct weston_seat *seat_base)
{
	struct evdev_seat *master = (struct evdev_seat *) seat_base;
	struct wl_event_loop *loop;
	struct weston_compositor *c = master->base.compositor;
	int fd;

	master->udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
	if (!master->udev_monitor) {
		weston_log("udev: failed to create the udev monitor\n");
		return 0;
	}

	udev_monitor_filter_add_match_subsystem_devtype(master->udev_monitor,
			"input", NULL);

	if (udev_monitor_enable_receiving(master->udev_monitor)) {
		weston_log("udev: failed to bind the udev monitor\n");
		udev_monitor_unref(master->udev_monitor);
		return 0;
	}

	loop = wl_display_get_event_loop(c->wl_display);
	fd = udev_monitor_get_fd(master->udev_monitor);
	master->udev_monitor_source =
		wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				     evdev_udev_handler, master);
	if (!master->udev_monitor_source) {
		udev_monitor_unref(master->udev_monitor);
		return 0;
	}

	return 1;
}

void
evdev_disable_udev_monitor(struct weston_seat *seat_base)
{
	struct evdev_seat *seat = (struct evdev_seat *) seat_base;

	if (!seat->udev_monitor)
		return;

	udev_monitor_unref(seat->udev_monitor);
	seat->udev_monitor = NULL;
	wl_event_source_remove(seat->udev_monitor_source);
	seat->udev_monitor_source = NULL;
}

void
evdev_input_create(struct weston_compositor *c, struct udev *udev,
		   const char *seat_id)
{
	struct evdev_seat *seat;

	seat = malloc(sizeof *seat);
	if (seat == NULL)
		return;

	memset(seat, 0, sizeof *seat);
	weston_seat_init(&seat->base, c);
	seat->base.led_update = evdev_led_update;

	wl_list_init(&seat->devices_list);
	seat->seat_id = strdup(seat_id);
	if (!evdev_enable_udev_monitor(udev, &seat->base)) {
		free(seat->seat_id);
		free(seat);
		return;
	}

	evdev_add_devices(udev, &seat->base);

	c->seat = &seat->base;
}

void
evdev_remove_devices(struct weston_seat *seat_base)
{
	struct evdev_seat *seat = (struct evdev_seat *) seat_base;
	struct evdev_input_device *device, *next;

	wl_list_for_each_safe(device, next, &seat->devices_list, link)
		device_removed(device);

	notify_keyboard_focus_out(&seat->base.seat);
}

void
evdev_input_destroy(struct weston_seat *seat_base)
{
	struct evdev_seat *seat = (struct evdev_seat *) seat_base;

	evdev_remove_devices(seat_base);
	evdev_disable_udev_monitor(&seat->base);

	wl_list_remove(&seat->base.link);
	free(seat->seat_id);
	free(seat);
}
