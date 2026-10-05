#ifndef INPUT_H
#define INPUT_H

#include <io/pad.h>

typedef struct {
    int buttons_down;    // Buttons currently pressed this frame
    int buttons_pressed; // Buttons that just transitioned to pressed
    short ly, lx, ry, rx;
} ps3_pad_state_t;

void ps3input_start();
void ps3input_stop();
void ps3input_get_data(ps3_pad_state_t *state);

// Rumble from the host: controller number, low-frequency (large) and
// high-frequency (small) motor, 16-bit each.  Stopped on stream end, on quit,
// and while the XMB is open.
void ps3input_set_rumble(unsigned short controller, unsigned short low, unsigned short high);
void ps3input_rumble_stop(void);
void ps3input_set_rumble_paused(int paused);

#endif
