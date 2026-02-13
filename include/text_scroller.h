#ifndef TEXT_SCROLLER_H
#define TEXT_SCROLLER_H

#include <stdint.h>
#include <stdbool.h>

#define SCROLLER_MAX_TEXT_LEN 200

void scroller_init(void);
void scroller_set_text(const char *text);
void scroller_set_color(uint8_t r, uint8_t g, uint8_t b);
void scroller_set_speed(uint8_t speed); // 1 (slowest) to 10 (fastest)
int  scroller_tick(bool *cycle_complete);  // render one frame, return delay_ms

#endif
