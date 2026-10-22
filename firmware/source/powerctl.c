// SPDX-License-Identifier: Apache-2.0
/* Live ESP8089 power ceilings derived from its calibrated per-channel tables.
 * Keep an immutable copy of the factory profile. Retarget both the digital
 * gain tables and the targets used by periodic power calibration.
 */
typedef unsigned int u32;
typedef unsigned char u8;
typedef signed char s8;
#include "fw-critical.h"
#define POWER_MAGIC 0x31525750U /* PWR1 */
#define CAL ((volatile u8 *)0x3ffeef18)
#define INIT_TARGET ((volatile u8 *)0x3ffef0c0)

__attribute__((section(".power_state")))
volatile u32 power_state[10] = { 0 };
extern u32 *alloc_event(u32 length, u32 id);
extern void send_event(u32 *first, u32 *last);
extern void tx_atten_set_interp(u32 channel, u8 *six);

static inline u32 factory_byte(u32 index)
{
	return ((volatile u8 *)&power_state[2])[index];
}

__attribute__((noinline))
int power_prepare(u32 cap, u8 *next)
{
	u32 i, slot = 0, target, delta;
	int value, correction = *(volatile s8 *)0x3ffeef40;

	for (i = 0; i < 30; i++) {
		target = factory_byte(24 + slot);
		delta = target > cap ? target - cap : 0;
		if (i < 24) {
			value = (s8)factory_byte(i) + (int)delta;
			/* Include the coarse correction and the CCK adjustment when
			 * checking the original routine's signed-byte arithmetic.
			 */
			if (value < -128 || value > 127 ||
			    value + correction < -128 || value + correction + 13 > 127)
				return -34;
			next[i] = value;
		} else {
			next[i] = target - delta;
		}
		if (++slot == 6)
			slot = 0;
	}
	return 0;
}

int power_apply(u32 cap)
{
	u8 next[30], six[6];
	u32 i;
	int status;

	if (!power_state[0]) {
		for (i = 0; i < 8; i++)
			power_state[2 + i] = ((volatile u32 *)CAL)[i];
		power_state[1] = factory_byte(24);
		power_state[0] = 1;
	}
	if (cap == ~0U)
		return 0;
	if (cap > 82)
		return -22;
	if (cap > factory_byte(24))
		cap = factory_byte(24);
	status = power_prepare(cap, next);
	if (status)
		return status;
	for (i = 0; i < 30; i++)
		CAL[i] = next[i];
	for (i = 0; i < 6; i++)
		INIT_TARGET[i] = next[24 + i];
	tx_atten_set_interp(*(volatile u8 *)0x3ffeef16, six);
	power_state[1] = cap;
	return 0;
}

void power_control(u32 *request)
{
	u32 *event, *out;

	if (request[0] != POWER_MAGIC)
		return;
	event = alloc_event(24, 24);
	if (!event)
		return;
	out = (u32 *)event[1] + 3;
	out[0] = POWER_MAGIC;
	out[1] = request[1];
	u32 ps = fw_irq_save();
	out[2] = power_apply(request[2]);
	fw_irq_restore(ps);
	out[3] = power_state[1];
	out[4] = factory_byte(24);
	out[5] = 0;
	send_event(event, event);
}
