#ifndef VIDEO_H
#define VIDEO_H

#include <Limelight.h>
#include <stdint.h>

extern DECODER_RENDERER_CALLBACKS decoder_callbacks_ps3;

void ps3video_start();
void ps3video_stop();
int  ps3video_is_active();
void ps3video_draw();
int  ps3video_get_current_fps();
int  ps3video_get_decoded_fps();
int  ps3video_get_decode_latency();
int  ps3video_get_render_latency();
int  ps3video_get_net_latency();
uint32_t ps3video_get_total_decoded_frames();
uint32_t ps3video_get_dropped_frames(void);
uint32_t ps3video_get_pics_total(void);    // always counted, for the log line
uint32_t ps3video_get_shown_total(void);
uint32_t ps3video_get_skipped_total(void);
uint32_t ps3video_take_decode_latency_ms(void); // avg since last call
// Tenths of a ms, averaged since the last call (jit_max = worst).
void ps3video_take_frame_timing(uint32_t *hlat, uint32_t *rxt,
                                uint32_t *jit, uint32_t *jit_max);
int ps3video_take_au_peak(void);   // AUs in the decoder, peak since last call
void ps3video_after_flip(void); // diagnostic, call after tiny3d_Flip()
void vdec_poll(void);  // Call from main thread to consume decoded frames

#endif
