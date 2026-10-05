#include <lv2/systime.h>
#include <codec/vdec.h>
#include <io/pad.h>
#include <malloc.h>
#include <ppu-asm.h>
#include <ppu-types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
#include <tiny3d.h>
#include <unistd.h>

#include "video.h"
#include "ui.h"
#include "net_logger.h"
#include <Limelight.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sys/mutex.h>

#define VDEC_MEM_SIZE (10 * 1024 * 1024)

static u64 last_fps_time = 0;
static int frames_this_second = 0;
static int current_video_fps = 0;

static int decoded_frames_this_second = 0;
static int current_decoded_fps = 0;
static u32 total_decoded_frames = 0;

#define SUBMIT_QUEUE_SIZE 16
static u64 submit_queue[SUBMIT_QUEUE_SIZE];
static int submit_head = 0;
static int submit_tail = 0;
static int submit_count = 0;

static u64 decode_latency_sum = 0;
static u32 decode_latency_count = 0;
static u64 render_latency_sum = 0;
static u32 render_latency_count = 0;

static int current_decode_latency_ms = 0;
static int current_render_latency_ms = 0;
static int current_net_latency_ms = 0;

// Triple buffering state
static int front_buf = -1; // Currently displaying
static int write_buf = 0;  // Currently decoding into
static sys_mutex_t frame_mutex;
static int mutex_initialized = 0;

static u32 vdec_handle = 0;
static void *vdec_mem_addr = NULL;
static int vdec_opened = 0;
static int vdec_sequence_started = 0;
static int vdec_module_owned = 0;

// Ping-pong bitstream buffers: one is submitted to VDEC ("back"),
// the other is filled by the network thread ("front").
// We swap them only after AUDONE to guarantee VDEC never reads a
// buffer that is being written or realloc'd.
// AUs in flight inside the decoder.  At 1080p60 the decoder's own latency is
// ~40 ms (TEE PS3 Remoteplay's session logs: 38-44 ms), i.e. 2-3 pictures deep
// at a 16.7 ms cadence, so 4 slots left almost no room before
// submit_decode_unit started throwing AUs away.  8 slots costs 2 MB.
#define VDEC_BUF_COUNT 8
#define VDEC_DECODE_BUF_INITIAL (512 * 1024)
#define VDEC_DECODE_BUF_MAX (4 * 1024 * 1024)
static u8  *vdec_buf[VDEC_BUF_COUNT]   = {NULL, NULL, NULL, NULL};
static u32  vdec_buf_size[VDEC_BUF_COUNT] = {0, 0, 0, 0};
static int  vdec_front    = 0; // index we fill
static int  vdec_back     = 0; // index last submitted
// Legacy aliases kept for minimal diff in the rest of the file:
#define vdec_decode_buffer      vdec_buf[vdec_front]
#define vdec_decode_buffer_size vdec_buf_size[vdec_front]

// Decoded frame buffer (system RAM — VDEC cannot write directly to RSX)
#define VDEC_FRAME_COUNT 4
static void *vdec_frame_bufs[VDEC_FRAME_COUNT] = {NULL, NULL, NULL, NULL}; // memalign'd, size = width*height*4
static u64 frame_ready_time[VDEC_FRAME_COUNT];
static u32 vdec_frame_buf_size = 0;

static volatile int vdec_picout_pending = 0;
// set by callback, cleared by poll

// RSX render target — mapped from system RAM (zero-copy: VDEC writes here, RSX reads directly)
static u32 video_texture_rsx_offsets[VDEC_FRAME_COUNT] = {0, 0, 0, 0};  // RSX offsets of vdec_frame_bufs
static int video_texture_mapped[VDEC_FRAME_COUNT] = {0, 0, 0, 0};

static int video_width = 1280;
static int video_height = 720;

// Output pixel format, latched at setup so it cannot change mid-stream while
// buffers sized for the other layout are still in flight.
static int vdec_yuv_mode = 0;
// Byte offsets of the Y / Cb / Cr planes inside each frame buffer, and the
// macroblock-padded luma height the decoder writes.
static u32 vdec_plane_off[3] = {0, 0, 0};
static u32 vdec_luma_height = 0;

static void yuv_set_plane_layout(u32 w, u32 h) {
  vdec_luma_height = h;
  vdec_plane_off[0] = 0;
  vdec_plane_off[1] = w * h;
  vdec_plane_off[2] = vdec_plane_off[1] + (w / 2u) * (h / 2u);
}
static u32 vdec_detected_pitch = 0; // Set in setup

static int ready_queue[VDEC_FRAME_COUNT];
static int ready_head = 0;
static int ready_tail = 0;
static int ready_count = 0;

static volatile int vdec_au_pending  = 0; // count of AUs in hardware queue
static volatile int vdec_seq_done    = 0; // set by SEQDONE callback

// Frames the client itself threw away, i.e. loss that happened AFTER the
// network delivered the data.  Distinguishing this from network loss matters
// when raising the bitrate: if rx= in the [PS3-NET] line matches the selected
// bitrate but this counter climbs, the ceiling is decode/render, not delivery.
static volatile u32 vdec_dropped_frames = 0;

// Always-on frame accounting for the [PS3-NET] log line.  The HUD counters
// above only tick while the stats overlay is up, so a log taken without it
// could never say whether the decoder kept pace -- the same trap as
// JellyFin-PS3's adec_pes_hwm, which sat behind a build flag and never ran on
// a console.  At 1080p60 the one question is decode rate, so count it always.
//   pics    - pictures the decoder handed back
//   shown   - pictures that reached the screen
//   skipped - decoded but overwritten in the ready queue before display
static volatile u32 vdec_pics_total = 0;
static volatile u32 vdec_shown_total = 0;
static volatile u32 vdec_skipped_total = 0;

// Decode latency (submit -> picture out), always measured, for the log line.
// Per TEE PS3 Remoteplay's analysis this is mostly one frame interval of
// waiting plus ~4-6 ms of real work, so compare it against 1000/fps.
static volatile u64 log_dlat_sum = 0;
static volatile u32 log_dlat_count = 0;

// Per-frame network/host timing from the decode unit, for the log line.  All
// in tenths of a millisecond.
//   hlat - host processing latency the host stamps on every frame (capture to
//          send: shows what x264/AMF actually costs on the PC; 0 = not sent)
//   rxt  - first packet to frame complete on the PS3 (the frame's spread on
//          the wire plus any FEC wait; host send pacing shows up here)
//   jit  - |arrival gap - one frame interval|, mean and worst.  TEE measured
//          the PC's send grid at sigma 1.39 ms; anything well above that is
//          the network or the PS3 receive thread.
static volatile u32 log_hlat_sum = 0, log_hlat_n = 0;
static volatile u32 log_rxt_sum = 0, log_rxt_n = 0;
static volatile u32 log_jit_sum = 0, log_jit_n = 0, log_jit_max = 0;
static u64 log_prev_rx_us = 0;
// Peak access units inside the decoder over the log interval: every one in
// flight is a frame interval of latency before its picture can come out.
static volatile int log_au_peak = 0;

