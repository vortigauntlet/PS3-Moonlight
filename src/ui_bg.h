#ifndef UI_BG_H
#define UI_BG_H

// The menu background: sky gradient, a moon whose halo and light shafts
// fall across the whole screen, and two XMB-style wave ribbons across the
// lower third.  Cheap (about 600 vertices) and never drawn while streaming.
void ui_bg_draw(void);
// Drawn after the screen: a faint additive wash of moonlight over everything.
void ui_bg_draw_light(void);

#endif
