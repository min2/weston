#include <stdio.h>
#include <assert.h>
#include <stdint.h>
/*
#include <linux/input.h>
*/
#include <stdlib.h>

#include "input-state.h"
#include "libevdev.h"

#define likely(x) __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)

/*
void
dump_keyz(struct weston_keyboard_keys_state *keyboard)
{
	uint32_t *end = keyboard->keys.data + keyboard->keys.size;
	unsigned long *e = keyboard->keys_where.data + keyboard->keys_where.size;
	unsigned long *k_where = keyboard->keys_where.data;
	uint32_t *k;
	wl_array_for_each(k, &keyboard->keys) {
		weston_log("KEYHELD %u %lu \n", *k, *k_where);
		k_where++;
	}
	assert(end==k);
	assert(e==k_where);
}
*/

void state_keyboard_keys_init(struct weston_keyboard_keys_state *s)
{
/*
	weston_log("Initiating %p \n", s);
*/
	wl_array_init(&s->keys);
	wl_array_init(&s->keys_where);
	s->used_bit_ids = 0;

/*
	weston_log("bits %lu \n", s->used_bit_ids);
*/
}

int state_keyboard_keys_activate(void *external, unsigned long int *bit_id, unsigned int *id)
{
	struct weston_keyboard_keys_state *keyboard = external;
	unsigned long flag = 1;


	weston_log("Activating %p %lu \n", keyboard, keyboard->used_bit_ids);
/*
	dump_keyz(keyboard);
*/
	do {
		if (0 == (flag & keyboard->used_bit_ids))
			break;

	} while (flag<<=1);

	if (unlikely(flag == 0))
		return -1; /* only 32 devices per seat :( */

	keyboard->used_bit_ids |= flag;
	*bit_id = flag;
	*id = 0;
/*
	weston_log("Assigned %lu \n", flag);
*/
	return 0;
}

inline static void
state_keyboard_keys_push(struct weston_keyboard_keys_state *keyboard,
			unsigned long bit_id, uint32_t key)
{
	uint32_t *k;
	unsigned long *k_where;
/*
	dump_keyz(keyboard);

	weston_log("puuuush %u\n", key);
*/
	k = wl_array_add(&keyboard->keys, sizeof *k);
	k_where = wl_array_add(&keyboard->keys_where, sizeof *k_where);
	*k = key;
	*k_where = bit_id;
/*
	dump_keyz(keyboard);
*/
}

inline static void
state_keyboard_keys_pop(struct weston_keyboard_keys_state *keyboard,
				uint32_t *k, unsigned long *k_where)
{
	uint32_t *end = keyboard->keys.data + keyboard->keys.size;
	unsigned long *e = keyboard->keys_where.data + keyboard->keys_where.size;
/*
	weston_log("Doing POP %p \n", keyboard);
	dump_keyz(keyboard);
*/
	*k = *(end-1);
	*k_where = *(e-1);
	keyboard->keys.size -= sizeof *k;
	keyboard->keys_where.size -= sizeof *k_where;
}

void state_keyboard_keys_deactivate(void *external, unsigned long bit_id, unsigned int id)
{
	struct weston_keyboard_keys_state *keyboard = external;
	uint32_t *k = keyboard->keys.data + keyboard->keys.size - 1;
	unsigned long *k_where;

/*
	weston_log("DeActivating %p %lu \n", keyboard, bit_id);
*/

	keyboard->used_bit_ids &= ~bit_id;

	weston_array_for_each_reverse(k_where, &keyboard->keys_where) {

		if (unlikely(bit_id == *k_where))
			state_keyboard_keys_pop(keyboard, k, k_where);
		else
			*k_where &= ~bit_id;

		k--;
	}
/*
	dump_keyz(keyboard);
	weston_log("DeActivated %p %lu \n", keyboard, bit_id);
*/
}

void state_keyboard_keys_release(struct weston_keyboard_keys_state *keyboard)
{
	wl_array_release(&keyboard->keys);
	wl_array_release(&keyboard->keys_where);
	assert(keyboard->used_bit_ids == 0);
}

