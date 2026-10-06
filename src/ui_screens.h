#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include "input.h"

// One draw_<screen>() per UI_STATE_*, plus the chrome shared by all of them:
// top bar, footer, toasts, the log drawer and the options panel.  Each screen
// also owns its navigation (focus, scroll, panels); anything that changes
// state outside the screen goes through the setters in ui_internal.h.

void ui_screens_init(void);
void ui_screens_input(const ps3_pad_state_t *pad);
void ui_screens_draw(void);          // menus
void ui_screens_draw_hud(void);      // the in-stream overlay
void ui_screens_draw_overlays(void); // toast; drawn in every state

#endif