// Display-queue latency trim.  The render loop already paces pictures to the
// vblank (one per flip, FIFO), which is the frame pacing TEE's release lacks.
// The cost of a FIFO is that once it holds two pictures it keeps holding two,
// i.e. a permanent extra refresh of lag after any hiccup.  If it never gets
// shallower than two for ~500 ms, drop the oldest once to get back to one.
// 500 ms is Moonlight's own Pacer window (pacer.cpp: a rolling DisplayFps/2
// history, and it drops only when the queue never fell to the lower mark).
#define READY_TRIM_FRAMES 30
static int ready_deep_frames = 0;
// Dropping a frame silently leaves the decoder referencing data it never got,
// so the picture stays corrupt until the next IDR the host happens to send.
// Ask for one -- but rate-limited, because a sustained overload would otherwise
// turn into an IDR storm, and IDR frames are the largest frames there are.
#define IDR_REQUEST_MIN_INTERVAL_US (2ULL * 1000 * 1000)
static u64 last_idr_request_time = 0;

static int drop_frame_and_maybe_request_idr(void) {
  u64 now = sysGetSystemTime();

  vdec_dropped_frames++;

  if (now - last_idr_request_time >= IDR_REQUEST_MIN_INTERVAL_US) {
    last_idr_request_time = now;
    return DR_NEED_IDR;
  }

  return DR_OK;
}

static void reset_video_runtime_state(void) {
  front_buf = -1;
  write_buf = 0;
  ready_head = 0;
  ready_tail = 0;
  ready_count = 0;
  vdec_front = 0;
  vdec_back = 0;
  vdec_au_pending = 0;
  vdec_picout_pending = 0;
  vdec_seq_done = 0;
  vdec_dropped_frames = 0;
  vdec_pics_total = 0;
  vdec_shown_total = 0;
  vdec_skipped_total = 0;
  log_dlat_sum = 0;
  log_dlat_count = 0;
  ready_deep_frames = 0;
  log_hlat_sum = log_hlat_n = 0;
  log_rxt_sum = log_rxt_n = 0;
  log_jit_sum = log_jit_n = log_jit_max = 0;
  log_prev_rx_us = 0;
  last_idr_request_time = 0;
  submit_head = 0;
  submit_tail = 0;
  submit_count = 0;
  decode_latency_sum = 0;
  decode_latency_count = 0;
  render_latency_sum = 0;
  render_latency_count = 0;
  current_decode_latency_ms = 0;
  current_render_latency_ms = 0;
  current_net_latency_ms = 0;
  frames_this_second = 0;
  decoded_frames_this_second = 0;
  current_video_fps = 0;
  current_decoded_fps = 0;
  last_fps_time = 0;
}

static void release_video_resources(void) {
  if (mutex_initialized) {
    sysMutexDestroy(frame_mutex);
    mutex_initialized = 0;
  }

  if (vdec_sequence_started) {
    vdec_seq_done = 0;
    vdecEndSequence(vdec_handle);
    int timeout = 2000;
    while (!vdec_seq_done && timeout-- > 0) usleep(100);
    vdec_sequence_started = 0;
  }
  if (vdec_opened) {
    vdecClose(vdec_handle);
    vdec_opened = 0;
    vdec_handle = 0;
  }

  if (vdec_mem_addr) {
    free(vdec_mem_addr);
    vdec_mem_addr = NULL;
  }
  for (int bi = 0; bi < VDEC_BUF_COUNT; bi++) {
    free(vdec_buf[bi]);
    vdec_buf[bi] = NULL;
    vdec_buf_size[bi] = 0;
  }
  for (int i = 0; i < VDEC_FRAME_COUNT; i++) {
    if (video_texture_mapped[i]) {
      gcmUnmapEaIoAddress(vdec_frame_bufs[i]);
      video_texture_mapped[i] = 0;
    }
    video_texture_rsx_offsets[i] = 0;
    free(vdec_frame_bufs[i]);
    vdec_frame_bufs[i] = NULL;
  }
  if (vdec_module_owned) {
    sysModuleUnload(SYSMODULE_VDEC);
    vdec_module_owned = 0;
  }

  reset_video_runtime_state();
}

// vdec_callback: runs on RPCS3's internal HLE thread.
// CRITICAL: must NOT make any PS3/LV2 API calls here (causes reentrant crash).
// Just set flags; the submit / main threads act on them.
//
// Callback types per PSL1GHT / Movian:
//   VDEC_CALLBACK_AUDONE  = 1  — AU consumed, buffer free
//   VDEC_CALLBACK_PICOUT  = 2  — decoded picture ready
//   VDEC_CALLBACK_SEQDONE = 3  — EndSequence has completed (async!)
//   VDEC_CALLBACK_ERROR   = 4  — fatal decoder error
static u32 vdec_callback(u32 handle, u32 msgtype, u32 msgdata, u32 arg) {
  (void)handle;
  (void)msgdata;
  (void)arg;
  if (msgtype == VDEC_CALLBACK_PICOUT) {
    vdec_picout_pending = 1;
  } else if (msgtype == VDEC_CALLBACK_AUDONE) {
    if (vdec_au_pending > 0)
        vdec_au_pending--;
  } else if (msgtype == VDEC_CALLBACK_SEQDONE) {
    // vdecEndSequence() has completed asynchronously.
    vdec_seq_done = 1;
  } else if (msgtype == 4 /* VDEC_CALLBACK_ERROR */) {
    printf("VDEC CALLBACK ERROR: 0x%x\n", msgdata);
    vdec_au_pending = 0;
    vdec_seq_done   = 1; // unblock any waiter
  }
  return 0;
}

