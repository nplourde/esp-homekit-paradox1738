#ifndef _PARADOX1738_H_
#define _PARADOX1738_H_

#include <stdbool.h>
#include <stddef.h>

#include "paradox_proto.h"

/* Decoded event as delivered to the HomeKit layer. */
typedef paradox_event_t paradox1738_event_t;


/**
 * @brief initialize the paradox 1738 lowlevel module
 *
 * @param none
 *
 * @return none
 */
void paradox1738_init(void);

/**
 * @brief deinitialize the paradox1738 lowlevel module
 *
 * @param none
 *
 * @return none
 */
void paradox1738_deinit(void);

/**
 * @brief read event data from Paradox 1738 serial port
 *
 * @param none
 *
 * @return none
 */
bool paradox1738_event_read(paradox1738_event_t *out_event);

/**
 * @brief copy the decoder counters (framing errors, discarded bytes, overruns)
 *
 * @param out_stats destination; nothing is written when NULL
 *
 * @return none
 */
void paradox1738_stats(paradox_stats_t *out_stats);

/**
 * @brief zero the decoder counters
 *
 * @param none
 *
 * @return none
 */
void paradox1738_stats_clear(void);

/**
 * @brief bring the link up and tear it down twice (T1.6)
 *
 * paradox1738_deinit() used to be declared but never implemented, which only
 * showed up at link time. This runs init/deinit/deinit so the second call is
 * proven harmless, and logs the result. Enabled with CONFIG_PARADOX_LINK_SELFTEST.
 *
 * @param none
 *
 * @return true when the link came up and both deinit calls were clean
 */
bool paradox1738_link_selftest(void);

#ifdef CONFIG_PARADOX_SIMULATOR
/* Recorded-panel replay support (see paradox_sim.c). */
const uint8_t (*paradox_sim_recorded_frames(void))[4];
size_t paradox_sim_recorded_frame_count(void);
size_t paradox_sim_replay_interval_ms(void);
#endif

#endif /* _PARADOX1738_H_ */
