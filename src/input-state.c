#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <linux/input.h>

#include "input-state.h"
#include "libevdev.h"

#define likely(x) __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)

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

void state_keyboard_keys_init(struct weston_keyboard_keys_state *keyboard)
{
/*
	weston_log("Initiating %p \n", keyboard);
*/
	wl_array_init(&keyboard->keys);
	wl_array_init(&keyboard->keys_where);
	keyboard->used_bit_ids = 0;
/*
	weston_log("bits %lu \n", keyboard->used_bit_ids);

*/


}

int state_keyboard_keys_activate(void *external, unsigned long int *bit_id, unsigned int *id)
{
	struct weston_keyboard_keys_state *keyboard = external;
	unsigned long flag = 1;

/*
	weston_log("Activating %p %lu \n", keyboard, keyboard->used_bit_ids);
	dump_keyz(keyboard);
*/

	do {
		if (0 == (flag & keyboard->used_bit_ids))
			break;

	} while (flag<<=1);

	if (unlikely(flag == 0))
		return -1;

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

	dump_keyz(keyboard);

	weston_log("puuuush %u\n", key);

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

	weston_log("Doing POP %p \n", keyboard);
	dump_keyz(keyboard);

	*k = *(end-1);
	*k_where = *(e-1);
	keyboard->keys.size -= sizeof *k;
	keyboard->keys_where.size -= sizeof *k_where;
}

