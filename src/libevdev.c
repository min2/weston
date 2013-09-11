/*
 * Copyright © 2013 Red Hat, Inc.
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

#include <config.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include "libevdev.h"
#include "libevdev-int.h"
#include "libevdev-util.h"
#include "event-names.h"

#define MAXEVENTS 64

static inline void
init_event(struct libevdev *dev, struct input_event *ev, int type, int code, int value)
{
	ev->time = dev->last_event_time;
	ev->type = type;
	ev->code = code;
	ev->value = value;
}

static void custom_callback(void *ptr, int key, int val)
{
	int emit_fake_events = ptr != NULL;
	if (emit_fake_events) {
		struct libevdev *dev = ptr;
		struct input_event *ev = queue_push(dev);
		init_event(dev, ev, EV_KEY, key, val ? 1 : 0);
	}
}

static int
sync_external_key_state(struct libevdev *dev, int emit_fake_events)
{
	int rc;
	const int s = ((KEY_CNT + 7) / 8);
	unsigned long *keystate = malloc(s);
	void *ptr = emit_fake_events ? (void *) dev : NULL;

	if (keystate == NULL)
		return -1;

	rc = ioctl(dev->fd, EVIOCGKEY(s), keystate);
	if (rc < 0)
		goto out;

	dev->interface->sync(dev->external_key_values,
			dev->key_values_bit_id, dev->key_values_id,
			&keystate[0], &keystate[NLONGS(KEY_MAX)],
			ptr, &custom_callback);

	rc = 0;
out:
	return rc ? -errno : 0;
}

static int bitmap_keyboard_keys_get_update(void *external, unsigned long bit_id, unsigned int id, uint32_t key, int val)
{
	struct libevdev_keys_bitmap *bmp = external;
	int i = bit_is_set(bmp->key_values, key);
	set_bit_state(bmp->key_values, key, val);
	return -(i == val);
}

static int bitmap_keyboard_keys_get(void *external, unsigned long bit_id, unsigned int id, uint32_t key)
{
	struct libevdev_keys_bitmap *bmp = external;
	return bit_is_set(bmp->key_values, key);
}

static void bitmap_keyboard_keys_sync(void *external, unsigned long bit_id, unsigned int id, unsigned long *buf, unsigned long *buf_end, void *ptr, void (*callback)(void *ptr, int key, int val))
{
	unsigned int i;
	struct libevdev_keys_bitmap *bmp = external;

	for (i = 0; i < KEY_CNT; i++) {
		int old, new;
		old = bit_is_set(bmp->key_values, i);
		new = bit_is_set(buf, i);
		if (old ^ new) {
			callback(ptr, i, new);
		}
	}

	/* zero-copy */
	free(bmp->key_values);
	bmp->key_values = buf;
}

static void bitmap_keyboard_keys_deactivate(void *external, unsigned long int bit_id, unsigned int id)
{
}
static int bitmap_keyboard_keys_activate(void *external, unsigned long int *bit_id, unsigned int *id)
{
	*bit_id = 1;
	*id = 0;
	return 0;
}


struct libevdev_external_key_values_interface bitmap_state_interface = {
	NULL,
	NULL,
	&bitmap_keyboard_keys_get_update,
	&bitmap_keyboard_keys_get,
	NULL,
	NULL,
	NULL,
	&bitmap_keyboard_keys_sync,
	&bitmap_keyboard_keys_deactivate,
	&bitmap_keyboard_keys_activate
};

static int sync_mt_state(struct libevdev *dev, int create_events);

static int
init_event_queue(struct libevdev *dev)
{
	/* FIXME: count the number of axes, keys, etc. to get a better idea at how many events per
	   EV_SYN we could possibly get. Then multiply that by the actual buffer size we care about */

	const int QUEUE_SIZE = 256;

	return queue_alloc(dev, QUEUE_SIZE);
}

static void
libevdev_dflt_log_func(enum libevdev_log_priority priority,
		       void *data,
		       const char *file, int line, const char *func,
		       const char *format, va_list args)
{
	const char *prefix;
	switch(priority) {
		case LIBEVDEV_LOG_ERROR: prefix = "libevdev error"; break;
		case LIBEVDEV_LOG_INFO: prefix = "libevdev info"; break;
		case LIBEVDEV_LOG_DEBUG:
					prefix = "libevdev debug";
					break;
		default:
					break;
	}
	/* default logging format:
	   libevev error in libevdev_some_func: blah blah
	   libevev info in libevdev_some_func: blah blah
	   libevev debug in file.c:123:libevdev_some_func: blah blah
	 */

	fprintf(stderr, "%s in ", prefix);
	if (priority == LIBEVDEV_LOG_DEBUG)
		fprintf(stderr, "%s:%d:", file, line);
	fprintf(stderr, "%s: ", func);
	vfprintf(stderr, format, args);
}

/*
 * Global logging settings.
 */
struct logdata log_data = {
	LIBEVDEV_LOG_INFO,
	libevdev_dflt_log_func,
	NULL,
};

void
log_msg(enum libevdev_log_priority priority,
	void *data,
	const char *file, int line, const char *func,
	const char *format, ...)
{
	va_list args;

	if (!log_data.handler)
		return;

	va_start(args, format);
	log_data.handler(priority, data, file, line, func, format, args);
	va_end(args);
}