// Called from the main render thread to consume a pending decoded picture.
void vdec_poll(void) {
  if (!ps3video_is_active() || !vdec_frame_bufs[0] || !vdec_handle)
    return;

  vdec_picout_pending = 0;

  vdecPictureFormat fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.format_type = vdec_yuv_mode ? VDEC_PICFMT_YUV420P : VDEC_PICFMT_ARGB32;
  fmt.color_matrix = VDEC_COLOR_MATRIX_BT709;
  fmt.alpha = 255;

  while (1) {
    s32 ret = vdecGetPicture(vdec_handle, &fmt, vdec_frame_bufs[write_buf]);
    if (ret != 0) break; // No more pictures

    if (submit_count > 0) {
        u64 submit_time = submit_queue[submit_tail];
        submit_tail = (submit_tail + 1) % SUBMIT_QUEUE_SIZE;
        submit_count--;
        u64 lat = sysGetSystemTime() - submit_time;
        log_dlat_sum += lat;
        log_dlat_count++;
        if (ui_get_show_stats()) {
            decode_latency_sum += lat;
            decode_latency_count++;
        }
    }

    u32 pic_addr = 0;
    if (vdecGetPicItem(vdec_handle, &pic_addr) == 0 && pic_addr != 0) {
      vdecPicture *pic = (vdecPicture *)(uintptr_t)pic_addr;
      vdecH264Info *info = (vdecH264Info *)(uintptr_t)pic->codec_specific_addr;
      static int first_pic_logged = 0;
      if (!first_pic_logged && info) {
        first_pic_logged = 1;
        NLOG("VDEC first picture: %ux%u picsize=%u (tight 4:2:0 would be %u), "
             "layout Y@%u Cb@%u Cr@%u luma_h=%u",
               (unsigned)info->width, (unsigned)info->height,
               (unsigned)pic->picture_size,
               (unsigned)(info->width * info->height * 3u / 2u),
               (unsigned)vdec_plane_off[0], (unsigned)vdec_plane_off[1],
               (unsigned)vdec_plane_off[2], (unsigned)vdec_luma_height);
      }
      // Follow the decoder's own picture height for the plane layout (see
      // ps3_video_setup).  Bounded to the buffer, which is sized for the
      // 16-row-padded height.
      if (vdec_yuv_mode && info && info->width == (u32)video_width &&
          info->height > 0 && info->height != vdec_luma_height &&
          info->height <= (((u32)video_height + 15u) & ~15u)) {
        NLOG("VDEC YUV: plane layout follows decoder height %u (was %u), picsize=%u",
               (unsigned)info->height, (unsigned)vdec_luma_height,
               (unsigned)pic->picture_size);
        if (mutex_initialized) sysMutexLock(frame_mutex, 0);
        yuv_set_plane_layout((u32)info->width, (u32)info->height);
        if (mutex_initialized) sysMutexUnlock(frame_mutex);
      }
      if (info && info->height > 0) {
        u32 expected_argb = info->width * info->height * 4;
        u32 expected_yuv  = info->width * info->height * 3 / 2;
        if (pic->picture_size == expected_yuv) {
          vdec_detected_pitch = info->width;
        } else if (pic->picture_size >= expected_argb) {
          vdec_detected_pitch = pic->picture_size / info->height;
        } else {
          vdec_detected_pitch = info->width * 4;
        }
      }
    }
    
    if (ui_get_show_stats()) {
        frame_ready_time[write_buf] = sysGetSystemTime();
    }
    vdec_pics_total++;

    if (mutex_initialized) sysMutexLock(frame_mutex, 0);
    
    // Diagnostic: dump one decoded picture, exactly as the decoder wrote it
    // (whole allocation, padding included), so its layout can be inspected
    // on a PC.  Armed by creating /dev_hdd0/tmp/moonlight_dumpframe.txt over
    // FTP; one-shot (the flag is deleted).  Waits ~3 s into the stream so the
    // picture is not the black first frame.
    {
        static u32 dump_countdown = 180;
        if (dump_countdown && --dump_countdown == 0) {
            FILE *flag = fopen("/dev_hdd0/tmp/moonlight_dumpframe.txt", "r");
            if (flag) {
                fclose(flag);
                remove("/dev_hdd0/tmp/moonlight_dumpframe.txt");
                u32 sz = (vdec_frame_buf_size + 0xFFFFFu) & ~0xFFFFFu;
                FILE *df = fopen("/dev_hdd0/tmp/moonlight_frame.bin", "wb");
                if (df) {
                    size_t wr = fwrite(vdec_frame_bufs[write_buf], 1, sz, df);
                    fclose(df);
                    NLOG("frame dump: %u bytes (%s) %dx%d luma_h=%u Y@%u Cb@%u Cr@%u",
                         (unsigned)wr, vdec_yuv_mode ? "YUV420P" : "ARGB32",
                         video_width, video_height, (unsigned)vdec_luma_height,
                         (unsigned)vdec_plane_off[0], (unsigned)vdec_plane_off[1],
                         (unsigned)vdec_plane_off[2]);
                }
            }
        }
    }

    if (ready_count < VDEC_FRAME_COUNT - 2) {
        ready_queue[ready_head] = write_buf;
        ready_head = (ready_head + 1) % VDEC_FRAME_COUNT;
        ready_count++;
    } else {
        // Drop the oldest frame to make room
        vdec_skipped_total++;
        ready_queue[ready_head] = write_buf;
        ready_head = (ready_head + 1) % VDEC_FRAME_COUNT;
        ready_tail = (ready_tail + 1) % VDEC_FRAME_COUNT;
    }
    
    // Find next available write_buf
    int next_write = (write_buf + 1) % VDEC_FRAME_COUNT;
    while(1) {
        int is_free = 1;
        if (next_write == front_buf) is_free = 0;
        for (int j = 0; j < ready_count; j++) {
            if (ready_queue[(ready_tail + j) % VDEC_FRAME_COUNT] == next_write) {
                is_free = 0;
                break;
            }
        }
        if (is_free) break;
        next_write = (next_write + 1) % VDEC_FRAME_COUNT;
    }
    write_buf = next_write;
    
    if (ui_get_show_stats()) {
        total_decoded_frames++;
        decoded_frames_this_second++;
    }
    if (mutex_initialized) sysMutexUnlock(frame_mutex);
  }
}