void state_keyboard_keys_deactivate(void *external, unsigned long bit_id, unsigned int id)
{
	struct weston_keyboard_keys_state *keyboard = external;
	uint32_t *end = keyboard->keys.data + keyboard->keys.size;
	uint32_t *k = keyboard->keys.data;
	unsigned long *k_where;
/*
	weston_log("DeActivating %p %lu \n", keyboard, bit_id);
*/
	keyboard->used_bit_ids &= ~bit_id;

	wl_array_for_each(k_where, &keyboard->keys_where) {
		if (unlikely(*k_where)) {
			while ((bit_id == *k_where) && (k < end)) {
				state_keyboard_keys_pop(keyboard, k, k_where);
				end--;
			}

			*k_where &= ~bit_id;
		}
		k++;
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
	const int err = -1;

	uint32_t *k;
	unsigned long *k_where = keyboard->keys_where.data;

	wl_array_for_each(k, &keyboard->keys) {
		if (*k == key)
			goto found;
		k_where++;
	}

	if (likely(state == WL_KEYBOARD_KEY_STATE_PRESSED)) {
		state_keyboard_keys_push(keyboard, bit_id, key);
		return ok;
	} else {
		return err;
	}
found:
	if (likely(state == WL_KEYBOARD_KEY_STATE_RELEASED)) {
		if (likely(bit_id == *k_where)) {
			state_keyboard_keys_pop(keyboard, k, k_where);
			return ok;
		} else {
			*k_where = ~bit_id & *k_where;
			return err;
		}
	} else {
		*k_where |= bit_id;
		return err;
	}
}

int state_keyboard_keys_get_set(void *external, unsigned long bit_id, unsigned int id,
				uint32_t key)
{
	struct weston_keyboard_keys_state *keyboard = external;
	return state_keyboard_keys_internal(keyboard, bit_id, key,
						WL_KEYBOARD_KEY_STATE_PRESSED);
}

int
state_keyboard_keys_get_reset(void *external, unsigned long bit_id,
				unsigned int id, uint32_t key)
{
	struct weston_keyboard_keys_state *keyboard = external;
	return state_keyboard_keys_internal(keyboard, bit_id, key,
						WL_KEYBOARD_KEY_STATE_RELEASED);
}


int
state_keyboard_keys_get_update(void *external, unsigned long bit_id, unsigned int id,
				uint32_t key, int val)
{
	int r;
	struct weston_keyboard_keys_state *keyboard = external;
	r = state_keyboard_keys_internal(keyboard, bit_id, key, val);
	if (r)
		weston_log("KEYERR %u %u\n", key, val);
	return r;
}

void
state_keyboard_keys_set(void *external, unsigned long bit_id, unsigned int id,
			uint32_t key)
{
	struct weston_keyboard_keys_state *keyboard = external;
	state_keyboard_keys_internal(keyboard, bit_id, key,
					WL_KEYBOARD_KEY_STATE_PRESSED);
}

void state_keyboard_keys_reset(void *external, unsigned long bit_id, unsigned int id,
				uint32_t key)
{
	struct weston_keyboard_keys_state *keyboard = external;
	state_keyboard_keys_internal(keyboard, bit_id, key,
					WL_KEYBOARD_KEY_STATE_RELEASED);
}


void state_keyboard_keys_update(void *external, unsigned long bit_id, unsigned int id,
				uint32_t key, int val)
{
	struct weston_keyboard_keys_state *keyboard = external;
	state_keyboard_keys_internal(keyboard, bit_id, key, val);
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

/*
	 = buf + NLONGS(KEY_CNT);
*/



void state_keyboard_keys_sync(void *external, unsigned long bit_id, unsigned int id, unsigned long *buf, unsigned long *buf_end, void *ptr, void (*callback)(void *ptr, int key, int val))
{
	struct weston_keyboard_keys_state *keyboard = external;
	uint32_t *end = keyboard->keys.data + keyboard->keys.size;
	uint32_t *k;
	unsigned long *k_where = keyboard->keys_where.data;
	unsigned int i, j;
	unsigned long *bufi;
	uint32_t key;
/*
	weston_log("Syncing %p\n", keyboard);
*/
	/* first we update the held keys */

	wl_array_for_each(k, &keyboard->keys) {
/*
		weston_log("!! %u \n", *k);
*/
		while ((k < end) && (bit_id == *k_where) && (!bit_is_set(buf, *k))) {

			key = *k;
			state_keyboard_keys_pop(keyboard, k, k_where);

			callback(ptr, key, 0);

			end = keyboard->keys.data + keyboard->keys.size;
		}

		if (k < end) {

			if (bit_is_set(buf, *k)) {
/*
				weston_log("!!killing %u \n", *k);
*/
				*k_where |= bit_id;
				clear_bit(buf, *k);
			} else {
				*k_where &= ~bit_id;
			}
		}

		k_where++;
	}
	/* now we add the newly pressed keys */

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

/*
	dump_keyz(keyboard);
	weston_log("Synced \n");
*/
}
/*
void state_buttons_init(struct weston_buttons_state *buttons)
{
	wl_array_init(&buttons->b_where);
	buttons->used_bit_ids = 0;
	buttons->mask = 0;
	buttons->key = 0;
}

void state_buttons_release(struct weston_buttons_state *buttons)
{
	assert(buttons->used_bit_ids == 0);
}

static int inline
state_buttons_internal(struct weston_buttons_state *buttons,
			unsigned long bit_id,
			uint32_t key, int state)
{
	const int err_multi_release = -1;
	const int err_multi_press = 0;
	const int ok_release = 0;
	const int ok_press = -1;
	unsigned long *bit;
	unsigned long tmp;
	int offset;

	if (unlikely(buttons->mask = 0)) {
		bit = wl_array_add(&buttons->b_where, 2 * sizeof *bit);
		bit[0] = 0;
		bit[1] = 0;
		buttons->mask = 1;
		buttons->key = key | buttons->mask;
	}

	while (unlikely((key | buttons->mask) != buttons->key)) {
		unsigned int btns_cnt = (buttons->mask ^ (buttons->mask >> 1));
		bit = wl_array_add(&buttons->b_where,
			btns_cnt * sizeof *bit);
		memset(bit, 0, btns_cnt * sizeof *bit);
		buttons->mask <<= 1;
		buttons->mask |= 1;
		buttons->key = key | buttons->mask;
	}

	offset = key & buttons->mask;

	bit = &buttons->buttons_where.data[offset];

	if (state == 1) {
		tmp = *bit;
		*bit |= bit_id;
		if (unlikely(tmp))
			return err_multi_press;
		else
			return ok_press;
	} else {
		if (likely(*bit == bit_id)) {
			*bit = 0;
			return ok_release;
		} else {
			*bit &= ~bit_id;
			return err_multi_release;
		}
	}
}
*/
/*
TODO:
* ta struktura
  - v rpi je to inak -> dat strukturu do struktury
*/