LIBEVDEV_EXPORT struct libevdev*
libevdev_new(void)
{
	struct libevdev *dev;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;
	dev->fd = -1;
	dev->num_slots = -1;
	dev->current_slot = -1;
	dev->grabbed = LIBEVDEV_UNGRAB;
	dev->sync_state = SYNC_NONE;

	dev->external_key_values = NULL;
	dev->interface = NULL;
	dev->key_values_bit_id = 0;
	dev->key_values_id = 0;

	return dev;
}

static int
libevdev_init_legacy_bitmap_keys(struct libevdev* dev, int fd)
{
	size_t s = (KEY_CNT + 7) / 8;
	unsigned long *buf;

	if ((dev->interface == NULL) && (dev->external_key_values == NULL)) {
		buf = calloc(1, s);

		if (buf != NULL) {
			struct libevdev_keys_bitmap *wrapper =
				malloc(sizeof(struct libevdev_keys_bitmap));
			if (wrapper != NULL) {
				wrapper->key_values = buf;

				if (libevdev_external_key_values_activate(dev,
					&bitmap_state_interface, wrapper))
					return -2;

				if (libevdev_has_event_type(dev, EV_KEY))
					sync_external_key_state(dev, 0);

			} else {
				free(buf);
				return -1;
			}
		}
	}


	return 0;
}

LIBEVDEV_EXPORT int
libevdev_new_from_fd(int fd, struct libevdev **dev)
{
	struct libevdev *d;
	int rc;

	d = libevdev_new();
	if (!d)
		return -ENOMEM;

	if (libevdev_init_legacy_bitmap_keys(d, fd))
		return -ENOMEM;

	rc = libevdev_set_fd(d, fd);
	if (rc < 0)
		libevdev_free(d);
	else
		*dev = d;
	return rc;
}

int libevdev_external_key_values_activate(
	struct libevdev *dev,
	struct libevdev_external_key_values_interface *interface,
	void *external_structure
)
{
	if (dev->external_key_values != NULL)
		return -2;

	dev->interface = interface;
	dev->external_key_values = external_structure;

	if (interface->activate(dev->external_key_values,
				&dev->key_values_bit_id, &dev->key_values_id))
		return -1;

	return 0;
}

void libevdev_external_key_values_deactivate(struct libevdev *dev)
{
	dev->interface->deactivate(dev->external_key_values,
				   dev->key_values_bit_id, dev->key_values_id);
	dev->interface = NULL;
	dev->external_key_values = NULL;
	dev->key_values_bit_id = 0;
	dev->key_values_id = 0;
}

LIBEVDEV_EXPORT void
libevdev_free(struct libevdev *dev)
{
	if (!dev)
		return;

	if (dev->interface == &bitmap_state_interface) {

		struct libevdev_keys_bitmap *wrapper =
						(struct libevdev_keys_bitmap *)
						dev->external_key_values;
		free(wrapper->key_values);
		free(wrapper);
	}

	free(dev->name);
	free(dev->phys);
	free(dev->uniq);
	queue_free(dev);
	free(dev);
}

/* DEPRECATED */
LIBEVDEV_EXPORT void
libevdev_set_log_handler(struct libevdev *dev, libevdev_log_func_t logfunc)
{
	/* Can't be backwards compatible to this yet, so don't even try */
	fprintf(stderr, "libevdev: ABI change. Log function will not be honored.\n");
}

LIBEVDEV_EXPORT void
libevdev_set_log_function(libevdev_log_func_t logfunc, void *data)
{
	log_data.handler = logfunc;
	log_data.userdata = data;
}

LIBEVDEV_EXPORT void
libevdev_set_log_priority(enum libevdev_log_priority priority)
{
	if (priority > LIBEVDEV_LOG_DEBUG)
		priority = LIBEVDEV_LOG_DEBUG;
	log_data.priority = priority;
}

LIBEVDEV_EXPORT enum libevdev_log_priority
libevdev_get_log_priority(void)
{
	return log_data.priority;
}

LIBEVDEV_EXPORT int
libevdev_change_fd(struct libevdev *dev, int fd)
{
	if (dev->fd == -1) {
		log_bug("device not initialized. call libevdev_set_fd() first\n");
		return -1;
	}
	dev->fd = fd;
	return 0;
}