static int ps3_video_setup(int videoFormat, int width, int height,
                           int redrawRate, void *context, int drFlags) {
  (void)videoFormat;
  (void)context;
  (void)drFlags;
  printf("PS3 Video Setup: %dx%d @ %d fps\n", width, height, redrawRate);
  reset_video_runtime_state();

  // Load VDEC base module. H264 codec support is built into the main VDEC
  // module on real PS3 hardware. SYSMODULE_VDEC_H264 is a separate SPU firmware
  // module that VDEC loads internally — we must NOT manually load it.
  s32 mod_rc = sysModuleLoad(SYSMODULE_VDEC);
  if (mod_rc == 0) {
    vdec_module_owned = 1;
  } else {
    printf(
        "sysModuleLoad(SYSMODULE_VDEC) returned %d (may already be loaded)\n",
        mod_rc);
  }

  // Decoder configuration, chosen from the stream geometry.
  //
  // H.264 macroblock-rate ceilings: Level 4.1 = 245,760 MB/s, Level 4.2 =
  // 522,240 MB/s.  720p60 = 216,000; 1080p30 = 244,800; 1080p60 = 489,600.
  u32 mb_per_frame = (u32)(((width + 15) / 16) * ((height + 15) / 16));
  u32 mb_per_sec = mb_per_frame * (u32)(redrawRate > 0 ? redrawRate : 60);
  int needs_42 = (mb_per_sec > 245760);
  int is_hd = (width >= 1920 || height >= 1080);

  // The PRACTICAL ceiling on this console is lower than either H.264 level.
  // Two independent measurements agree on roughly 220,000 MB/s: 720p60
  // (216,000) runs well in Cell Stream at ~25 ms, and Cell Stream measured
  // ~27 fps when it tried 1080p, i.e. it saturated at ~220,000.  1080p30 at
  // 244,800 is about 11% over that line.
  //
  // Deblocking is where that 11% comes from.  It is the most expensive stage of
  // H.264 decode -- the stage the IBM Cell work singled out for vectorisation --
  // and cellVdec exposes a pseudo-level that skips it entirely.
  int no_deblock = ui_get_no_deblock();
  if (no_deblock < 0) {
    // Auto: only modes that need Level 4.2 (1080p50/60), which are ~2x over
    // the practical decode ceiling.  720p60 and 1080p30 keep full quality;
    // whether 1080p30 needs it is a hardware A/B the user runs from the menu.
    no_deblock = needs_42;
  }

  // CELL_VDEC_AVC_LEVEL_UNK.  RPCS3's reimplementation of the real SDK enum
  // documents this value as disabling the deblocking filter.  It is tried first
  // and falls through to the ordinary levels if the firmware refuses it, so a
  // wrong guess costs nothing but a log line.
  #define VDEC_LEVEL_NO_DEBLOCK 1042u

  u32 profile_levels[6];
  int num_levels = 0;
  if (no_deblock) profile_levels[num_levels++] = VDEC_LEVEL_NO_DEBLOCK;
  if (needs_42) profile_levels[num_levels++] = 42;
  profile_levels[num_levels++] = 41;
  if (!needs_42) profile_levels[num_levels++] = 42;
  profile_levels[num_levels++] = 40;
  profile_levels[num_levels++] = 32;

  vdecType type;
  type.codec_type = VDEC_CODEC_TYPE_H264;

  vdecConfig config;
  memset(&config, 0, sizeof(config));
  config.ppu_thread_prio = 500;
  config.ppu_thread_stack_size = 0x10000;
  config.spu_thread_prio = 250;

  // SPU threads.  HD gets 3 (JellyFin-PS3's hardware-proven count), and a mode
  // that needs Level 4.2 gets 4 -- Movian's ps3_vdec.c opens every Level 4.2
  // stream with exactly 4.  Keyed off HD, not only the level: 1080p30 sits
  // just UNDER the Level 4.1 line and must not fall back to the 720p count.
  // The work area is sized by vdecQueryAttr from the level alone, so more SPUs
  // cost no memory.  If the firmware refuses the count, the open loop below
  // retries at 3 before giving up.
  {
    int want_spus = ui_get_vdec_spus();
    if (want_spus <= 0) want_spus = needs_42 ? 4 : (is_hd ? 3 : 2);
    config.num_spus = (u32)want_spus;
  }

  static opd32 vdec_callback_opd;
  vdecClosure closure;
  closure.fn = __build_opd32(vdec_callback, &vdec_callback_opd);
  closure.arg = (u32)0;

  printf("VDEC: %dx%d@%d = %u MB/s, hd=%d, deblock=%s, spus=%u\n",
         width, height, redrawRate, mb_per_sec, is_hd,
         no_deblock ? "OFF (speed)" : "on", config.num_spus);

  for (int spu_try = 0; spu_try < 2 && !vdec_opened; spu_try++) {
  if (spu_try == 1) {
    if (config.num_spus <= 3) break;
    printf("VDEC: every level refused %u SPUs - retrying with 3\n", config.num_spus);
    config.num_spus = 3;
  }
  for (int li = 0; li < num_levels && !vdec_opened; li++) {
    type.profile_level = profile_levels[li];

    // Ask the decoder how much memory this level needs rather than guessing.
    // This also probes whether the level exists at all: an unsupported one
    // fails the query instead of wasting open attempts.
    vdecAttr attr;
    memset(&attr, 0, sizeof(attr));
    s32 qret = vdecQueryAttr(&type, &attr);
    if (qret != 0) {
      printf("vdecQueryAttr(level=%u) failed: 0x%x - not supported\n",
             profile_levels[li], (u32)qret);
      continue;
    }
    // A pseudo-level is not in the published table, so do not trust its answer
    // blindly -- an absurd figure means we misread what 1042 does.
    if (attr.mem_size < (4u * 1024 * 1024) || attr.mem_size > (200u * 1024 * 1024)) {
      printf("vdecQueryAttr(level=%u): implausible mem_size=%u - skipping\n",
             profile_levels[li], attr.mem_size);
      continue;
    }
    printf("vdecQueryAttr(level=%u): mem_size=%u (%.1f MB)\n",
           profile_levels[li], attr.mem_size, attr.mem_size / 1048576.0f);

    for (int pass = 0; pass < 2 && !vdec_opened; pass++) {
      u32 want = (pass == 0) ? attr.mem_size : (attr.mem_size + attr.mem_size / 4);
      u32 mem_bytes = (want + (1024 * 1024 - 1)) & ~(1024 * 1024 - 1);

      if (vdec_mem_addr) {
        free(vdec_mem_addr);
        vdec_mem_addr = NULL;
      }
      vdec_mem_addr = memalign(1024 * 1024, mem_bytes);
      if (!vdec_mem_addr) {
        printf("VDEC: failed to allocate %u MB\n", mem_bytes / (1024 * 1024));
        continue;
      }
      config.mem_addr = (u32)(uintptr_t)vdec_mem_addr;
      config.mem_size = mem_bytes;

      s32 rc = vdecOpen(&type, &config, &closure, &vdec_handle);
      if (rc == 0) {
        printf("vdecOpen OK: level=%u mem=%u MB spus=%u deblock=%s\n",
               profile_levels[li], mem_bytes / (1024 * 1024), config.num_spus,
               (profile_levels[li] == VDEC_LEVEL_NO_DEBLOCK) ? "OFF" : "on");
        if (no_deblock && profile_levels[li] != VDEC_LEVEL_NO_DEBLOCK) {
          printf("VDEC: NOTE - deblocking could not be disabled; this mode is "
                 "likely to fall short of %d fps\n", redrawRate);
        }
        if (needs_42 && profile_levels[li] != 42 &&
            profile_levels[li] != VDEC_LEVEL_NO_DEBLOCK) {
          printf("VDEC: WARNING - this mode needs Level 4.2 but opened at %u; "
                 "expect the decoder to fall behind or output black\n",
                 profile_levels[li]);
        }
        vdec_opened = 1;
      } else {
        printf("vdecOpen failed: level=%u mem=%u MB spus=%u rc=0x%x\n",
               profile_levels[li], mem_bytes / (1024 * 1024), config.num_spus,
               (u32)rc);
      }
    }
  }
  }

  if (!vdec_opened) {
    printf("All vdecOpen attempts failed — no H264 decoder available.\n");
    if (vdec_mem_addr) {
      free(vdec_mem_addr);
      vdec_mem_addr = NULL;
    }
    release_video_resources();
    return -1;
  }

  if (vdecStartSequence(vdec_handle) != 0) {
    printf("vdecStartSequence failed\n");
    release_video_resources();
    return -1;
  }
  vdec_sequence_started = 1;

  // Allocate all buffers.
  for (int bi = 0; bi < VDEC_BUF_COUNT; bi++) {
    if (!vdec_buf[bi]) {
      vdec_buf[bi] = memalign(128, VDEC_DECODE_BUF_INITIAL);
      if (!vdec_buf[bi]) {
        printf("Failed to allocate VDEC input buffer %d\n", bi);
        release_video_resources();
        return -1;
      }
      vdec_buf_size[bi] = VDEC_DECODE_BUF_INITIAL;
    }
  }
  vdec_front = 0;
  vdec_back  = 0;

  video_width = width;
  video_height = height;

  // Allocate frame decode buffer in system RAM.
  // gcmMapMainMemory requires 1MB alignment and size rounded up to 1MB.
  vdec_yuv_mode = ui_get_pixel_format();

  if (vdec_yuv_mode) {
    // Tight planar YUV420: Y, then Cb, then Cr, packed at the decoder's
    // REPORTED picture height -- 1080, not the 1088 it decodes in whole
    // macroblocks.  JellyFin-PS3 reads planes at jbuf_fh() (the
    // vdecH264Info height) and is correct on hardware; assuming 1088 put
    // both chroma planes 8 luma rows (15 KB at 1920) off at 1080p and
    // scrambled every colour.  720p never showed it: 720 is a multiple of 16.
    // vdec_poll() re-derives the offsets from each picture's reported height.
    //
    // The buffer itself stays sized for 1088 so a padded write cannot overrun.
    //
    // Plane pitches must stay 64-byte aligned for a linear RSX texture.  Both
    // supported widths satisfy that: luma 1280/1920 and chroma 640/960 are all
    // multiples of 64.
    u32 h_pad = ((u32)height + 15u) & ~15u;
    yuv_set_plane_layout((u32)width, (u32)height);
    vdec_frame_buf_size = (u32)width * h_pad * 3u / 2u;
    vdec_detected_pitch = (u32)width;
  } else {
    vdec_luma_height = (u32)height;
    vdec_plane_off[0] = vdec_plane_off[1] = vdec_plane_off[2] = 0;
    vdec_frame_buf_size = (u32)(width * height * 4);
    vdec_detected_pitch = (u32)width * 4u;
  }

  NLOG("VDEC output: %s, %u bytes/frame",
         vdec_yuv_mode ? "YUV420P" : "ARGB32", vdec_frame_buf_size);

  u32 align_size = (vdec_frame_buf_size + 0xFFFFF) & ~0xFFFFF;

  for (int i = 0; i < VDEC_FRAME_COUNT; i++) {
      if (!vdec_frame_bufs[i]) {
          vdec_frame_bufs[i] = memalign(1024 * 1024, align_size);
          if (!vdec_frame_bufs[i]) {
              printf("Failed to alloc vdec_frame_bufs[%d] (%u bytes)\n", i, align_size);
              release_video_resources();
              return -1;
          }
          memset(vdec_frame_bufs[i], 0, align_size);
          printf("vdec_frame_bufs[%d] allocated: %u bytes (aligned %u) @ %p\n",
                 i, vdec_frame_buf_size, align_size, vdec_frame_bufs[i]);

          s32 map_rc = gcmMapMainMemory(vdec_frame_bufs[i], align_size, &video_texture_rsx_offsets[i]);
          if (map_rc != 0) {
              printf("gcmMapMainMemory failed: 0x%x\n", map_rc);
              release_video_resources();
              return -1;
          } else {
              video_texture_mapped[i] = 1;
              printf("vdec_frame_bufs[%d] mapped to RSX offset: 0x%x\n", i, video_texture_rsx_offsets[i]);
          }
      }
  }

  // No separate RSX texture allocation needed:
  // vdec_frame_bufs are in system RAM and will be used directly via rsxAddressToOffset.

  sys_mutex_attr_t attr;
  sysMutexAttrInitialize(attr);
  if (sysMutexCreate(&frame_mutex, &attr) == 0) {
      mutex_initialized = 1;
      printf("frame_mutex initialized\n");
  } else {
      printf("Failed to initialize frame_mutex\n");
      release_video_resources();
      return -1;
  }

  // Set GCM flip mode: VSYNC (Default) or HSYNC (Immediate Flip / VSync OFF) based on UI settings
  if (ui_get_vsync()) {
      gcmSetFlipMode(GCM_FLIP_VSYNC);
      printf("PS3 Video Setup: VSync ENABLED\n");
  } else {
      gcmSetFlipMode(GCM_FLIP_HSYNC);
      printf("PS3 Video Setup: VSync DISABLED (Immediate Flip)\n");
  }

  ps3video_start();
  printf("PS3 Video Setup complete.\n");
  return 0;
}

