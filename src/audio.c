#include "audio.h"
#include "net_logger.h"
#include <stdio.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/event_queue.h>
#include <sys/mutex.h>
#include <lv2/systime.h>
#include <string.h>
#include <opus_multistream.h>

/* The PS3 audio port is 2 or 8 channels wide; PSL1GHT defines nothing else and
   the console rejects a 6-channel port (rc=0x80310704, measured in the Jellyfin
   app).  A 5.1 stream therefore rides an 8-wide port with slots 6/7 zeroed.

   Port slot order is FL FR FC LFE SL SR BL BR.  Moonlight decodes in the order
   FL FR FC LFE BL BR SL SR, so:
     5.1 (mask 0x3F, rears reported as BL/BR) lands on slots 0-5 unchanged;
     7.1 swaps its two rear pairs, done once in the Opus mapping table. */
#define PORT_CHANNELS_MAX 8

/* Size of the temporary buffer used to decode Opus frame.
   Opus supports up to 120ms frame sizes, which translates to
   5760 samples per channel at 48kHz sample rate. */
#define MAX_OPUS_FRAME_SIZE 5760

/* Circular ring buffer size in floats.  A multiple of both port widths, so a
   frame never straddles the wrap: 12288 frames, 256 ms of 48 kHz audio. */
#define RING_FRAMES 12288
#define RING_BUFFER_SIZE (RING_FRAMES * PORT_CHANNELS_MAX)

/* Backlog limits, in frames.  Past BLOAT_FRAMES (~42 ms) the reader skips ahead
   to keep only the newest KEEP_FRAMES (~10.7 ms). */
#define BLOAT_FRAMES 2048
#define KEEP_FRAMES  512

/* Circular ring buffer to bridge the Opus decode callback and the audio playback thread */
static float audio_ring_buffer[RING_BUFFER_SIZE];
static int rb_write_idx = 0;
static int rb_read_idx = 0;
static int rb_size = RING_BUFFER_SIZE; /* RING_FRAMES * port_channels */
static sys_mutex_t rb_mutex;

/* Stream channels (what Opus decodes: 2, 6 or 8) and port width (2 or 8). */
static int stream_channels = 2;
static int port_channels = 2;
static int hq_surround = 0;

/* Thread and audio system handles */
static volatile int active_audio_thread = 0;
static u32 audio_port;
static audioPortConfig audio_cfg;
static sys_event_queue_t audio_queue;
static sys_ipc_key_t audio_key;
static sys_ppu_thread_t audio_thread;

/* Opus Multistream Decoder instance pointer */
static OpusMSDecoder *opus_decoder = NULL;

/* Decode buffer, port-width frames: Opus writes stream-width frames and a 6ch
   stream is then widened to 8 in place. */
static float decode_buffer[MAX_OPUS_FRAME_SIZE * PORT_CHANNELS_MAX];

/* Decoded audio packets counter for visual debug overlay */
static u32 audio_decoded_packets_count = 0;

/* Telemetry.  Decode time is per packet on the AudioRecv thread (it decodes
   inline, CAPABILITY_DIRECT_SUBMIT), so a slow decode also delays receiving.
   Underruns are hardware blocks filled with silence; drops and trims are
   frames discarded for overflow and for backlog. */
static volatile u32 tel_underruns = 0, tel_blocks = 0, tel_underruns_total = 0;
static volatile u32 tel_drop_frames = 0, tel_trim_frames = 0;
static u32 tel_dec_sum_us = 0, tel_dec_max_us = 0, tel_dec_n = 0;
static u64 tel_last_log_us = 0;
static volatile float tel_peak[PORT_CHANNELS_MAX];
/* Last completed interval, read by the HUD. */
static volatile u32 hud_dec_avg_us = 0, hud_dec_max_us = 0;

/**
 * Write port-width frames into the circular ring buffer.
 * If the ring buffer runs out of space, we prioritize real-time low latency
 * by advancing the read pointer, thus discarding the oldest frames to make room
 * for the newest audio packets.  Whole frames only: dropping a partial frame
 * would rotate every channel into its neighbour's speaker.
 */