LIBEVDEV_EXPORT int
libevdev_set_fd(struct libevdev* dev, int fd)
{
	int rc;
	int i;
	char buf[256];

	if (libevdev_init_legacy_bitmap_keys(dev, fd)) {
		errno = ENOMEM;
		goto out;
	}

	if (dev->fd != -1) {
		log_bug("device already initialized.\n");
		return -EBADF;
	}

	rc = ioctl(fd, EVIOCGBIT(0, sizeof(dev->bits)), dev->bits);
	if (rc < 0)
		goto out;

	memset(buf, 0, sizeof(buf));
	rc = ioctl(fd, EVIOCGNAME(sizeof(buf) - 1), buf);
	if (rc < 0)
		goto out;

	free(dev->name);
	dev->name = strdup(buf);
	if (!dev->name) {
		errno = ENOMEM;
		goto out;
	}

	free(dev->phys);
	dev->phys = NULL;
	memset(buf, 0, sizeof(buf));
	rc = ioctl(fd, EVIOCGPHYS(sizeof(buf) - 1), buf);
	if (rc < 0) {
		/* uinput has no phys */
		if (errno != ENOENT)
			goto out;
	} else {
		dev->phys = strdup(buf);
		if (!dev->phys) {
			errno = ENOMEM;
			goto out;
		}
	}

	free(dev->uniq);
	dev->uniq = NULL;
	memset(buf, 0, sizeof(buf));
	rc = ioctl(fd, EVIOCGUNIQ(sizeof(buf) - 1), buf);
	if (rc < 0) {
		if (errno != ENOENT)
			goto out;
	} else  {
		dev->uniq = strdup(buf);
		if (!dev->uniq) {
			errno = ENOMEM;
			goto out;
		}
	}

	rc = ioctl(fd, EVIOCGID, &dev->ids);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGVERSION, &dev->driver_version);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGPROP(sizeof(dev->props)), dev->props);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGBIT(EV_REL, sizeof(dev->rel_bits)), dev->rel_bits);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(dev->abs_bits)), dev->abs_bits);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGBIT(EV_LED, sizeof(dev->led_bits)), dev->led_bits);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(dev->key_bits)), dev->key_bits);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGBIT(EV_SW, sizeof(dev->sw_bits)), dev->sw_bits);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGBIT(EV_MSC, sizeof(dev->msc_bits)), dev->msc_bits);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGBIT(EV_FF, sizeof(dev->ff_bits)), dev->ff_bits);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGBIT(EV_SND, sizeof(dev->snd_bits)), dev->snd_bits);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGLED(sizeof(dev->led_values)), dev->led_values);
	if (rc < 0)
		goto out;

	rc = ioctl(fd, EVIOCGSW(sizeof(dev->sw_values)), dev->sw_values);
	if (rc < 0)
		goto out;

	/* rep is a special case, always set it to 1 for both values if EV_REP is set */
	if (bit_is_set(dev->bits, EV_REP)) {
		for (i = 0; i < REP_CNT; i++)
			set_bit(dev->rep_bits, i);
		rc = ioctl(fd, EVIOCGREP, dev->rep_values);
		if (rc < 0)
			goto out;
	}

	for (i = ABS_X; i <= ABS_MAX; i++) {
		if (bit_is_set(dev->abs_bits, i)) {
			struct input_absinfo abs_info;
			rc = ioctl(fd, EVIOCGABS(i), &abs_info);
			if (rc < 0)
				goto out;

			dev->abs_info[i] = abs_info;
			if (i == ABS_MT_SLOT) {
				dev->num_slots = abs_info.maximum + 1;
				dev->current_slot = abs_info.value;
			}

		}
	}

	dev->fd = fd;
	sync_mt_state(dev, 0);

	rc = init_event_queue(dev);
	if (rc < 0) {
		dev->fd = -1;
		return -rc;
	}

	/* not copying key state because we won't know when we'll start to
	 * use this fd and key's are likely to change state by then.
	 * Same with the valuators, really, but they may not change.
	 */

out:
	return rc < 0 ? -errno : 0;
}

LIBEVDEV_EXPORT int
libevdev_get_fd(const struct libevdev* dev)
{
	return dev->fd;
}

static int
sync_key_state(struct libevdev *dev)
{
	sync_external_key_state(dev, 1);
}

static int
sync_sw_state(struct libevdev *dev)
{
	int rc;
	int i;
	unsigned long swstate[NLONGS(SW_CNT)] = {0};

	rc = ioctl(dev->fd, EVIOCGSW(sizeof(swstate)), swstate);
	if (rc < 0)
		goto out;

	for (i = 0; i < SW_CNT; i++) {
		int old, new;
		old = bit_is_set(dev->sw_values, i);
		new = bit_is_set(swstate, i);
		if (old ^ new) {
			struct input_event *ev = queue_push(dev);
			init_event(dev, ev, EV_SW, i, new ? 1 : 0);
		}
	}

	memcpy(dev->sw_values, swstate, rc);

	rc = 0;
out:
	return rc ? -errno : 0;
}

static int
sync_led_state(struct libevdev *dev)
{
	int rc;
	int i;
	unsigned long ledstate[NLONGS(LED_CNT)] = {0};

	rc = ioctl(dev->fd, EVIOCGLED(sizeof(ledstate)), ledstate);
	if (rc < 0)
		goto out;

	for (i = 0; i < LED_CNT; i++) {
		int old, new;
		old = bit_is_set(dev->led_values, i);
		new = bit_is_set(ledstate, i);
		if (old ^ new) {
			struct input_event *ev = queue_push(dev);
			init_event(dev, ev, EV_LED, i, new ? 1 : 0);
		}
	}

	memcpy(dev->led_values, ledstate, rc);

	rc = 0;
out:
	return rc ? -errno : 0;
}
static int
sync_abs_state(struct libevdev *dev)
{
	int rc;
	int i;

	for (i = ABS_X; i < ABS_CNT; i++) {
		struct input_absinfo abs_info;

		if (i >= ABS_MT_MIN && i <= ABS_MT_MAX)
			continue;

		if (!bit_is_set(dev->abs_bits, i))
			continue;

		rc = ioctl(dev->fd, EVIOCGABS(i), &abs_info);
		if (rc < 0)
			goto out;

		if (dev->abs_info[i].value != abs_info.value) {
			struct input_event *ev = queue_push(dev);

			init_event(dev, ev, EV_ABS, i, abs_info.value);
			dev->abs_info[i].value = abs_info.value;
		}
	}

	rc = 0;
out:
	return rc ? -errno : 0;
}

