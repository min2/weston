#ifndef _WESTON_INPUT_STATE_H_
#define _WESTON_INPUT_STATE_H_

#include <wayland-server.h>
#include "libevdev.h"

#define weston_array_for_each_reverse(pos, array)			\
	for (pos = (void *)((const char *)(array)->data + (array)->size); \
	     (const char *) (pos)-- > (const char *) (array)->data;	\
	     (pos)--)

struct weston_keyboard_keys_state {
	struct wl_array keys;
	struct wl_array keys_where;
	unsigned long used_bit_ids;
	unsigned long old_bit_ids;

};

/*
struct weston_buttons_state {
	struct wl_array b_where;
	unsigned long used_bit_ids;
	uint32_t key, mask;
};
*/

void
dump_keyz(struct weston_keyboard_keys_state *keyboard);


void state_keyboard_keys_init(struct weston_keyboard_keys_state *keyboard);
void state_keyboard_keys_release(struct weston_keyboard_keys_state *keyboard);

WL_EXPORT int state_keyboard_keys_get_update(void *external, unsigned long id_bit, unsigned int id, uint32_t key, int val);

WL_EXPORT int state_keyboard_keys_get(void *external, unsigned long id_bit, unsigned int id, uint32_t key);

WL_EXPORT void state_keyboard_keys_sync(void *external, unsigned long bit_id, unsigned int id, unsigned long *buf, unsigned long *buf_end, void *ptr, void (*callback)(void *ptr, int key, int val));

WL_EXPORT void state_keyboard_keys_deactivate(void *external, unsigned long int bit_id, unsigned int id);
WL_EXPORT int state_keyboard_keys_activate(void *external, unsigned long int *bit_id, unsigned int *id);
/*
const struct libevdev_external_key_values_interface input_state_interface;
*/




#endif