static void rb_write(const float* data, int num_floats) {
    if (num_floats > rb_size - port_channels) {
        data += num_floats - (rb_size - port_channels);
        num_floats = rb_size - port_channels;
    }

    sysMutexLock(rb_mutex, 0);

    int used = (rb_write_idx - rb_read_idx + rb_size) % rb_size;
    int free_space = rb_size - port_channels - used;

    if (free_space < num_floats) {
        int needed = num_floats - free_space;
        rb_read_idx = (rb_read_idx + needed) % rb_size;
        tel_drop_frames += needed / port_channels;
    }

    for (int i = 0; i < num_floats; i++) {
        audio_ring_buffer[rb_write_idx] = data[i];
        rb_write_idx = (rb_write_idx + 1) % rb_size;
    }

    sysMutexUnlock(rb_mutex);
}

/**
 * Read port-width frames from the circular ring buffer.
 * If there is not enough audio data to fill the request, it returns 0.
 * To keep audio latency minimal, a backlog past BLOAT_FRAMES is cut back to
 * the newest KEEP_FRAMES.
 */
static int rb_read(float* data, int num_floats) {
    sysMutexLock(rb_mutex, 0);

    int available = (rb_write_idx - rb_read_idx + rb_size) % rb_size;
    if (available < num_floats) {
        sysMutexUnlock(rb_mutex);
        return 0; /* Not enough data (underflow) */
    }

    for (int i = 0; i < num_floats; i++) {
        data[i] = audio_ring_buffer[rb_read_idx];
        rb_read_idx = (rb_read_idx + 1) % rb_size;
    }

    int remaining = (rb_write_idx - rb_read_idx + rb_size) % rb_size;
    if (remaining > BLOAT_FRAMES * port_channels) {
        int keep = KEEP_FRAMES * port_channels;
        tel_trim_frames += (remaining - keep) / port_channels;
        rb_read_idx = (rb_write_idx - keep + rb_size) % rb_size;
    }

    sysMutexUnlock(rb_mutex);
    return 1;
}

/* Initialization state tracking flags */
static int audio_system_initialized = 0;
static int audio_thread_started = 0;

/**
 * High-priority audio playback thread loop.
 * It waits for the PS3 audio notification queue to signal that a block in the
 * circular audio hardware buffer is free, then writes the next decoded block of floats.
 */
static void audio_loop(void* arg) {
    (void)arg;
    sys_event_t event;
    u32 current_block = 0;
    static float temp_block[PORT_CHANNELS_MAX * AUDIO_BLOCK_SAMPLES];
    const int block_floats = port_channels * AUDIO_BLOCK_SAMPLES;

    while (active_audio_thread) {
        /* Timeout is 20,000 microseconds (20ms) to ensure thread can exit quickly on shutdown */
        if (sysEventQueueReceive(audio_queue, &event, 20000) == 0) {
            if (!active_audio_thread || !audio_cfg.audioDataStart || audio_cfg.numBlocks == 0) {
                break;
            }

            /* Compute the target hardware buffer address for the current block */
            float* buffer_addr = (float*)((u64)audio_cfg.audioDataStart +
                                (current_block * block_floats * sizeof(float)));

            tel_blocks++;
            if (rb_read(temp_block, block_floats)) {
                memcpy(buffer_addr, temp_block, block_floats * sizeof(float));
                /* Per-slot peak of what reaches the port: a centre that peaks
                   here but is silent in the room is being dropped downstream. */
                for (int i = 0; i < block_floats; i++) {
                    float v = temp_block[i];
                    if (v < 0) v = -v;
                    int c = i % port_channels;
                    if (v > tel_peak[c]) tel_peak[c] = v;
                }
            } else {
                /* Underflow occurred. Write silence to prevent static noise */
                memset(buffer_addr, 0, block_floats * sizeof(float));
                tel_underruns++;
                tel_underruns_total++;
            }

            /* Advance to the next block in the hardware ring buffer */
            current_block = (current_block + 1) % audio_cfg.numBlocks;
        }
    }
    sysThreadExit(0);
}

/**
 * Clean up the audio renderer, stopping threads, closing ports, and freeing resources.
 */
static void ps3_renderer_cleanup() {
    if (!audio_system_initialized) return;

    active_audio_thread = 0;

    if (audio_thread_started) {
        /* Wait for the playback thread to terminate cleanly */
        u64 retval;
        sysThreadJoin(audio_thread, &retval);
        audio_thread_started = 0;
    }

    /* Stop audio port transmission */
    audioPortStop(audio_port);

    /* Unregister the notification event queue */
    audioRemoveNotifyEventQueue(audio_key);

    /* Close port and clean up event queue and mutex */
    audioPortClose(audio_port);
    sysEventQueueDestroy(audio_queue, 0);
    sysMutexDestroy(rb_mutex);

    /* Free Opus decoder instance */
    if (opus_decoder) {
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
    }

    audioQuit();
    memset(&audio_cfg, 0, sizeof(audio_cfg));
    audio_system_initialized = 0;
}