static int
sync_mt_state(struct libevdev *dev, int create_events)
{
	int rc;
	int i;
	struct mt_state {
		int code;
		int val[MAX_SLOTS];
	} mt_state[ABS_MT_CNT];

	for (i = ABS_MT_MIN; i <= ABS_MT_MAX; i++) {
		int idx;
		if (i == ABS_MT_SLOT)
			continue;

		if (!libevdev_has_event_code(dev, EV_ABS, i))
			continue;

		idx = i - ABS_MT_MIN;
		mt_state[idx].code = i;
		rc = ioctl(dev->fd, EVIOCGMTSLOTS(sizeof(struct mt_state)), &mt_state[idx]);
		if (rc < 0)
			goto out;
	}

	for (i = 0; i < dev->num_slots; i++) {
		int j;
		struct input_event *ev;

		if (create_events) {
			ev = queue_push(dev);
			init_event(dev, ev, EV_ABS, ABS_MT_SLOT, i);
		}

		for (j = ABS_MT_MIN; j <= ABS_MT_MAX; j++) {
			int jdx = j - ABS_MT_MIN;

			if (j == ABS_MT_SLOT)
				continue;

			if (!libevdev_has_event_code(dev, EV_ABS, j))
				continue;

			if (dev->mt_slot_vals[i][jdx] == mt_state[jdx].val[i])
				continue;

			if (create_events) {
				ev = queue_push(dev);
				init_event(dev, ev, EV_ABS, j, mt_state[jdx].val[i]);
			}
			dev->mt_slot_vals[i][jdx] = mt_state[jdx].val[i];
		}
	}

	rc = 0;
out:
	return rc ? -errno : 0;
}

static int
sync_state(struct libevdev *dev)
{
	int i;
	int rc = 0;
	struct input_event *ev;

	/* FIXME: if we have events in the queue after the SYN_DROPPED (which was
	   queue[0]) we need to shift this backwards. Except that chances are that the
	   queue may be either full or too full to prepend all the events needed for
	   SYNC_IN_PROGRESS.

	   so we search for the last sync event in the queue and drop everything before
	   including that event and rely on the kernel to tell us the right value for that
	   bitfield during the sync process.
	 */

	for (i = queue_num_elements(dev) - 1; i >= 0; i--) {
		struct input_event e = {{0,0}, 0, 0, 0};
		queue_peek(dev, i, &e);
		if (e.type == EV_SYN)
			break;
	}

	if (i > 0)
		queue_shift_multiple(dev, i + 1, NULL);

	if (libevdev_has_event_type(dev, EV_KEY))
		rc = sync_key_state(dev);
	if (libevdev_has_event_type(dev, EV_LED))
		rc = sync_led_state(dev);
	if (libevdev_has_event_type(dev, EV_SW))
		rc = sync_sw_state(dev);
	if (rc == 0 && libevdev_has_event_type(dev, EV_ABS))
		rc = sync_abs_state(dev);
	if (rc == 0 && libevdev_has_event_code(dev, EV_ABS, ABS_MT_SLOT))
		rc = sync_mt_state(dev, 1);

	dev->queue_nsync = queue_num_elements(dev);

	if (dev->queue_nsync > 0) {
		ev = queue_push(dev);
		init_event(dev, ev, EV_SYN, SYN_REPORT, 0);
		dev->queue_nsync++;
	}

	return rc;
}

static int
update_key_state(struct libevdev *dev, const struct input_event *e)
{
	if (!libevdev_has_event_type(dev, EV_KEY))
		return 1;

	if (e->code > KEY_MAX)
		return 1;

	return dev->interface->get_update(dev->external_key_values,
					dev->key_values_bit_id,
					dev->key_values_id,
					e->code, e->value != 0);
}

static int
update_mt_state(struct libevdev *dev, const struct input_event *e)
{
	if (e->code == ABS_MT_SLOT) {
		int i;
		dev->current_slot = e->value;
		/* sync abs_info with the current slot values */
		for (i = ABS_MT_SLOT + 1; i <= ABS_MT_MAX; i++) {
			if (libevdev_has_event_code(dev, EV_ABS, i))
				dev->abs_info[i].value = dev->mt_slot_vals[dev->current_slot][i - ABS_MT_MIN];
		}

		return 0;
	} else if (dev->current_slot == -1)
		return 1;

	dev->mt_slot_vals[dev->current_slot][e->code - ABS_MT_MIN] = e->value;

	return 0;
}

static int
update_abs_state(struct libevdev *dev, const struct input_event *e)
{
	if (!libevdev_has_event_type(dev, EV_ABS))
		return 1;

	if (e->code > ABS_MAX)
		return 1;

	if (e->code >= ABS_MT_MIN && e->code <= ABS_MT_MAX)
		update_mt_state(dev, e);

	dev->abs_info[e->code].value = e->value;

	return 0;
}

static int
update_led_state(struct libevdev *dev, const struct input_event *e)
{
	if (!libevdev_has_event_type(dev, EV_LED))
		return 1;

	if (e->code > LED_MAX)
		return 1;

	set_bit_state(dev->led_values, e->code, e->value != 0);

	return 0;
}

static int
update_sw_state(struct libevdev *dev, const struct input_event *e)
{
	if (!libevdev_has_event_type(dev, EV_SW))
		return 1;

	if (e->code > SW_MAX)
		return 1;

	set_bit_state(dev->sw_values, e->code, e->value != 0);

	return 0;
}