static void ps3_video_cleanup() {
  printf("PS3 Video Cleanup\n");
  ps3video_stop();
  usleep(32000); // Wait 2 frames to ensure main thread exits ps3video_draw

  release_video_resources();
}

static int ps3_video_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  if (!decodeUnit || decodeUnit->fullLength <= 0) {
    return DR_OK;
  }

  // Timing first, so dropped frames are still measured.
  if (decodeUnit->frameHostProcessingLatency) {
    log_hlat_sum += decodeUnit->frameHostProcessingLatency;
    log_hlat_n++;
  }
  if (decodeUnit->enqueueTimeUs >= decodeUnit->receiveTimeUs && decodeUnit->receiveTimeUs) {
    log_rxt_sum += (u32)((decodeUnit->enqueueTimeUs - decodeUnit->receiveTimeUs) / 100);
    log_rxt_n++;
  }
  if (decodeUnit->receiveTimeUs) {
    if (log_prev_rx_us && decodeUnit->receiveTimeUs > log_prev_rx_us) {
      int x100 = ui_get_refresh_x100();
      u64 interval_us = x100 > 0 ? (100000000ULL / (u64)x100) : 16683;
      u64 gap = decodeUnit->receiveTimeUs - log_prev_rx_us;
      u64 dev = gap > interval_us ? gap - interval_us : interval_us - gap;
      // A gap of several intervals is a lost frame, not jitter.
      if (dev < interval_us * 3) {
        u32 d = (u32)(dev / 100);
        log_jit_sum += d;
        log_jit_n++;
        if (d > log_jit_max) log_jit_max = d;
      }
    }
    log_prev_rx_us = decodeUnit->receiveTimeUs;
  }
  if ((u32)decodeUnit->fullLength > VDEC_DECODE_BUF_MAX) {
    // An access unit larger than the hard ceiling.  Never clip it into the
    // buffer: a truncated AU still looks well-formed to VDEC and decodes into
    // garbage instead of being reported as a loss.
    printf("VDEC: AU of %d bytes exceeds VDEC_DECODE_BUF_MAX (%d) - dropped\n",
           decodeUnit->fullLength, VDEC_DECODE_BUF_MAX);
    return drop_frame_and_maybe_request_idr();
  }
  // Non-blocking check: if VDEC hardware queue is full, drop this frame
  // immediately.  We must NEVER block here because in queue-based mode the
  // decoder thread must stay responsive to keep draining the decode unit
  // queue.  The main thread's vdec_poll() (1000Hz) is responsible for
  // freeing VDEC slots by consuming decoded pictures.
  if (vdec_au_pending >= VDEC_BUF_COUNT) {
    // Main thread will free slots via vdec_poll(); this frame is gone.
    return drop_frame_and_maybe_request_idr();
  }

  // We now own vdec_front exclusively (VDEC owns vdec_back and it is done).
  // Grow the front buffer if needed — safe because VDEC is NOT reading it.
  u32 needed = (u32)decodeUnit->fullLength + 64;
  if (vdec_buf_size[vdec_front] < needed) {
    u32 new_size = needed + 2048;
    u8 *new_buf = memalign(128, new_size);
    if (new_buf) {
      free(vdec_buf[vdec_front]);
      vdec_buf[vdec_front] = new_buf;
      vdec_buf_size[vdec_front] = new_size;
    } else {
      printf("VDEC: failed to grow front buffer to %u\n", new_size);
      return drop_frame_and_maybe_request_idr();
    }
  }

  // Step 1: Concatenate all fragments (entries) into one continuous buffer
  static int debug_frame_count = 0;
  if (++debug_frame_count % 120 == 1) {
    int entries_count = 0;
    PLENTRY e = decodeUnit->bufferList;
    while (e) { entries_count++; e = e->next; }
    printf("[VDEC] Frame %d: %d NAL units (slices) in decode unit (len=%d)\n",
           debug_frame_count, entries_count, decodeUnit->fullLength);
  }

  u32 length = 0;
  PLENTRY entry = decodeUnit->bufferList;
  while (entry != NULL) {
    if (!entry->data || (u32)entry->length > vdec_decode_buffer_size - length)
      return DR_OK;
    memcpy(vdec_decode_buffer + length, entry->data, entry->length);
    length += entry->length;
    entry = entry->next;
  }
  if (length == 0 || length != (u32)decodeUnit->fullLength) return DR_OK;

  // Step 2: Convert from AVCC (length-prefixed) to Annex B (start-code prefixed)
  // if necessary, handling multi-NALU packets correctly.
  //
  // Moonlight sends H.264 in AVCC format: each NAL unit is prefixed with a
  // 4-byte big-endian length field.  libvdec requires Annex B format where
  // every NAL unit is instead prefixed with the 4-byte start code 00 00 00 01.
  // We must walk the entire buffer and replace ALL length fields, not just the
  // first one, otherwise subsequent NAL units in the same AU are corrupted.
  const u8 start_code[] = {0, 0, 0, 1};
  if (length > 4) {
    // Detect whether the buffer is already in Annex B format.
    int already_annexb = (memcmp(vdec_decode_buffer, start_code, 4) == 0) ||
                         (memcmp(vdec_decode_buffer, start_code + 1, 3) == 0);

    if (!already_annexb) {
      // Assume AVCC: walk the buffer replacing each 4-byte length with
      // 00 00 00 01.  The in-place replacement is safe because both are
      // the same width (4 bytes).
      u32 pos = 0;
      int converted = 0;
      while (pos + 4 <= length) {
        u32 nalu_len = ((u32)vdec_decode_buffer[pos]     << 24) |
                       ((u32)vdec_decode_buffer[pos + 1] << 16) |
                       ((u32)vdec_decode_buffer[pos + 2] <<  8) |
                        (u32)vdec_decode_buffer[pos + 3];

        if (nalu_len == 0 || pos + 4 + nalu_len > length) {
          // Malformed AVCC or not AVCC at all — stop conversion.
          break;
        }

        // Replace the 4-byte length field with the Annex B start code.
        memcpy(vdec_decode_buffer + pos, start_code, 4);
        converted++;
        pos += 4 + nalu_len;
      }

      if (!converted) {
        // Not AVCC and no start code — prepend one start code and submit.
        if (length + 4 <= vdec_decode_buffer_size) {
          memmove(vdec_decode_buffer + 4, vdec_decode_buffer, length);
          memcpy(vdec_decode_buffer, start_code, 4);
          length += 4;
        }
      } else if (pos != length) {
        // A partially valid AVCC frame must not be submitted after mutating
        // only some NAL length fields into Annex B start codes.
        return DR_OK;
      }
    }
  }

  // Submit the FRONT buffer to VDEC.  After this call VDEC takes ownership
  // of vdec_buf[vdec_front]; the AUDONE callback will swap front/back.
  vdecAU au;
  au.packet_addr = (u32)(uintptr_t)vdec_buf[vdec_front];
  au.packet_size = length;
  au.pts.low = 0;
  au.pts.hi = 0;
  au.dts.low = 0;
  au.dts.hi = 0;
  au.userdata = 0;

  vdec_au_pending++;
  if (vdec_au_pending > log_au_peak) log_au_peak = vdec_au_pending;

  s32 dec_rc = vdecDecodeAu(vdec_handle, VDEC_DECODER_MODE_NORMAL, &au);
  if (dec_rc != 0) {
    // 0x80610004 = VDEC_ERROR_BUSY (input queue full)
    if ((u32)dec_rc == 0x80610004) {
      // Retry once immediately in case a slot just freed up
      dec_rc = vdecDecodeAu(vdec_handle, VDEC_DECODER_MODE_NORMAL, &au);
    }
    
    if (dec_rc != 0) {
        if ((u32)dec_rc == 0x80610004) {
            printf("vdecDecodeAu: BUSY (overflow) — dropping AU\n");
        } else {
            printf("vdecDecodeAu failed: 0x%x\n", (u32)dec_rc);
        }
        vdec_au_pending--;
        return drop_frame_and_maybe_request_idr();
    }
  }

  // Success! Cycle the front buffer index.  Timestamps are always recorded:
  // the log line's dlat= needs them whether or not the HUD is up.
  submit_queue[submit_head] = sysGetSystemTime();
  submit_head = (submit_head + 1) % SUBMIT_QUEUE_SIZE;
  if (submit_count < SUBMIT_QUEUE_SIZE) {
      submit_count++;
  } else {
      submit_tail = (submit_tail + 1) % SUBMIT_QUEUE_SIZE;
  }

  vdec_back = vdec_front;
  vdec_front = (vdec_front + 1) % VDEC_BUF_COUNT;

  return DR_OK;
}