int
state_keyboard_keys_get(void *external, unsigned long bit_id,
			unsigned int id, uint32_t key)
{
	struct weston_keyboard_keys_state *keyboard = external;
	uint32_t *k;
	const int released = 0;
	const int pressed = -1;
	wl_array_for_each(k, &keyboard->keys) {
		if (*k == key)
			return pressed;
	}
	return released;
}

inline static int
state_keyboard_keys_internal(struct weston_keyboard_keys_state *keyboard,
				unsigned long bit_id,
				uint32_t key, enum wl_keyboard_key_state state)
{
	const int ok = 0;
	const int err_filter = -1;
	const int err_nofilter = -1;

	uint32_t *k;
	unsigned long *k_where = keyboard->keys_where.data;

	/*
		if this adds latency, that means many keys are held
		in that case, we can:
			- sort it in spare time and use binary searches
			- use min max any key mask to skip some useless searches
	*/

	wl_array_for_each(k, &keyboard->keys) {
		if (*k == key)
			goto found;
		k_where++;
	}

	if (likely(state == WL_KEYBOARD_KEY_STATE_PRESSED)) {
		state_keyboard_keys_push(keyboard, bit_id, key);
		return ok;
	} else {
		return err_filter;
	}
found:
	if (likely(state == WL_KEYBOARD_KEY_STATE_RELEASED)) {
		if (likely(bit_id == *k_where)) {
			state_keyboard_keys_pop(keyboard, k, k_where);
			return ok;
		} else {
			*k_where = ~bit_id & *k_where;
			return err_filter;
		}
	} else {
		*k_where |= bit_id;
		return err_filter;
	}
}

int
state_keyboard_keys_get_update(void *external, unsigned long bit_id, unsigned int id,
				uint32_t key, int val)
{
	int r;
	struct weston_keyboard_keys_state *keyboard = external;
	r = state_keyboard_keys_internal(keyboard, bit_id, key, val);
/*
	if (r)
		weston_log("KEYERR %u %u\n", key, val);
*/
	return r;
}

#ifndef LONG_BITS
#define LONG_BITS (sizeof(long) * 8)
#endif

static inline void
clear_bit(unsigned long *array, int bit)
{
    array[bit / LONG_BITS] &= ~(1LL << (bit % LONG_BITS));
}

static inline int
bit_is_set(const unsigned long *array, int bit)
{
    return !!(array[bit / LONG_BITS] & (1LL << (bit % LONG_BITS)));
}

void state_keyboard_keys_sync(void *external, unsigned long bit_id, unsigned int id, unsigned long *buf, unsigned long *buf_end, void *ptr, void (*callback)(void *ptr, int key, int val))
{
	struct weston_keyboard_keys_state *keyboard = external;
	uint32_t *k;
	unsigned long *k_where = keyboard->keys_where.data + keyboard->keys.size - 1;
	unsigned int i, j;
	unsigned long *bufi;
	uint32_t key;

	/* first we check how changed the status of our keys */
	weston_array_for_each_reverse(k, &keyboard->keys) {

		if (bit_is_set(buf, *k)) {

			*k_where |= bit_id;
			clear_bit(buf, *k);
		} else {
			/* key not held any more, need to fix */
			if (bit_id == *k_where) {
				key = *k;
				state_keyboard_keys_pop(keyboard, k, k_where);

				callback(ptr, key, 0);
			} else {
				*k_where &= ~bit_id;
			}

		}

		k_where--;
	}
	/* now we push the newly pressed keys to the end */

	for (i = 0 , bufi = buf; bufi < buf_end; bufi++) {
		if (*bufi) {
			for (j = 0; j < LONG_BITS; j++) {
				if (bit_is_set(bufi, j)) {
					key = i + j;
					callback(ptr, key, 1);
/*
					weston_log("KeyDownIng %u \n", key);
*/
					state_keyboard_keys_push(keyboard,
								bit_id, key);
				}
			}
		}
	}

	free(buf);

}