static int
update_state(struct libevdev *dev, const struct input_event *e)
{
	int rc = 0;

	switch(e->type) {
		case EV_SYN:
		case EV_REL:
			break;
		case EV_KEY:
			rc = update_key_state(dev, e);
			break;
		case EV_ABS:
			rc = update_abs_state(dev, e);
			break;
		case EV_LED:
			rc = update_led_state(dev, e);
			break;
		case EV_SW:
			rc = update_sw_state(dev, e);
			break;
	}

	dev->last_event_time = e->time;

	return rc;
}

static int
read_more_events(struct libevdev *dev)
{
	int free_elem;
	int len;
	struct input_event *next;

	free_elem = queue_num_free_elements(dev);
	if (free_elem <= 0)
		return 0;

	next = queue_next_element(dev);
	len = read(dev->fd, next, free_elem * sizeof(struct input_event));
	if (len < 0) {
		return -errno;
	} else if (len > 0 && len % sizeof(struct input_event) != 0)
		return -EINVAL;
	else if (len > 0) {
		int nev = len/sizeof(struct input_event);
		queue_set_num_elements(dev, queue_num_elements(dev) + nev);
	}

	return 0;
}

LIBEVDEV_EXPORT int
libevdev_next_event(struct libevdev *dev, unsigned int flags, struct input_event *ev)
{
	int rc = 0;

	if (dev->fd < 0) {
		log_bug("device not initialized. call libevdev_set_fd() first\n");
		return -EBADF;
	}

	if (!(flags & (LIBEVDEV_READ_NORMAL|LIBEVDEV_READ_SYNC|LIBEVDEV_FORCE_SYNC))) {
		log_bug("invalid flags %#x\n.\n", flags);
		return -EINVAL;
	}

	if (flags & LIBEVDEV_READ_SYNC) {
		if (dev->sync_state == SYNC_NEEDED) {
			rc = sync_state(dev);
			if (rc != 0)
				return rc;
			dev->sync_state = SYNC_IN_PROGRESS;
		}

		if (dev->queue_nsync == 0) {
			dev->sync_state = SYNC_NONE;
			return -EAGAIN;
		}

	} else if (dev->sync_state != SYNC_NONE) {
		struct input_event e;

		/* call update_state for all events here, otherwise the library has the wrong view
		   of the device too */
		while (queue_shift(dev, &e) == 0) {
			dev->queue_nsync--;
			update_state(dev, &e);
		}

		dev->sync_state = SYNC_NONE;
	}

	/* FIXME: if the first event after SYNC_IN_PROGRESS is a SYN_DROPPED, log this */

	/* Always read in some more events. Best case this smoothes over a potential SYN_DROPPED,
	   worst case we don't read fast enough and end up with SYN_DROPPED anyway.

	   Except if the fd is in blocking mode and we still have events from the last read, don't
	   read in any more.
	 */
	do {
		if (!(flags & LIBEVDEV_READ_BLOCKING) ||
		    queue_num_elements(dev) == 0) {
			rc = read_more_events(dev);
			if (rc < 0 && rc != -EAGAIN)
				goto out;
		}

		if (flags & LIBEVDEV_FORCE_SYNC) {
			dev->sync_state = SYNC_NEEDED;
			rc = 1;
			goto out;
		}


		if (queue_shift(dev, ev) != 0)
			return -EAGAIN;

		update_state(dev, ev);

	/* if we disabled a code, get the next event instead */
	} while(!libevdev_has_event_code(dev, ev->type, ev->code));

	rc = 0;
	if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
		dev->sync_state = SYNC_NEEDED;
		rc = 1;
	}

	if (flags & LIBEVDEV_READ_SYNC && dev->queue_nsync > 0) {
		dev->queue_nsync--;
		rc = 1;
		if (dev->queue_nsync == 0)
			dev->sync_state = SYNC_NONE;
	}

out:
	return rc;
}

LIBEVDEV_EXPORT int
libevdev_has_event_pending(struct libevdev *dev)
{
	struct pollfd fds = { dev->fd, POLLIN, 0 };
	int rc;

	if (dev->fd < 0) {
		log_bug("device not initialized. call libevdev_set_fd() first\n");
		return -EBADF;
	}

	if (queue_num_elements(dev) != 0)
		return 1;

	rc = poll(&fds, 1, 0);
	return (rc >= 0) ? rc : -errno;
}

LIBEVDEV_EXPORT const char *
libevdev_get_name(const struct libevdev *dev)
{
	return dev->name ? dev->name : "";
}

LIBEVDEV_EXPORT const char *
libevdev_get_phys(const struct libevdev *dev)
{
	return dev->phys;
}

LIBEVDEV_EXPORT const char *
libevdev_get_uniq(const struct libevdev *dev)
{
	return dev->uniq;
}

#define STRING_SETTER(field) \
LIBEVDEV_EXPORT void libevdev_set_##field(struct libevdev *dev, const char *field) \
{ \
	if (field == NULL) \
		return; \
	free(dev->field); \
	dev->field = strdup(field); \
}

STRING_SETTER(name);
STRING_SETTER(phys);
STRING_SETTER(uniq);


#define PRODUCT_GETTER(name) \
LIBEVDEV_EXPORT int libevdev_get_id_##name(const struct libevdev *dev) \
{ \
	return dev->ids.name; \
}