u32 ps3video_get_dropped_frames(void) { return vdec_dropped_frames; }
u32 ps3video_get_pics_total(void)     { return vdec_pics_total; }
u32 ps3video_get_shown_total(void)    { return vdec_shown_total; }
u32 ps3video_get_skipped_total(void)  { return vdec_skipped_total; }
// Average decode latency in ms since the last call; resets the accumulator.
int ps3video_take_au_peak(void) {
  int p = log_au_peak;
  log_au_peak = vdec_au_pending;
  return p;
}

void ps3video_take_frame_timing(u32 *hlat, u32 *rxt, u32 *jit, u32 *jit_max) {
  *hlat = log_hlat_n ? log_hlat_sum / log_hlat_n : 0;
  *rxt = log_rxt_n ? log_rxt_sum / log_rxt_n : 0;
  *jit = log_jit_n ? log_jit_sum / log_jit_n : 0;
  *jit_max = log_jit_max;
  log_hlat_sum = log_hlat_n = 0;
  log_rxt_sum = log_rxt_n = 0;
  log_jit_sum = log_jit_n = log_jit_max = 0;
}

u32 ps3video_take_decode_latency_ms(void) {
    u32 n = log_dlat_count;
    u64 sum = log_dlat_sum;
    log_dlat_sum = 0;
    log_dlat_count = 0;
    return n ? (u32)((sum / n + 500) / 1000) : 0;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_ps3 = {
    .setup = ps3_video_setup,
    .cleanup = ps3_video_cleanup,
    .submitDecodeUnit = ps3_video_submit_decode_unit,
    // Queue-based mode: a separate decoder thread dequeues frames and calls
    // submitDecodeUnit(), keeping the recv thread free to drain the socket.
    // The decode unit queue is sized to 120 frames on PS3 to absorb transient
    // VDEC stalls without overflowing.
    .capabilities = CAPABILITY_SLICES_PER_FRAME(4)};

// Video Thread flag
static int active_video_thread = 0;

void ps3video_start() { active_video_thread = 1; }

void ps3video_stop() { active_video_thread = 0; }

int ps3video_is_active() { return active_video_thread; }

int ps3video_get_current_fps() { return current_video_fps; }
int ps3video_get_decoded_fps() { return current_decoded_fps; }
int ps3video_get_decode_latency() { return current_decode_latency_ms; }
int ps3video_get_render_latency() { return current_render_latency_ms; }
int ps3video_get_net_latency() { return current_net_latency_ms; }
u32 ps3video_get_total_decoded_frames() { return total_decoded_frames; }

// ---------------------------------------------------------------------------
// YUV self-test.  Armed by /dev_hdd0/tmp/moonlight_yuvtest.txt (one-shot).
// About 4 s into a YUV stream it draws four phases of 20 frames, each with a
// different chroma binding, and after each phase captures the screen buffers
// (every 4th pixel, every 4th row) to /dev_hdd0/tmp/moonlight_fb_<phase>_<n>.bin:
//   A = unit1 Cb, unit2 Cr   (what the shader decode says is right)
//   B = unit1 Cr, unit2 Cb
//   C = both Cb              D = both Cr
// If the binds reach the shader, the four captures differ clearly.
// ---------------------------------------------------------------------------
extern u32 *Video_buffer[2];
extern int Video_pitch;
static int yuv_test_state = 0;      // 0 not checked, 1 running, 2 done/off
static u32 yuv_test_frames = 0;
static int yuv_test_phase = 0;      // 0..3 = A..D
static volatile int yuv_test_capture = -1; // phase index awaiting capture

static void yuv_test_select(u32 cb, u32 cr, u32 *u1, u32 *u2) {
  if (yuv_test_state == 0) {
    if (++yuv_test_frames < 240) return;
    FILE *flag = fopen("/dev_hdd0/tmp/moonlight_yuvtest.txt", "r");
    if (flag) {
      fclose(flag);
      remove("/dev_hdd0/tmp/moonlight_yuvtest.txt");
      yuv_test_state = 1;
      yuv_test_frames = 0;
      yuv_test_phase = 0;
      NLOG("yuv_test: starting (A=Cb/Cr B=Cr/Cb C=Cb/Cb D=Cr/Cr)");
    } else {
      yuv_test_state = 2;
    }
  }
  if (yuv_test_state != 1) return;
  switch (yuv_test_phase) {
    case 0: *u1 = cb; *u2 = cr; break;
    case 1: *u1 = cr; *u2 = cb; break;
    case 2: *u1 = cb; *u2 = cb; break;
    default: *u1 = cr; *u2 = cr; break;
  }
  if (++yuv_test_frames >= 20 && yuv_test_capture < 0) {
    yuv_test_capture = yuv_test_phase; // captured after this frame's flip
    yuv_test_frames = 0;
    if (++yuv_test_phase > 3) yuv_test_state = 2;
  }
}

// Called by the UI right after tiny3d_Flip().
void ps3video_after_flip(void) {
  int ph = yuv_test_capture;
  if (ph < 0) return;
  yuv_test_capture = -1;
  int w = ui_get_width(), h = ui_get_height();
  if (w <= 0 || h <= 0 || Video_pitch <= 0) return;
  for (int b = 0; b < 2; b++) {
    if (!Video_buffer[b]) continue;
    char path[64];
    snprintf(path, sizeof(path), "/dev_hdd0/tmp/moonlight_fb_%c_%d.bin", 'A' + ph, b);
    FILE *f = fopen(path, "wb");
    if (!f) continue;
    u32 ow = (u32)w / 4u, oh = (u32)h / 4u;
    u32 hdr[2] = {ow, oh};
    fwrite(hdr, sizeof(hdr), 1, f);
    static u32 row[1920 / 4];
    for (u32 y = 0; y < oh; y++) {
      const volatile u32 *src = (const volatile u32 *)((const u8 *)Video_buffer[b] + (size_t)(y * 4u) * (u32)Video_pitch);
      for (u32 x = 0; x < ow && x < 1920u / 4u; x++) row[x] = src[x * 4u];
      fwrite(row, 4, ow, f);
    }
    fclose(f);
  }
  NLOG("yuv_test: captured phase %c", 'A' + ph);
}

// Bind one 8-bit plane (Y, Cb or Cr) on a texture unit.
//
// GCM_TEXTURE_FORMAT_B8 is NV's L8: the single byte broadcasts to all channels,
// so tiny3d's YUV shader reads each plane's sample as .x.  The chroma planes are
// half size in both axes and the texture unit bilinearly upsamples them for
// free, which is the whole point of handing the RSX planar data.
static void bind_yuv_plane(gcmContextData *ctx, int unit, u32 offset,
                           u32 w, u32 h, u32 pitch) {
  gcmTexture tex;
  memset(&tex, 0, sizeof(tex));
  tex.format    = 0x20 | 0x81; // LIN | B8
  tex.mipmap    = 1;
  tex.dimension = 2; // GCM_TEXTURE_DIMS_2D
  tex.cubemap   = 0;
  // Same identity remap the ARGB path uses.
  tex.remap     = (2 << 14) | (2 << 12) | (2 << 10) | (2 << 8) |
                  (3 << 6)  | (2 << 4)  | (1 << 2)  | (0 << 0);
  tex.width     = (u16)w;
  tex.height    = (u16)h;
  tex.depth     = 1;
  tex.location  = GCM_LOCATION_CELL; // main RAM, IOMMU mapped
  tex.pitch     = pitch;
  tex.offset    = offset;

  rsxLoadTexture(ctx, unit, &tex);
  rsxTextureControl(ctx, unit, GCM_TRUE, 0, 12 << 8, 1);
  rsxTextureFilter(ctx, unit, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
                   GCM_TEXTURE_CONVOLUTION_QUINCUNX);
  rsxTextureWrapMode(ctx, unit, GCM_TEXTURE_CLAMP_TO_EDGE,
                     GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                     0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

void ps3video_draw() {
  if (!ps3video_is_active() || !vdec_frame_bufs[0])
    return;

  if (ui_get_show_stats()) {
      u64 now = sysGetSystemTime();
      if (last_fps_time == 0) last_fps_time = now;
      if (now - last_fps_time >= 1000000) {
        current_video_fps = frames_this_second;
        current_decoded_fps = decoded_frames_this_second;
        
        if (decode_latency_count > 0) {
            current_decode_latency_ms = (int)((decode_latency_sum / decode_latency_count) / 1000);
        } else {
            current_decode_latency_ms = 0;
        }
        if (render_latency_count > 0) {
            current_render_latency_ms = (int)((render_latency_sum / render_latency_count) / 1000);
        } else {
            current_render_latency_ms = 0;
        }

        uint32_t rtt = 0;
        if (LiGetEstimatedRttInfo(&rtt, NULL)) {
            current_net_latency_ms = (int)rtt;
        } else {
            current_net_latency_ms = 0;
        }

        decode_latency_sum = 0;
        decode_latency_count = 0;
        render_latency_sum = 0;
        render_latency_count = 0;

        frames_this_second = 0;
        decoded_frames_this_second = 0;
        last_fps_time = now;
      }
  }

  if (mutex_initialized) sysMutexLock(frame_mutex, 0);
  int ll = ui_get_low_latency();
  if (ll >= 2) {
      // Newest picture wins.  Measured too aggressive over Wi-Fi: with ~12 ms
      // arrival jitter pictures land in pairs, and Cyberpunk showed 40 of 60
      // decoded per second (skip ~18/s).
      while (ready_count > 1) {
          ready_tail = (ready_tail + 1) % VDEC_FRAME_COUNT;
          ready_count--;
          vdec_skipped_total++;
      }
      ready_deep_frames = 0;
  } else if (ready_count >= 2) {
      // FIFO absorbs a pair that lands in one refresh; a queue that STAYS two
      // deep is lag, and is trimmed after 6 vblanks (balanced, default) or
      // 30 (smooth).
      if (++ready_deep_frames >= (ll == 1 ? 6 : READY_TRIM_FRAMES)) {
          ready_tail = (ready_tail + 1) % VDEC_FRAME_COUNT;
          ready_count--;
          vdec_skipped_total++;
          ready_deep_frames = 0;
      }
  } else {
      ready_deep_frames = 0;
  }
  if (ready_count > 0) {
      front_buf = ready_queue[ready_tail];
      ready_tail = (ready_tail + 1) % VDEC_FRAME_COUNT;
      ready_count--;
      vdec_shown_total++;
      if (ui_get_show_stats()) {
          frames_this_second++;
          render_latency_sum += (sysGetSystemTime() - frame_ready_time[front_buf]);
          render_latency_count++;
      }
  }
  int draw_idx = front_buf;
  if (mutex_initialized) sysMutexUnlock(frame_mutex);

  if (draw_idx < 0 || video_texture_rsx_offsets[draw_idx] == 0) return;

  // tiny3d_SetTexture() hardcodes GCM_LOCATION_RSX (local memory).
  // Our frame buffer is in main (Cell) memory via gcmMapMainMemory.
  // We must use rsxLoadTexture() directly with GCM_LOCATION_CELL.
  gcmContextData *ctx = (gcmContextData *)tiny3d_Get_GCM_Context();
  if (ctx && vdec_yuv_mode) {
    // Planar YUV420: three L8 planes on units 0/1/2.  tiny3d's built-in YUV
    // shader (USE_YUV_8BIT_TEXTURES) samples all three from TEXTURE0's
    // coordinates, so the quad below needs no change.
    u32 base = video_texture_rsx_offsets[draw_idx];
    u32 cw = (u32)video_width / 2u;
    u32 ch = vdec_luma_height / 2u;

    tiny3d_DoCmd_Space(192); // three texture setups, not one
    bind_yuv_plane(ctx, 0, base + vdec_plane_off[0],
                   (u32)video_width, (u32)video_height, (u32)video_width);
    // tiny3d's yuv8 shader (decoded from nv_shaders.h): unit 1 feeds only
    // the B and G terms (x2.018, x0.391) so it is Cb; unit 2 feeds R and G
    // (x1.596, x0.813) so it is Cr.  The decoder's planes are Y|Cb|Cr (a raw
    // frame dump rendered on a PC proves it).  On the TV both this order and
    // the reverse looked identical ("blue and gold"), so whether these binds
    // reach the shader at all is what yuv_test (below) measures.
    u32 cb = base + vdec_plane_off[1], cr = base + vdec_plane_off[2];
    u32 u1 = cb, u2 = cr;
    yuv_test_select(cb, cr, &u1, &u2);
    bind_yuv_plane(ctx, 1, u1, cw, ch, cw);
    bind_yuv_plane(ctx, 2, u2, cw, ch, cw);

    tiny3d_Enable_YUV(USE_YUV_8BIT_TEXTURES);
  } else if (ctx) {
    gcmTexture tex;
    memset(&tex, 0, sizeof(tex));
    // Linear ARGB32 texture format:
    // GCM_TEXTURE_FORMAT_LIN (0x20) | GCM_TEXTURE_FORMAT_A8R8G8B8 (0x85)
    tex.format    = 0x20 | 0x85; // LIN | A8R8G8B8
    tex.mipmap    = 1;
    tex.dimension = 2; // GCM_TEXTURE_DIMS_2D
    tex.cubemap   = 0;
    // Identity remap (ARGB -> ARGB):
    // Source components (indices): A=0, R=1, G=2, B=3
    // Destination (out): B=bits 6-7, G=bits 4-5, R=bits 2-3, A=bits 0-1
    tex.remap     = (2 << 14) | (2 << 12) | (2 << 10) | (2 << 8) | // All types = REMAP
                    (3 << 6)  | (2 << 4)  | (1 << 2)  | (0 << 0);  // B=3, G=2, R=1, A=0
    tex.width     = (u16)video_width;
    tex.height    = (u16)video_height;
    tex.depth     = 1;
    tex.location  = GCM_LOCATION_CELL; // 1 = main RAM (IOMMU mapped)
    tex.pitch     = (u32)(video_width * 4);
    tex.offset    = video_texture_rsx_offsets[draw_idx]; // from gcmMapMainMemory

    tiny3d_DoCmd_Space(64); // ensure command buffer has space
    rsxLoadTexture(ctx, 0, &tex);
    rsxTextureControl(ctx, 0, GCM_TRUE, 0, 12 << 8, 1); // enable, min/mag lod, maxaniso=1
    rsxTextureFilter(ctx, 0, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
                     GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(ctx, 0, GCM_TEXTURE_CLAMP_TO_EDGE,
                       GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                       0, GCM_TEXTURE_ZFUNC_LESS, 0);
  }

  int screen_width = ui_get_width();
  int screen_height = ui_get_height();
  if (screen_width <= 0 || screen_height <= 0) {
      screen_width = video_width;
      screen_height = video_height;
  }

  float qx, qy, qw, qh;
  ui_stream_rect(screen_width, screen_height, &qx, &qy, &qw, &qh);

  tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);

  // Top-Left
  tiny3d_VertexPos(qx, qy, 65535);
  tiny3d_VertexTexture(0.0f, 0.0f);

  // Top-Right
  tiny3d_VertexPos(qx + qw, qy, 65535);
  tiny3d_VertexTexture(1.0f, 0.0f);

  // Bottom-Left
  tiny3d_VertexPos(qx, qy + qh, 65535);
  tiny3d_VertexTexture(0.0f, 1.0f);

  // Bottom-Right
  tiny3d_VertexPos(qx + qw, qy + qh, 65535);
  tiny3d_VertexTexture(1.0f, 1.0f);

  tiny3d_End();

  // Leave the shader as we found it: everything else drawn this frame (the HUD,
  // the menus) is ordinary RGB and would come out miscoloured otherwise.
  if (vdec_yuv_mode) {
    tiny3d_Disable_YUV();
  }
}
