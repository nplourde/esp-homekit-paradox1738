#ifndef _PARADOX1738_H_
#define _PARADOX1738_H_

#include <stdbool.h>
#include <stddef.h>

typedef struct
{
    /* data */
    int EventId;
    int CategoryId;
} paradox1738_event_t;


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

#ifdef CONFIG_PARADOX_SIMULATOR
/* Recorded-panel replay support (see paradox_sim.c). */
const uint8_t (*paradox_sim_recorded_frames(void))[4];
size_t paradox_sim_recorded_frame_count(void);
size_t paradox_sim_replay_interval_ms(void);
#endif

#endif /* _PARADOX1738_H_ */