PRODUCT_GETTER(product);
PRODUCT_GETTER(vendor);
PRODUCT_GETTER(bustype);
PRODUCT_GETTER(version);

#define PRODUCT_SETTER(field) \
LIBEVDEV_EXPORT void libevdev_set_id_##field(struct libevdev *dev, int field) \
{ \
	dev->ids.field = field;\
}

PRODUCT_SETTER(product);
PRODUCT_SETTER(vendor);
PRODUCT_SETTER(bustype);
PRODUCT_SETTER(version);

LIBEVDEV_EXPORT int
libevdev_get_driver_version(const struct libevdev *dev)
{
	return dev->driver_version;
}

LIBEVDEV_EXPORT int
libevdev_has_property(const struct libevdev *dev, unsigned int prop)
{
	return (prop <= INPUT_PROP_MAX) && bit_is_set(dev->props, prop);
}

LIBEVDEV_EXPORT int
libevdev_enable_property(struct libevdev *dev, unsigned int prop)
{
	if (prop > INPUT_PROP_MAX)
		return -1;

	set_bit(dev->props, prop);
	return 0;
}

LIBEVDEV_EXPORT int
libevdev_has_event_type(const struct libevdev *dev, unsigned int type)
{
	return (type <= EV_MAX) && bit_is_set(dev->bits, type);
}

LIBEVDEV_EXPORT int
libevdev_has_event_code(const struct libevdev *dev, unsigned int type, unsigned int code)
{
	const unsigned long *mask;
	int max;

	if (!libevdev_has_event_type(dev, type))
		return 0;

	if (type == EV_SYN)
		return 1;

	max = type_to_mask_const(dev, type, &mask);

	if (max == -1 || code > (unsigned int)max)
		return 0;

	return bit_is_set(mask, code);
}

LIBEVDEV_EXPORT int
libevdev_get_event_value(const struct libevdev *dev, unsigned int type, unsigned int code)
{
	int value;

	if (!libevdev_has_event_type(dev, type) || !libevdev_has_event_code(dev, type, code))
		return 0;

	switch (type) {
		case EV_ABS: value = dev->abs_info[code].value; break;
		case EV_KEY: value = dev->interface->get(dev->external_key_values,
					dev->key_values_bit_id,
					dev->key_values_id,
					code); break;
		case EV_LED: value = bit_is_set(dev->led_values, code); break;
		case EV_SW: value = bit_is_set(dev->sw_values, code); break;
		default:
			value = 0;
			break;
	}

	return value;
}

LIBEVDEV_EXPORT int
libevdev_set_event_value(struct libevdev *dev, unsigned int type, unsigned int code, int value)
{
	int rc = 0;
	struct input_event e;

	if (!libevdev_has_event_type(dev, type) || !libevdev_has_event_code(dev, type, code))
		return -1;

	e.type = type;
	e.code = code;
	e.value = value;

	switch(type) {
		case EV_ABS: rc = update_abs_state(dev, &e); break;
		case EV_KEY: rc = update_key_state(dev, &e) > 0 ? -1 : 0; break;
		case EV_LED: rc = update_led_state(dev, &e); break;
		case EV_SW: rc = update_sw_state(dev, &e); break;
		default:
			     rc = -1;
			     break;
	}

	return rc;
}

LIBEVDEV_EXPORT int
libevdev_fetch_event_value(const struct libevdev *dev, unsigned int type, unsigned int code, int *value)
{
	if (libevdev_has_event_type(dev, type) &&
	    libevdev_has_event_code(dev, type, code)) {
		*value = libevdev_get_event_value(dev, type, code);
		return 1;
	} else
		return 0;
}

LIBEVDEV_EXPORT int
libevdev_get_slot_value(const struct libevdev *dev, unsigned int slot, unsigned int code)
{
	if (!libevdev_has_event_type(dev, EV_ABS) || !libevdev_has_event_code(dev, EV_ABS, code))
		return 0;

	if (dev->num_slots < 0 || slot >= (unsigned int)dev->num_slots || slot >= MAX_SLOTS)
		return 0;

	if (code > ABS_MT_MAX || code < ABS_MT_MIN)
		return 0;

	return dev->mt_slot_vals[slot][code - ABS_MT_MIN];
}

LIBEVDEV_EXPORT int
libevdev_set_slot_value(struct libevdev *dev, unsigned int slot, unsigned int code, int value)
{
	if (!libevdev_has_event_type(dev, EV_ABS) || !libevdev_has_event_code(dev, EV_ABS, code))
		return -1;

	if (dev->num_slots == -1 || slot >= (unsigned int)dev->num_slots || slot >= MAX_SLOTS)
		return -1;

	if (code > ABS_MT_MAX || code < ABS_MT_MIN)
		return -1;

	if (code == ABS_MT_SLOT) {
		if (value < 0 || value >= libevdev_get_num_slots(dev))
			return -1;
		dev->current_slot = value;
	}

	dev->mt_slot_vals[slot][code - ABS_MT_MIN] = value;


	return 0;
}

LIBEVDEV_EXPORT int
libevdev_fetch_slot_value(const struct libevdev *dev, unsigned int slot, unsigned int code, int *value)
{
	if (libevdev_has_event_type(dev, EV_ABS) &&
	    libevdev_has_event_code(dev, EV_ABS, code) &&
	    dev->num_slots >= 0 &&
	    slot < (unsigned int)dev->num_slots && slot < MAX_SLOTS) {
		*value = libevdev_get_slot_value(dev, slot, code);
		return 1;
	} else
		return 0;
}