/**
 * Initialize the PS3 audio output system, notify queue, ring buffer mutex, and the Opus decoder.
 */
static int ps3_renderer_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* audioContext, int arFlags) {
    (void)audioContext;
    (void)arFlags;
    /* The host decides the channel count; a request for surround can come back
       as stereo, so accept whatever it sends that the port can carry. */
    if (!opusConfig || opusConfig->sampleRate != 48000 || opusConfig->streams <= 0 ||
        (opusConfig->channelCount != 2 && opusConfig->channelCount != 6 &&
         opusConfig->channelCount != 8)) {
        return -1;
    }
    stream_channels = opusConfig->channelCount;
    port_channels = (stream_channels == 2) ? 2 : 8;
    rb_size = RING_FRAMES * port_channels;
    /* High-quality surround sends every channel as its own uncoupled stream. */
    hq_surround = (stream_channels > 2 && opusConfig->coupledStreams == 0);

    NLOG("audio: requested %dch, host sent %dch (%d streams, %d coupled, %d samples/frame)%s, "
         "%dch port",
         CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(audioConfiguration), stream_channels,
         opusConfig->streams, opusConfig->coupledStreams, opusConfig->samplesPerFrame,
         hq_surround ? " HIGH QUALITY" : "", port_channels);

    if (audioInit() != 0) return -1;

    /* Reset circular buffer indices, packets counter and telemetry */
    rb_write_idx = 0;
    rb_read_idx = 0;
    audio_decoded_packets_count = 0;
    tel_underruns = tel_blocks = tel_drop_frames = tel_trim_frames = 0;
    tel_underruns_total = 0;
    tel_dec_sum_us = tel_dec_max_us = tel_dec_n = 0;
    hud_dec_avg_us = hud_dec_max_us = 0;
    tel_last_log_us = sysGetSystemTime();
    for (int c = 0; c < PORT_CHANNELS_MAX; c++) tel_peak[c] = 0.0f;

    /* Create mutex for ring buffer synchronization */
    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    if (sysMutexCreate(&rb_mutex, &attr) != 0) {
        audioQuit();
        return -1;
    }

    unsigned char mapping[PORT_CHANNELS_MAX];
    memcpy(mapping, opusConfig->mapping, stream_channels);
    if (stream_channels == 8) {
        /* Moonlight BL BR SL SR -> port SL SR BL BR */
        unsigned char t;
        t = mapping[4]; mapping[4] = mapping[6]; mapping[6] = t;
        t = mapping[5]; mapping[5] = mapping[7]; mapping[7] = t;
    }

    /* Initialize Opus Multistream Decoder using parameters provided by Moonlight/Sunshine */
    int error = OPUS_OK;
    opus_decoder = opus_multistream_decoder_create(
        opusConfig->sampleRate,
        stream_channels,
        opusConfig->streams,
        opusConfig->coupledStreams,
        mapping,
        &error
    );
    if (!opus_decoder || error != OPUS_OK) {
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }

    /* 8 blocks of 256 samples either way: the hardware ring is the same 42 ms
       of latency whether it carries stereo or surround. */
    audioPortParam param;
    param.numChannels = (port_channels == 8) ? AUDIO_PORT_8CH : AUDIO_PORT_2CH;
    param.numBlocks = AUDIO_BLOCK_8;
    param.attrib = 0;
    param.level = 1.0f;

    if (audioPortOpen(&param, &audio_port) != 0) {
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }

    if (audioGetPortConfig(audio_port, &audio_cfg) != 0) {
        audioPortClose(audio_port);
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }

    /* Create event queue for block-completion notifications from the audio hardware */
    if (audioCreateNotifyEventQueue(&audio_queue, &audio_key) != 0) {
        audioPortClose(audio_port);
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }

    if (audioSetNotifyEventQueue(audio_key) != 0) {
        audioRemoveNotifyEventQueue(audio_key);
        sysEventQueueDestroy(audio_queue, 0);
        audioPortClose(audio_port);
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }

    /* Start the port before the playback thread so startup rollback cannot
     * leave a thread blocked forever waiting for hardware notifications. */
    if (audioPortStart(audio_port) != 0) {
        audioRemoveNotifyEventQueue(audio_key);
        sysEventQueueDestroy(audio_queue, 0);
        audioPortClose(audio_port);
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }

    /* Create and start the playback thread with high priority (100) */
    active_audio_thread = 1;
    if (sysThreadCreate(&audio_thread, audio_loop, 0, 100, 0x4000, THREAD_JOINABLE, "Audio Thread") != 0) {
        active_audio_thread = 0;
        audioPortStop(audio_port);
        audioRemoveNotifyEventQueue(audio_key);
        sysEventQueueDestroy(audio_queue, 0);
        audioPortClose(audio_port);
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }

    audio_thread_started = 1;
    audio_system_initialized = 1;
    return 0;
}

