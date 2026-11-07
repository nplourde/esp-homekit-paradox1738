#ifndef _PARADOX1738_H_
#define _PARADOX1738_H_

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

#endif /* _PARADOX1738_H_ */