LIBEVDEV_EXPORT int
libevdev_get_num_slots(const struct libevdev *dev)
{
	return dev->num_slots;
}

LIBEVDEV_EXPORT int
libevdev_get_current_slot(const struct libevdev *dev)
{
	return dev->current_slot;
}

LIBEVDEV_EXPORT const struct input_absinfo*
libevdev_get_abs_info(const struct libevdev *dev, unsigned int code)
{
	if (!libevdev_has_event_type(dev, EV_ABS) ||
	    !libevdev_has_event_code(dev, EV_ABS, code))
		return NULL;

	return &dev->abs_info[code];
}

#define ABS_GETTER(name) \
LIBEVDEV_EXPORT int libevdev_get_abs_##name(const struct libevdev *dev, unsigned int code) \
{ \
	const struct input_absinfo *absinfo = libevdev_get_abs_info(dev, code); \
	return absinfo ? absinfo->name : 0; \
}

ABS_GETTER(maximum);
ABS_GETTER(minimum);
ABS_GETTER(fuzz);
ABS_GETTER(flat);
ABS_GETTER(resolution);

#define ABS_SETTER(field) \
LIBEVDEV_EXPORT void libevdev_set_abs_##field(struct libevdev *dev, unsigned int code, int val) \
{ \
	if (!libevdev_has_event_code(dev, EV_ABS, code)) \
		return; \
	dev->abs_info[code].field = val; \
}

ABS_SETTER(maximum)
ABS_SETTER(minimum)
ABS_SETTER(fuzz)
ABS_SETTER(flat)
ABS_SETTER(resolution)

LIBEVDEV_EXPORT void
libevdev_set_abs_info(struct libevdev *dev, unsigned int code, const struct input_absinfo *abs)
{
	if (!libevdev_has_event_code(dev, EV_ABS, code))
		return;

	dev->abs_info[code] = *abs;
}

LIBEVDEV_EXPORT int
libevdev_enable_event_type(struct libevdev *dev, unsigned int type)
{
	if (type > EV_MAX)
		return -1;

	if (libevdev_has_event_type(dev, type))
		return 0;

	set_bit(dev->bits, type);

	if (type == EV_REP) {
		int delay = 0, period = 0;
		libevdev_enable_event_code(dev, EV_REP, REP_DELAY, &delay);
		libevdev_enable_event_code(dev, EV_REP, REP_PERIOD, &period);
	}
	return 0;
}

LIBEVDEV_EXPORT int
libevdev_disable_event_type(struct libevdev *dev, unsigned int type)
{
	if (type > EV_MAX || type == EV_SYN)
		return -1;

	clear_bit(dev->bits, type);

	return 0;
}

LIBEVDEV_EXPORT int
libevdev_enable_event_code(struct libevdev *dev, unsigned int type,
			   unsigned int code, const void *data)
{
	unsigned int max;
	unsigned long *mask = NULL;

	if (libevdev_enable_event_type(dev, type))
		return -1;

	switch(type) {
		case EV_SYN:
			return 0;
		case EV_ABS:
		case EV_REP:
			if (data == NULL)
				return -1;
			break;
		default:
			if (data != NULL)
				return -1;
			break;
	}

	max = type_to_mask(dev, type, &mask);

	if (code > max)
		return -1;

	set_bit(mask, code);

	if (type == EV_ABS) {
		const struct input_absinfo *abs = data;
		dev->abs_info[code] = *abs;
	} else if (type == EV_REP) {
		const int *value = data;
		dev->rep_values[code] = *value;
	}

	return 0;
}

LIBEVDEV_EXPORT int
libevdev_disable_event_code(struct libevdev *dev, unsigned int type, unsigned int code)
{
	unsigned int max;
	unsigned long *mask = NULL;

	if (type > EV_MAX)
		return -1;

	max = type_to_mask(dev, type, &mask);

	if (code > max)
		return -1;

	clear_bit(mask, code);

	return 0;
}

LIBEVDEV_EXPORT int
libevdev_kernel_set_abs_value(struct libevdev *dev, unsigned int code, const struct input_absinfo *abs)
{
	return libevdev_kernel_set_abs_info(dev, code, abs);
}

LIBEVDEV_EXPORT int
libevdev_kernel_set_abs_info(struct libevdev *dev, unsigned int code, const struct input_absinfo *abs)
{
	int rc;

	if (dev->fd < 0) {
		log_bug("device not initialized. call libevdev_set_fd() first\n");
		return -EBADF;
	}

	if (code > ABS_MAX)
		return -EINVAL;

	rc = ioctl(dev->fd, EVIOCSABS(code), abs);
	if (rc < 0)
		rc = -errno;
	else
		rc = libevdev_enable_event_code(dev, EV_ABS, code, abs);

	return rc;
}

LIBEVDEV_EXPORT int
libevdev_grab(struct libevdev *dev, enum libevdev_grab_mode grab)
{
	int rc = 0;

	if (dev->fd < 0) {
		log_bug("device not initialized. call libevdev_set_fd() first\n");
		return -EBADF;
	}

	if (grab != LIBEVDEV_GRAB && grab != LIBEVDEV_UNGRAB) {
		log_bug("invalid grab parameter %#x\n", grab);
		return -EINVAL;
	}

	if (grab == dev->grabbed)
		return 0;

	if (grab == LIBEVDEV_GRAB)
		rc = ioctl(dev->fd, EVIOCGRAB, (void *)1);
	else if (grab == LIBEVDEV_UNGRAB)
		rc = ioctl(dev->fd, EVIOCGRAB, (void *)0);

	if (rc == 0)
		dev->grabbed = grab;

	return rc < 0 ? -errno : 0;
}

