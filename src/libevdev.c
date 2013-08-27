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
#include <linux/uinput.h>

#include "libevdev.h"
#include "libevdev-int.h"
#include "libevdev-util.h"
#include "event-names.h"

#define MAXEVENTS 64

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
_libevdev_log(struct libevdev *dev, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	dev->log(format, args);
	va_end(args);
}

static void
libevdev_noop_log_func(const char *format, va_list args)
{
}

struct libevdev*
libevdev_new(void)
{
	struct libevdev *dev;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;
	dev->fd = -1;
	dev->num_slots = -1;
	dev->current_slot = -1;
	dev->log = libevdev_noop_log_func;
	dev->grabbed = LIBEVDEV_UNGRAB;
	dev->sync_state = SYNC_NONE;

	dev->external_key_values = NULL;

	return dev;
}

int
libevdev_new_from_fd(int fd, struct libevdev **dev)
{
	struct libevdev *d;
	int rc;

	d = libevdev_new();
	if (!d)
		return -ENOSPC;

	rc = libevdev_set_fd(d, fd);
	if (rc < 0)
		libevdev_free(d);
	else
		*dev = d;
	return rc;
}

int libevdev_external_key_values_activate(
	struct libevdev *dev,
	const struct libevdev_external_key_values_interface *interface,
	void *external_structure
)
{
	if (dev->external_key_values != NULL)
		return -2;
/*
	weston_log("Gonna use %p\n", external_structure);
*/
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
}

void
libevdev_free(struct libevdev *dev)
{
	if (!dev)
		return;

	free(dev->name);
	free(dev->phys);
	free(dev->uniq);
	queue_free(dev);
	free(dev);
}

void
libevdev_set_log_handler(struct libevdev *dev, libevdev_log_func_t logfunc)
{
	if (dev == NULL)
		return;

	dev->log = logfunc ? logfunc : libevdev_noop_log_func;
}

int
libevdev_change_fd(struct libevdev *dev, int fd)
{
	if (dev->fd == -1)
		return -1;
	dev->fd = fd;
	return 0;
}

int
libevdev_set_fd(struct libevdev* dev, int fd)
{
	int rc;
	int i;
	char buf[256];

	if (dev->fd != -1)
		return -EBADF;

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
		errno = ENOSPC;
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
			errno = ENOSPC;
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
			errno = ENOSPC;
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

	if (NULL == dev->external_key_values) {
		rc = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(dev->key_bits)), dev->key_bits);
		if (rc < 0)
			goto out;
	}

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
	return rc ? -errno : 0;
}

int
libevdev_get_fd(const struct libevdev* dev)
{
	return dev->fd;
}

static inline void
init_event(struct libevdev *dev, struct input_event *ev, int type, int code, int value)
{
	ev->time = dev->last_event_time;
	ev->type = type;
	ev->code = code;
	ev->value = value;
}

static void state_callback(void *ptr, int key, int val)
{
	if (ptr) {
		struct libevdev *dev = ptr;
		struct input_event *ev = queue_push(dev);
		init_event(dev, ev, EV_KEY, key, val ? 1 : 0);
	}
}

static int
sync_external_key_state(struct libevdev *dev, int emit_fake_events)
{
	int rc;
	int i;
	unsigned long keystate[NLONGS(KEY_MAX)];
	void *ptr = emit_fake_events ? (void *) dev : NULL;

	rc = ioctl(dev->fd, EVIOCGKEY(sizeof(keystate)), keystate);
	if (rc < 0)
		return -errno;
/*
	weston_log("Gonna sync %p\n", dev->external_key_values);
*/
	dev->interface->sync(dev->external_key_values,
			dev->key_values_bit_id, dev->key_values_id,
			&keystate[0], &keystate[NLONGS(KEY_MAX)], 
			ptr, &state_callback);

	return 0;
}

static int
sync_key_state(struct libevdev *dev)
{
	int rc;
	int i;
	unsigned long keystate[NLONGS(KEY_MAX)];

	rc = ioctl(dev->fd, EVIOCGKEY(sizeof(keystate)), keystate);
	if (rc < 0)
		goto out;

	for (i = 0; i < KEY_MAX; i++) {
		int old, new;
		old = bit_is_set(dev->key_values, i);
		new = bit_is_set(keystate, i);
		if (old ^ new) {
			state_callback(dev, i, new);
		}
		set_bit_state(dev->key_values, i, new);
	}

	rc = 0;
out:
	return rc ? -errno : 0;
}

int
libevdev_sync_key_state(struct libevdev *dev)
{
	if (dev->external_key_values)
		sync_external_key_state(dev, 0);
	else
		sync_key_state(dev);
}

