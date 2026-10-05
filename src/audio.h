#ifndef AUDIO_H
#define AUDIO_H

#include <Limelight.h>

extern AUDIO_RENDERER_CALLBACKS audio_callbacks_ps3;

void ps3audio_start();
void ps3audio_stop();
unsigned int ps3audio_get_decoded_packets();
// Stats overlay readout.  channels is 0 when no stream is playing; decode
// times are the last 5 s interval; underruns count since the stream began.
void ps3audio_get_hud(int *channels, int *hq, unsigned *dec_avg_us,
                      unsigned *dec_max_us, unsigned *underruns);

#endif