/* DEPRECATED */
LIBEVDEV_EXPORT int
libevdev_is_event_type(const struct input_event *ev, unsigned int type)
ALIAS(libevdev_event_is_type);

LIBEVDEV_EXPORT int
libevdev_event_is_type(const struct input_event *ev, unsigned int type)
{
	return type < EV_CNT && ev->type == type;
}

/* DEPRECATED */
LIBEVDEV_EXPORT int
libevdev_is_event_code(const struct input_event *ev, unsigned int type, unsigned int code)
ALIAS(libevdev_event_is_code);

LIBEVDEV_EXPORT int
libevdev_event_is_code(const struct input_event *ev, unsigned int type, unsigned int code)
{
	int max;

	if (!libevdev_event_is_type(ev, type))
		return 0;

	max = libevdev_event_type_get_max(type);
	return (max > -1 && code <= (unsigned int)max && ev->code == code);
}

/* DEPRECATED */
LIBEVDEV_EXPORT const char*
libevdev_get_event_type_name(unsigned int type)
ALIAS(libevdev_event_type_get_name);

LIBEVDEV_EXPORT const char*
libevdev_event_type_get_name(unsigned int type)
{
	if (type > EV_MAX)
		return NULL;

	return ev_map[type];
}

/* DEPRECATED */
LIBEVDEV_EXPORT const char*
libevdev_get_event_code_name(unsigned int type, unsigned int code)
ALIAS(libevdev_event_code_get_name);

LIBEVDEV_EXPORT const char*
libevdev_event_code_get_name(unsigned int type, unsigned int code)
{
	int max = libevdev_event_type_get_max(type);

	if (max == -1 || code > (unsigned int)max)
		return NULL;

	return event_type_map[type][code];
}

/* DEPRECATED */
LIBEVDEV_EXPORT const char*
libevdev_get_input_prop_name(unsigned int prop)
ALIAS(libevdev_property_get_name);

/* DEPRECATED */
LIBEVDEV_EXPORT const char*
libevdev_get_property_name(unsigned int prop)
ALIAS(libevdev_property_get_name);

LIBEVDEV_EXPORT const char*
libevdev_property_get_name(unsigned int prop)
{
	if (prop > INPUT_PROP_MAX)
		return NULL;

	return input_prop_map[prop];
}

/* DEPRECATED */
LIBEVDEV_EXPORT int
libevdev_get_event_type_max(unsigned int type)
ALIAS(libevdev_event_type_get_max);

LIBEVDEV_EXPORT int
libevdev_event_type_get_max(unsigned int type)
{
	if (type > EV_MAX)
		return -1;

	return ev_max[type];
}

LIBEVDEV_EXPORT int
libevdev_get_repeat(struct libevdev *dev, int *delay, int *period)
{
	if (!libevdev_has_event_type(dev, EV_REP))
		return -1;

	if (delay != NULL)
		*delay = dev->rep_values[REP_DELAY];
	if (period != NULL)
		*period = dev->rep_values[REP_PERIOD];

	return 0;
}

LIBEVDEV_EXPORT int
libevdev_kernel_set_led_value(struct libevdev *dev, unsigned int code, enum libevdev_led_value value)
{
	return libevdev_kernel_set_led_values(dev, code, value, -1);
}

LIBEVDEV_EXPORT int
libevdev_kernel_set_led_values(struct libevdev *dev, ...)
{
	struct input_event ev[LED_MAX + 1];
	enum libevdev_led_value val;
	va_list args;
	int code;
	int rc = 0;
	size_t nleds = 0;

	if (dev->fd < 0) {
		log_bug("device not initialized. call libevdev_set_fd() first\n");
		return -EBADF;
	}

	memset(ev, 0, sizeof(ev));

	va_start(args, dev);
	code = va_arg(args, unsigned int);
	while (code != -1) {
		if (code > LED_MAX) {
			rc = -EINVAL;
			break;
		}
		val = va_arg(args, enum libevdev_led_value);
		if (val != LIBEVDEV_LED_ON && val != LIBEVDEV_LED_OFF) {
			rc = -EINVAL;
			break;
		}

		if (libevdev_has_event_code(dev, EV_LED, code)) {
			struct input_event *e = ev;

			while (e->type > 0 && e->code != code)
				e++;

			if (e->type == 0)
				nleds++;
			e->type = EV_LED;
			e->code = code;
			e->value = (val == LIBEVDEV_LED_ON);
		}
		code = va_arg(args, unsigned int);
	}
	va_end(args);

	if (rc == 0 && nleds > 0) {
		ev[nleds].type = EV_SYN;
		ev[nleds++].code = SYN_REPORT;

		rc = write(libevdev_get_fd(dev), ev, nleds * sizeof(ev[0]));
		if (rc > 0) {
			nleds--; /* last is EV_SYN */
			while (nleds--)
				update_led_state(dev, &ev[nleds]);
		}
		rc = (rc != -1) ? 0 : -errno;
	}

	return rc;
}