/* One [PS3-AUD] line every 5 s.  Peaks are percent of full scale per port
   slot, in FL FR FC LFE SL SR BL BR order. */
static void audio_telemetry(u64 now) {
    if (now - tel_last_log_us < 5000000) return;
    tel_last_log_us = now;

    u32 avg = tel_dec_n ? tel_dec_sum_us / tel_dec_n : 0;
    hud_dec_avg_us = avg;
    hud_dec_max_us = tel_dec_max_us;

    int pk[PORT_CHANNELS_MAX];
    for (int c = 0; c < PORT_CHANNELS_MAX; c++) {
        float v = tel_peak[c];
        pk[c] = (int)(v * 100.0f + 0.5f);
        tel_peak[c] = 0.0f;
    }

    NLOG("[PS3-AUD] ch=%d%s dec=%u/%uus pkts=%u under=%u/%u drop=%u trim=%u "
         "peak%%=%d,%d,%d,%d,%d,%d,%d,%d",
         stream_channels, hq_surround ? "hq" : "", (unsigned)avg, (unsigned)tel_dec_max_us,
         (unsigned)tel_dec_n, (unsigned)tel_underruns, (unsigned)tel_blocks,
         (unsigned)tel_drop_frames, (unsigned)tel_trim_frames,
         pk[0], pk[1], pk[2], pk[3], pk[4], pk[5], pk[6], pk[7]);

    tel_dec_sum_us = tel_dec_max_us = tel_dec_n = 0;
    tel_underruns = tel_blocks = tel_drop_frames = tel_trim_frames = 0;
}

/**
 * Decode incoming Opus packet into float PCM samples and push them into the ring buffer.
 */
static void ps3_renderer_decode_and_play_sample(char* data, int length) {
    if (!opus_decoder) return;

    u64 t0 = sysGetSystemTime();
    /* Decode the raw Opus multistream packet directly to float PCM */
    int decoded_samples = opus_multistream_decode_float(
        opus_decoder,
        (data && length > 0) ? (const unsigned char*)data : NULL,
        length,
        decode_buffer,
        MAX_OPUS_FRAME_SIZE,
        0
    );
    if (decoded_samples < 0) {
        /* Failed to decode the Opus frame */
        return;
    }

    if (stream_channels != port_channels) {
        /* Widen 6 -> 8 in place, last frame first so no source is overwritten
           before it is read. */
        for (int i = decoded_samples - 1; i >= 0; i--) {
            float f[6];
            memcpy(f, decode_buffer + i * stream_channels, sizeof(f));
            float *d = decode_buffer + i * port_channels;
            memcpy(d, f, sizeof(f));
            d[6] = 0.0f;
            d[7] = 0.0f;
        }
    }
    u64 t1 = sysGetSystemTime();

    u32 us = (u32)(t1 - t0);
    tel_dec_sum_us += us;
    tel_dec_n++;
    if (us > tel_dec_max_us) tel_dec_max_us = us;

    rb_write(decode_buffer, decoded_samples * port_channels);
    audio_decoded_packets_count++;
    audio_telemetry(t1);
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_ps3 = {
  .init = ps3_renderer_init,
  .cleanup = ps3_renderer_cleanup,
  .decodeAndPlaySample = ps3_renderer_decode_and_play_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

void ps3audio_start() {
  /* Helper function matching interface. Thread starting is handled in init */
}

void ps3audio_stop() {
  ps3_renderer_cleanup();
}

unsigned int ps3audio_get_decoded_packets() {
    return audio_decoded_packets_count;
}

void ps3audio_get_hud(int *channels, int *hq, unsigned *dec_avg_us,
                      unsigned *dec_max_us, unsigned *underruns) {
    *channels = audio_system_initialized ? stream_channels : 0;
    *hq = hq_surround;
    *dec_avg_us = hud_dec_avg_us;
    *dec_max_us = hud_dec_max_us;
    *underruns = tel_underruns_total;
}