static int
sync_abs_state(struct libevdev *dev)
{
	int rc;
	int i;

	for (i = ABS_X; i <= ABS_MAX; i++) {
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
		int val[LIBE_MAX_SLOTS];
	} mt_state[ABS_MT_CNT];

	for (i = ABS_MT_MIN; i < ABS_MT_MAX; i++) {
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

		for (j = ABS_MT_MIN; j < ABS_MT_MAX; j++) {
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
		struct input_event e;
		queue_peek(dev, i, &e);
		if (e.type == EV_SYN)
			break;
	}

	if (i > 0)
		queue_shift_multiple(dev, i + 1, NULL);

	if (libevdev_has_event_type(dev, EV_KEY))
		rc = sync_key_state(dev);
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

	if (dev->external_key_values)
		return dev->interface->get_update(dev->external_key_values,
			dev->key_values_bit_id, dev->key_values_id, e->code, e->value != 0);
	else
		set_bit_state(dev->key_values, e->code, e->value != 0);

	return 0;
}

static int
update_mt_state(struct libevdev *dev, const struct input_event *e)
{
	if (e->code == ABS_MT_SLOT) {
		dev->current_slot = e->value;
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
		return update_mt_state(dev, e);

	dev->abs_info[e->code].value = e->value;

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

int libevdev_next_event(struct libevdev *dev, unsigned int flags, struct input_event *ev)
{
	int rc = 0;

	if (dev->fd < 0)
		return -ENODEV;

	if (!(flags & (LIBEVDEV_READ_NORMAL|LIBEVDEV_READ_SYNC|LIBEVDEV_FORCE_SYNC)))
		return -EINVAL;

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

int libevdev_has_event_pending(struct libevdev *dev)
{
	struct pollfd fds = { dev->fd, POLLIN, 0 };
	int rc;

	if (dev->fd < 0)
		return -EBADF;

	if (queue_num_elements(dev) != 0)
		return 1;

	rc = poll(&fds, 1, 0);
	return (rc >= 0) ? rc : -errno;
}

const char *
libevdev_get_name(const struct libevdev *dev)
{
	return dev->name ? dev->name : "";
}

const char *
libevdev_get_phys(const struct libevdev *dev)
{
	return dev->phys;
}

const char *
libevdev_get_uniq(const struct libevdev *dev)
{
	return dev->uniq;
}

#define STRING_SETTER(field) \
void libevdev_set_##field(struct libevdev *dev, const char *field) \
{ \
	if (field == NULL) \
		return; \
	free(dev->field); \
	dev->field = strdup(field); \
}

STRING_SETTER(name);
STRING_SETTER(phys);
STRING_SETTER(uniq);


#define PRODUCT_GETTER(name, field) \
int libevdev_get_##name(const struct libevdev *dev) \
{ \
	return dev->ids.field; \
}

PRODUCT_GETTER(product_id, product); /* DEPRECATED */
PRODUCT_GETTER(vendor_id, vendor); /* DEPRECATED */
PRODUCT_GETTER(bustype, bustype); /* DEPRECATED */
PRODUCT_GETTER(version, version); /* DEPRECATED */

PRODUCT_GETTER(id_product, product);
PRODUCT_GETTER(id_vendor, vendor);
PRODUCT_GETTER(id_bustype, bustype);
PRODUCT_GETTER(id_version, version);

#define PRODUCT_SETTER(field) \
void libevdev_set_id_##field(struct libevdev *dev, int field) \
{ \
	dev->ids.field = field;\
}

PRODUCT_SETTER(product);
PRODUCT_SETTER(vendor);
PRODUCT_SETTER(bustype);
PRODUCT_SETTER(version);

int libevdev_get_driver_version(const struct libevdev *dev)
{
	return dev->driver_version;
}

int
libevdev_has_property(const struct libevdev *dev, unsigned int prop)
{
	return (prop <= INPUT_PROP_MAX) && bit_is_set(dev->props, prop);
}

int
libevdev_enable_property(struct libevdev *dev, unsigned int prop)
{
	if (prop > INPUT_PROP_MAX)
		return -1;

	set_bit(dev->props, prop);
	return 0;
}

int
libevdev_has_event_type(const struct libevdev *dev, unsigned int type)
{
	return (type <= EV_MAX) && bit_is_set(dev->bits, type);
}

int
libevdev_has_event_code(const struct libevdev *dev, unsigned int type, unsigned int code)
{
	const unsigned long *mask;
	int max;

	if (!libevdev_has_event_type(dev, type))
		return 0;

	if (type == EV_SYN)
		return 1;

	max = type_to_mask_const(dev, type, &mask);

	if (max == -1 || code > max)
		return 0;

	return bit_is_set(mask, code);
}

int
libevdev_get_event_value(const struct libevdev *dev, unsigned int type, unsigned int code)
{
	int value;

	if (!libevdev_has_event_type(dev, type) || !libevdev_has_event_code(dev, type, code))
		return 0;

	switch (type) {
		case EV_ABS: value = dev->abs_info[code].value; break;
		case EV_KEY: value = bit_is_set(dev->key_values, code); break;
		default:
			value = 0;
			break;
	}

	return value;
}

int
libevdev_fetch_event_value(const struct libevdev *dev, unsigned int type, unsigned int code, int *value)
{
	if (libevdev_has_event_type(dev, type) &&
	    libevdev_has_event_code(dev, type, code)) {
		*value = libevdev_get_event_value(dev, type, code);
		return 1;
	} else
		return 0;
}

int
libevdev_get_slot_value(const struct libevdev *dev, unsigned int slot, unsigned int code)
{
	if (!libevdev_has_event_type(dev, EV_ABS) || !libevdev_has_event_code(dev, EV_ABS, code))
		return 0;

	if (slot >= dev->num_slots || slot >= LIBE_MAX_SLOTS)
		return 0;

	if (code > ABS_MT_MAX || code < ABS_MT_MIN)
		return 0;

	return dev->mt_slot_vals[slot][code - ABS_MT_MIN];
}

int
libevdev_fetch_slot_value(const struct libevdev *dev, unsigned int slot, unsigned int code, int *value)
{
	if (libevdev_has_event_type(dev, EV_ABS) &&
	    libevdev_has_event_code(dev, EV_ABS, code) &&
	    slot < dev->num_slots && slot < LIBE_MAX_SLOTS) {
		*value = libevdev_get_slot_value(dev, slot, code);
		return 1;
	} else
		return 0;
}

int
libevdev_get_num_slots(const struct libevdev *dev)
{
	return dev->num_slots;
}

int
libevdev_get_current_slot(const struct libevdev *dev)
{
	return dev->current_slot;
}

const struct input_absinfo*
libevdev_get_abs_info(const struct libevdev *dev, unsigned int code)
{
	if (!libevdev_has_event_type(dev, EV_ABS) ||
	    !libevdev_has_event_code(dev, EV_ABS, code))
		return NULL;

	return &dev->abs_info[code];
}

#define ABS_GETTER(name, field) \
int libevdev_get_abs_##name(const struct libevdev *dev, unsigned int code) \
{ \
	const struct input_absinfo *absinfo = libevdev_get_abs_info(dev, code); \
	return absinfo ? absinfo->field : 0; \
}

ABS_GETTER(max, maximum); /* DEPRECATED */
ABS_GETTER(min, minimum); /* DEPRECATED */
ABS_GETTER(maximum, maximum);
ABS_GETTER(minimum, minimum);
ABS_GETTER(fuzz, fuzz)
ABS_GETTER(flat, flat)
ABS_GETTER(resolution, resolution)

#define ABS_SETTER(field) \
void libevdev_set_abs_##field(struct libevdev *dev, unsigned int code, int val) \
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

void libevdev_set_abs_info(struct libevdev *dev, unsigned int code, const struct input_absinfo *abs)
{
	if (!libevdev_has_event_code(dev, EV_ABS, code))
		return;

	dev->abs_info[code] = *abs;
}

int
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

int
libevdev_disable_event_type(struct libevdev *dev, unsigned int type)
{
	if (type > EV_MAX || type == EV_SYN)
		return -1;

	clear_bit(dev->bits, type);

	return 0;
}

int
libevdev_enable_event_code(struct libevdev *dev, unsigned int type,
			   unsigned int code, const void *data)
{
	unsigned int max;
	unsigned long *mask;

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

int
libevdev_disable_event_code(struct libevdev *dev, unsigned int type, unsigned int code)
{
	unsigned int max;
	unsigned long *mask;

	if (type > EV_MAX)
		return -1;

	max = type_to_mask(dev, type, &mask);

	if (code > max)
		return -1;

	clear_bit(mask, code);

	return 0;
}

int
libevdev_kernel_set_abs_value(struct libevdev *dev, unsigned int code, const struct input_absinfo *abs)
{
	int rc;

	if (code > ABS_MAX)
		return -EINVAL;

	rc = ioctl(dev->fd, EVIOCSABS(code), abs);
	if (rc < 0)
		rc = -errno;
	else
		rc = libevdev_enable_event_code(dev, EV_ABS, code, abs);

	return rc;
}

int
libevdev_grab(struct libevdev *dev, int grab)
{
	int rc = 0;

	if (grab != LIBEVDEV_GRAB && grab != LIBEVDEV_UNGRAB)
		return -EINVAL;

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

int
libevdev_is_event_type(const struct input_event *ev, unsigned int type)
{
	return type < EV_MAX && ev->type == type;
}

int
libevdev_is_event_code(const struct input_event *ev, unsigned int type, unsigned int code)
{
	return type < EV_MAX &&
		ev->type == type &&
		(type == EV_SYN || code <= libevdev_get_event_type_max(type)) &&
		ev->code == code;
}

const char*
libevdev_get_event_type_name(unsigned int type)
{
	if (type > EV_MAX)
		return NULL;

	return ev_map[type];
}

const char*
libevdev_get_event_code_name(unsigned int type, unsigned int code)
{
	if (type > EV_MAX)
		return NULL;

	if (code > ev_max[type])
		return NULL;

	return event_type_map[type][code];
}

/* DEPRECATED */
const char*
libevdev_get_input_prop_name(unsigned int prop)
{
	return libevdev_get_property_name(prop);
}

const char*
libevdev_get_property_name(unsigned int prop)
{
	if (prop > INPUT_PROP_MAX)
		return NULL;

	return input_prop_map[prop];
}

int
libevdev_get_event_type_max(unsigned int type)
{
	if (type > EV_MAX)
		return -1;

	return ev_max[type];
}

int
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
