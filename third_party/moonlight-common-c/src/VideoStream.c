#include "Limelight-internal.h"

#define FIRST_FRAME_MAX 1500
#define FIRST_FRAME_TIMEOUT_SEC 10

#define FIRST_FRAME_PORT 47996

static RTP_VIDEO_QUEUE rtpQueue;

static SOCKET rtpSocket = INVALID_SOCKET;
static SOCKET firstFrameSocket = INVALID_SOCKET;

static PPLT_CRYPTO_CONTEXT decryptionCtx;

static PLT_THREAD udpPingThread;
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

static bool receivedDataFromPeer;
static uint64_t firstDataTimeMs;
static bool receivedFullFrame;

#ifdef __PPU__
// Published for the on-screen stats overlay so the measured receive rate can be
// compared against the requested bitrate without reading the log.
volatile int ps3_video_rx_kbps = 0;
volatile int ps3_video_rxq_bytes = 0;
#endif

// We can't request an IDR frame until the depacketizer knows
// that a packet was lost. This timeout bounds the time that
// the RTP queue will wait for missing/reordered packets.
// PS3's tiny kernel UDP buffer causes late packet delivery.
// On real hardware (LAN), 15ms is enough to catch reordered packets
// while keeping video latency low.  Higher values increase recovery
// time after packet loss because the depacketizer waits longer before
// requesting an IDR frame.
#ifdef __PPU__
#define RTP_QUEUE_DELAY 15
#else
#define RTP_QUEUE_DELAY 10
#endif

// This is the desired number of video packets that can be
// stored in the socket's receive buffer. 2048 is chosen
// because it should be large enough for all reasonable
// frame sizes (probably 2 or 3 frames) without using too
// much kernel memory with larger packet sizes. It also
// can smooth over transient pauses in network traffic
// and subsequent packet/frame bursts that follow.
#define RTP_RECV_PACKETS_BUFFERED 2048

// Initialize the video stream
void initializeVideoStream(void) {
    initializeVideoDepacketizer(StreamConfig.packetSize);
    RtpvInitializeQueue(&rtpQueue);
    decryptionCtx = PltCreateCryptoContext();
    receivedDataFromPeer = false;
    firstDataTimeMs = 0;
    receivedFullFrame = false;
}

// Clean up the video stream
void destroyVideoStream(void) {
    PltDestroyCryptoContext(decryptionCtx);
    destroyVideoDepacketizer();
    RtpvCleanupQueue(&rtpQueue);
}

// UDP Ping proc
static void VideoPingThreadProc(void* context) {
    char legacyPingData[] = { 0x50, 0x49, 0x4E, 0x47 };
    LC_SOCKADDR saddr;

    LC_ASSERT(VideoPortNumber != 0);

    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, VideoPortNumber);

    // We do not check for errors here. Socket errors will be handled
    // on the read-side in ReceiveThreadProc(). This avoids potential
    // issues related to receiving ICMP port unreachable messages due
    // to sending a packet prior to the host PC binding to that port.
    int pingCount = 0;
    while (!PltIsThreadInterrupted(&udpPingThread)) {
        if (VideoPingPayload.payload[0] != 0) {
            pingCount++;
            VideoPingPayload.sequenceNumber = BE32(pingCount);

            sendto(rtpSocket, (char*)&VideoPingPayload, sizeof(VideoPingPayload), 0, (struct sockaddr*)&saddr, AddrLen);
        }
        else {
            sendto(rtpSocket, legacyPingData, sizeof(legacyPingData), 0, (struct sockaddr*)&saddr, AddrLen);
        }

        PltSleepMsInterruptible(&udpPingThread, 500);
    }
}

// Receive thread proc
static void VideoReceiveThreadProc(void* context) {
    int err;
    int bufferSize, receiveSize, decryptedSize, minSize;
    char* buffer;
    char* encryptedBuffer;
    int queueStatus;
    bool useSelect;
    int waitingForVideoMs;
    bool encrypted;

    encrypted = !!(EncryptionFeaturesEnabled & SS_ENC_VIDEO);
    decryptedSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    minSize = sizeof(RTP_PACKET) + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    receiveSize = decryptedSize + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    bufferSize = decryptedSize + sizeof(RTPV_QUEUE_ENTRY);
    buffer = NULL;

    if (setNonFatalRecvTimeoutMs(rtpSocket, UDP_RECV_POLL_TIMEOUT_MS) < 0) {
        // SO_RCVTIMEO failed, so use select() to wait
        useSelect = true;
    }
    else {
        // SO_RCVTIMEO timeout set for recv()
        useSelect = false;
    }

    // Allocate a staging buffer to use for each received packet
    if (encrypted) {
        encryptedBuffer = (char*)malloc(receiveSize);
        if (encryptedBuffer == NULL) {
            Limelog("Video Receive: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }
    }
    else {
        encryptedBuffer = NULL;
    }

    waitingForVideoMs = 0;
    
    // Pre-allocate a batch buffer.  On PS3 real hardware the smaller batch
    // reduces time spent in the drain loop, giving the main thread more
    // opportunities to call vdec_poll() and avoid VDEC back-pressure.
    #ifdef __PPU__
    #define VIDEO_BATCH_SIZE 16
    #define VIDEO_MAX_PACKET_SIZE 1500
    #else
    #define VIDEO_BATCH_SIZE 32
    #define VIDEO_MAX_PACKET_SIZE 2048
    #endif
    static char batch_data[VIDEO_BATCH_SIZE][VIDEO_MAX_PACKET_SIZE];
    static int  batch_lens[VIDEO_BATCH_SIZE];

    // recvUdpSocket() writes up to receiveSize bytes into a fixed-size staging
    // slot.  packetSize is now user-selectable, so clamp rather than trust it:
    // a datagram larger than the slot is truncated (and then rejected) instead
    // of running off the end of the array.
    if (receiveSize > VIDEO_MAX_PACKET_SIZE) {
        Limelog("Video Receive: clamping receiveSize %d to %d\n",
                receiveSize, VIDEO_MAX_PACKET_SIZE);
        receiveSize = VIDEO_MAX_PACKET_SIZE;
    }

#ifdef __PPU__
    // PS3 network diagnostics.  This line is the primary instrument for finding
    // the console's real bitrate ceiling, so it carries everything needed to
    // tell the failure modes apart:
    //
    //   rx= tracks the selected bitrate, fecrec/fecfail near 0
    //        -> the receive path is keeping up; the next step up is worth trying.
    //   rx= falls short of the selected bitrate, fecfail climbing
    //        -> packets are lost before we read them; this step is over the
    //           ceiling, come back down one.
    //   rxq=/rxqpk= are normally -1. netGetSockInfo is UNIMPLEMENTED on this
    //        firmware -- it returns -1 on a stock libnet init too -- so -1 says
    //        nothing about the socket, only about that call. The instruments
    //        that do work here are rx= against the requested bitrate, fecfail=,
    //        and the startup "net: probe" line.
    //   batch= pinned at VIDEO_BATCH_SIZE -> the drain loop itself is the limit,
    //        not the buffer.
    //
    // fecrec/fecfail/oos are per-interval deltas; recv= is cumulative.
    static int ps3_net_pkts_total = 0;
    static uint64_t ps3_net_bytes_total = 0;
    static int ps3_net_batch_max = 0;
    static uint64_t ps3_net_last_log = 0;
    static uint32_t ps3_prev_fec_rec = 0, ps3_prev_fec_fail = 0, ps3_prev_oos = 0;
    // Decoder-side counters from src/video.c.  Always counted (not HUD-gated),
    // so every log says whether the DECODER kept pace, not just the network:
    //   dec  = pictures decoded this second   (1080p60 target: 60)
    //   shw  = pictures put on screen
    //   skip = decoded but replaced before display
    //   drop = access units thrown away before decode (VDEC queue full)
    extern uint32_t ps3video_get_pics_total(void);
    extern uint32_t ps3video_get_shown_total(void);
    extern uint32_t ps3video_get_skipped_total(void);
    extern uint32_t ps3video_get_dropped_frames(void);
    extern uint32_t ps3video_take_decode_latency_ms(void);
    extern int ps3video_take_au_peak(void);
    extern int ps3_decode_queue_depth(void); // VideoDepacketizer.c
    extern void ps3video_take_frame_timing(uint32_t *hlat, uint32_t *rxt,
                                           uint32_t *jit, uint32_t *jit_max);
    static uint32_t ps3_prev_dec = 0, ps3_prev_shw = 0, ps3_prev_skip = 0, ps3_prev_drop = 0;
    // video.c zeroes its totals when the decoder is set up; never report that
    // as a 4-billion-frame negative delta.
#define PS3_DELTA(cur, prev) ((cur) >= (prev) ? (cur) - (prev) : (cur))
    // Peak bytes actually queued in the kernel socket buffer over the interval.
    //
    // This matters more than buf=.  getsockopt(SO_RCVBUF) reports the value that
    // was ASKED FOR, not the memory libnet actually funded it with -- on the
    // stock 128 KB pool a 512 KB request reads back as 512 KB and buffers
    // nothing like it.  A large buf= next to a rxqpk= that never climbs past
    // ~60 KB is the signature of a buffer that was accepted but not backed.
    static int ps3_rxq_peak = 0;
    extern volatile int ps3_enobufs_count;   // defined in PlatformSockets.c
    extern volatile int ps3_accepted_rcvbuf; // defined in PlatformSockets.c

    // The counters above are function-scope statics, so they survive into the
    // next stream, while rtpQueue.stats is zeroed by RtpvInitializeQueue().
    // Reset them here or the first interval of a second stream reports a delta
    // against the previous stream's totals, which underflows.
    ps3_net_pkts_total = 0;
    ps3_net_bytes_total = 0;
    ps3_net_batch_max = 0;
    ps3_rxq_peak = 0;
    ps3_net_last_log = 0;
    ps3_prev_fec_rec = 0;
    ps3_prev_fec_fail = 0;
    ps3_prev_oos = 0;
    ps3_prev_dec = ps3_prev_shw = ps3_prev_skip = ps3_prev_drop = 0;
    ps3_video_rx_kbps = 0;
    ps3_video_rxq_bytes = 0;
#endif

    while (!PltIsThreadInterrupted(&receiveThread)) {
        int batchCount = 0;

        // Phase 1: Drain Phase - Empty the kernel socket buffer into our local batch
        // as fast as the PPU can call recv(). We avoid ALL processing here.
        for (batchCount = 0; batchCount < VIDEO_BATCH_SIZE; batchCount++) {
            err = recvUdpSocket(rtpSocket,
                                batch_data[batchCount],
                                receiveSize,
                                (batchCount == 0) ? useSelect : false);
            
            if (err < 0) {
                if (batchCount > 0 && (LastSocketError() == EWOULDBLOCK || LastSocketError() == EAGAIN)) {
                    break;
                }
                Limelog("Video Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError());
                ListenerCallbacks.connectionTerminated(LastSocketFail());
                goto Exit;
            }
            else if (err == 0) {
                if (batchCount > 0) break; 
                
                if (!receivedDataFromPeer) {
                    waitingForVideoMs += UDP_RECV_POLL_TIMEOUT_MS;
                    if (waitingForVideoMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                        Limelog("Terminating connection due to lack of video traffic\n");
                        ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_TRAFFIC);
                        goto Exit;
                    }
                }
                break;
            }
            batch_lens[batchCount] = err;
#ifdef __PPU__
            ps3_net_bytes_total += (uint64_t)err;
#endif
        }

        // Phase 2: Process Phase - Now that the network buffer is safe, 
        // perform the heavy lifting (Decryption, Memory Allocation, Queueing).
        for (int i = 0; i < batchCount; i++) {
            int current_len = batch_lens[i];
            char* current_raw = batch_data[i];

            // Successfully received a packet
            if (!receivedDataFromPeer) {
                receivedDataFromPeer = true;
                Limelog("Received first video packet after %d ms\n", waitingForVideoMs);
                firstDataTimeMs = PltGetMillis();
            }

#ifndef LC_FUZZING
            if (!receivedFullFrame) {
                if (PltGetMillis() - firstDataTimeMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                    Limelog("Terminating connection due to lack of a successful video frame\n");
                    ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_FRAME);
                    goto Exit;
                }
            }
#endif

            if (current_len < minSize) {
                continue;
            }

            // Ensure we have a buffer to decrypt into
            if (buffer == NULL) {
                buffer = (char*)malloc(bufferSize);
                if (buffer == NULL) {
                    Limelog("Video Receive: malloc() failed\n");
                    ListenerCallbacks.connectionTerminated(-1);
                    goto Exit;
                }
            }

            // Decrypt the packet if encryption is enabled
            if (encrypted) {
                PENC_VIDEO_HEADER encHeader = (PENC_VIDEO_HEADER)current_raw;

                if (encHeader->frameNumber && LE32(encHeader->frameNumber) < RtpvGetCurrentFrameNumber(&rtpQueue)) {
                    continue;
                }

                if (!PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                                    (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                                    encHeader->iv, sizeof(encHeader->iv),
                                    encHeader->tag, sizeof(encHeader->tag),
                                    ((unsigned char*)(encHeader + 1)), current_len - sizeof(ENC_VIDEO_HEADER),
                                    (unsigned char*)buffer, &current_len)) {
                    Limelog("Failed to decrypt video packet!\n");
                    continue;
                }
            } else {
                // If not encrypted, copy from batch to Moonlight-owned buffer
                memcpy(buffer, current_raw, current_len);
            }

            // Convert fields to host byte-order
            PRTP_PACKET packet = (PRTP_PACKET)&buffer[0];
            packet->sequenceNumber = BE16(packet->sequenceNumber);
            packet->timestamp = BE32(packet->timestamp);
            packet->ssrc = BE32(packet->ssrc);

            queueStatus = RtpvAddPacket(&rtpQueue, packet, current_len, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);

            if (queueStatus == RTPF_RET_QUEUED) {
                // The queue takes ownership of the buffer
                buffer = NULL;
            }
        }

#ifdef __PPU__
        // Update PS3 network stats and log periodically
        ps3_net_pkts_total += batchCount;
        if (batchCount > ps3_net_batch_max) ps3_net_batch_max = batchCount;
        {
            uint64_t now = PltGetMillis();
            if (ps3_net_last_log == 0) ps3_net_last_log = now;
            if (now - ps3_net_last_log >= 1000) {
                uint32_t elapsed = (uint32_t)(now - ps3_net_last_log);
                // Throughput actually reaching the app, in kbps.
                uint32_t kbps = elapsed ? (uint32_t)((ps3_net_bytes_total * 8) / elapsed) : 0;
                RTP_VIDEO_STATS* st = &rtpQueue.stats;
                int rxq = -1;

                // Kept because it costs nothing and would be genuinely useful
                // if a firmware or CFW ever implements it -- but see the note
                // above: this returns -1 on this console.
                {
                    netSocketInfo si;
                    memset(&si, 0, sizeof(si));
                    if (netGetSockInfo(rtpSocket, &si, 1) >= 0) {
                        rxq = si.recv_queue_len;
                    }
                }
                if (rxq > ps3_rxq_peak) ps3_rxq_peak = rxq;

                uint32_t t_hlat, t_rxt, t_jit, t_jitmax;
                ps3video_take_frame_timing(&t_hlat, &t_rxt, &t_jit, &t_jitmax);
                Limelog("[PS3-NET] rx=%u.%03u Mbps pkts=%d buf=%d rxq=%d rxqpk=%d batch=%d/%d "
                        "enobufs=%d recv=%u fecrec=%u fecfail=%u oos=%u "
                        "dec=%u shw=%u skip=%u drop=%u dlat=%ums "
                        "hlat=%u.%ums rxt=%u.%ums jit=%u.%u/%u.%ums qd=%d au=%d\n",
                        kbps / 1000, kbps % 1000,
                        ps3_net_pkts_total,
                        ps3_accepted_rcvbuf,
                        rxq, ps3_rxq_peak,
                        ps3_net_batch_max, VIDEO_BATCH_SIZE,
                        ps3_enobufs_count,
                        st->packetCountVideo,
                        st->packetCountFecRecovered - ps3_prev_fec_rec,
                        st->packetCountFecFailed - ps3_prev_fec_fail,
                        st->packetCountOOS - ps3_prev_oos,
                        PS3_DELTA(ps3video_get_pics_total(), ps3_prev_dec),
                        PS3_DELTA(ps3video_get_shown_total(), ps3_prev_shw),
                        PS3_DELTA(ps3video_get_skipped_total(), ps3_prev_skip),
                        PS3_DELTA(ps3video_get_dropped_frames(), ps3_prev_drop),
                        ps3video_take_decode_latency_ms(),
                        t_hlat / 10, t_hlat % 10, t_rxt / 10, t_rxt % 10,
                        t_jit / 10, t_jit % 10, t_jitmax / 10, t_jitmax % 10,
                        ps3_decode_queue_depth(), ps3video_take_au_peak());
                ps3_prev_dec = ps3video_get_pics_total();
                ps3_prev_shw = ps3video_get_shown_total();
                ps3_prev_skip = ps3video_get_skipped_total();
                ps3_prev_drop = ps3video_get_dropped_frames();

                ps3_video_rx_kbps = (int)kbps;
                ps3_video_rxq_bytes = rxq;

                ps3_prev_fec_rec = st->packetCountFecRecovered;
                ps3_prev_fec_fail = st->packetCountFecFailed;
                ps3_prev_oos = st->packetCountOOS;
                ps3_net_pkts_total = 0;
                ps3_net_bytes_total = 0;
                ps3_net_batch_max = 0;
                ps3_rxq_peak = 0;
                ps3_enobufs_count = 0;
                ps3_net_last_log = now;
            }
        }
#endif
    }

Exit:

    if (buffer != NULL) {
        free(buffer);
    }

    if (encryptedBuffer != NULL) {
        free(encryptedBuffer);
    }
}

void notifyKeyFrameReceived(void) {
    // Remember that we got a full frame successfully
    receivedFullFrame = true;
}

// Decoder thread proc
static void VideoDecoderThreadProc(void* context) {
    while (!PltIsThreadInterrupted(&decoderThread)) {
        VIDEO_FRAME_HANDLE frameHandle;
        PDECODE_UNIT decodeUnit;

        if (!LiWaitForNextVideoFrame(&frameHandle, &decodeUnit)) {
            return;
        }

        LiCompleteVideoFrame(frameHandle, VideoCallbacks.submitDecodeUnit(decodeUnit));
    }
}

// Read the first frame of the video stream
int readFirstFrame(void) {
    // All that matters is that we close this socket.
    // This starts the flow of video on Gen 3 servers.

    closeSocket(firstFrameSocket);
    firstFrameSocket = INVALID_SOCKET;

    return 0;
}

// Terminate the video stream
void stopVideoStream(void) {
    if (!receivedDataFromPeer) {
        Limelog("No video traffic was ever received from the host!\n");
    }

    VideoCallbacks.stop();

    // Wake up client code that may be waiting on the decode unit queue
    stopVideoDepacketizer();

    PltInterruptThread(&udpPingThread);
    PltInterruptThread(&receiveThread);
    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        PltInterruptThread(&decoderThread);
    }

    if (firstFrameSocket != INVALID_SOCKET) {
        shutdownTcpSocket(firstFrameSocket);
    }

    PltJoinThread(&udpPingThread);
    PltJoinThread(&receiveThread);
    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        PltJoinThread(&decoderThread);
    }

    if (firstFrameSocket != INVALID_SOCKET) {
        closeSocket(firstFrameSocket);
        firstFrameSocket = INVALID_SOCKET;
    }
    if (rtpSocket != INVALID_SOCKET) {
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }

    VideoCallbacks.cleanup();
}

// Start the video stream
int startVideoStream(void* rendererContext, int drFlags) {
    int err;

    firstFrameSocket = INVALID_SOCKET;

    // This must be called before the decoder thread starts submitting
    // decode units
    LC_ASSERT(NegotiatedVideoFormat != 0);
    err = VideoCallbacks.setup(NegotiatedVideoFormat, StreamConfig.width,
        StreamConfig.height, StreamConfig.fps, rendererContext, drFlags);
    if (err != 0) {
        return err;
    }

#ifdef __PPU__
    // PS3 libnet has a very small default UDP recv buffer (~8KB) and requires
    // even-numbered sizes.  The old code asked for a flat 64 KB because that was
    // the most PSL1GHT's 128 KB libnet pool could ever back.  The app now asks
    // libnet for a much larger pool at startup (net_init_pool() in src/main.c),
    // so this is sized from the bitrate instead of from that old cap.
    {
        // Size the receive buffer from the BURST, not from a time window.
        //
        // Sunshine sends each frame as a run of packets back-to-back at link
        // rate, so the buffer has to swallow a whole frame before the receive
        // thread gets a look in -- on a gigabit LAN a frame lands in well under
        // a millisecond.  A time window is a video-playback idea; for a game
        // stream a deep buffer is latency, not safety.  What actually has to fit
        // is the largest single burst, which is an IDR frame.
        //
        // Frame size is bitrate/fps, so 30 fps doubles the burst for the same
        // bitrate.  1080p30 at 20 Mbps is a ~101 KB average frame and a ~406 KB
        // IDR -- the old flat 64 KB could not even hold an average frame there,
        // and the previous formula here ignored fps entirely.
        //
        // bindUdpSocket() steps the request down until the kernel accepts one,
        // so asking for more than the libnet pool can back is safe.
        int64_t bytesPerFrame = (int64_t)StreamConfig.bitrate * 125 /
                                (StreamConfig.fps > 0 ? StreamConfig.fps : 60);
        int64_t idrBurst = bytesPerFrame * 4 * 5 / 4; // IDR ~4x average, +25% FEC
        int videoRcvBufSize = (int)(idrBurst * 2);    // two IDRs of headroom

        // Round UP to a power of two.  65536 is the only size this platform is
        // known to accept, and it is a power of two; the one size known to be
        // refused (93440) is not even a multiple of 512.  Asking for an
        // arbitrary byte count risks being refused at every rung of the
        // step-down ladder and landing on the unusable 8 KB default, which is
        // worse than not trying at all.
        {
            extern volatile int ps3_video_rcvbuf_kb;
            if (ps3_video_rcvbuf_kb > 0) {
                // Explicit override from /dev_hdd0/tmp/moonlight_rcvbuf.txt,
                // honoured exactly.  Note the Jellyfin client measured 512 KB
                // BEATING Movian's 128 KB on this console, so the default
                // (burst-derived, larger) is the one with evidence behind it.
                videoRcvBufSize = ps3_video_rcvbuf_kb * 1024;
            }
            else {
                int pow2 = 64 * 1024; // never ask less than the known-good size
                while (pow2 < videoRcvBufSize && pow2 < 1024 * 1024) {
                    pow2 *= 2;
                }
                videoRcvBufSize = pow2; // 64K .. 1M
            }
        }

        Limelog("[PS3-NET] requesting video rcvbuf=%d bytes for %d kbps @%d fps "
                "(frame ~%d KB, IDR ~%d KB; halving ladder down to 65536)\n",
                videoRcvBufSize, StreamConfig.bitrate, StreamConfig.fps,
                (int)(bytesPerFrame * 5 / 4 / 1024), (int)(idrBurst / 1024));

        rtpSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen,
                                  videoRcvBufSize,
                                  SOCK_QOS_TYPE_VIDEO);
    }
#else
    rtpSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen,
                              RTP_RECV_PACKETS_BUFFERED * (StreamConfig.packetSize + MAX_RTP_HEADER_SIZE),
                              SOCK_QOS_TYPE_VIDEO);
#endif
    if (rtpSocket == INVALID_SOCKET) {
        VideoCallbacks.cleanup();
        return LastSocketError();
    }

    VideoCallbacks.start();

    err = PltCreateThread("VideoRecv", VideoReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        VideoCallbacks.stop();
        closeSocket(rtpSocket);
        VideoCallbacks.cleanup();
        return err;
    }

    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        err = PltCreateThread("VideoDec", VideoDecoderThreadProc, NULL, &decoderThread);
        if (err != 0) {
            VideoCallbacks.stop();
            PltInterruptThread(&receiveThread);
            PltJoinThread(&receiveThread);
            closeSocket(rtpSocket);
            VideoCallbacks.cleanup();
            return err;
        }
    }

    if (AppVersionQuad[0] == 3) {
        // Connect this socket to open port 47998 for our ping thread
        firstFrameSocket = connectTcpSocket(&RemoteAddr, AddrLen,
                                            FIRST_FRAME_PORT, FIRST_FRAME_TIMEOUT_SEC);
        if (firstFrameSocket == INVALID_SOCKET) {
            VideoCallbacks.stop();
            stopVideoDepacketizer();
            PltInterruptThread(&receiveThread);
            if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
                PltInterruptThread(&decoderThread);
            }
            PltJoinThread(&receiveThread);
            if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
                PltJoinThread(&decoderThread);
            }
            closeSocket(rtpSocket);
            VideoCallbacks.cleanup();
            return LastSocketError();
        }
    }

    // Start pinging before reading the first frame so GFE knows where
    // to send UDP data
    err = PltCreateThread("VideoPing", VideoPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        VideoCallbacks.stop();
        stopVideoDepacketizer();
        PltInterruptThread(&receiveThread);
        if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
            PltInterruptThread(&decoderThread);
        }
        PltJoinThread(&receiveThread);
        if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
            PltJoinThread(&decoderThread);
        }
        closeSocket(rtpSocket);
        if (firstFrameSocket != INVALID_SOCKET) {
            closeSocket(firstFrameSocket);
            firstFrameSocket = INVALID_SOCKET;
        }
        VideoCallbacks.cleanup();
        return err;
    }

    if (AppVersionQuad[0] == 3) {
        // Read the first frame to start the flow of video
        err = readFirstFrame();
        if (err != 0) {
            stopVideoStream();
            return err;
        }
    }

    return 0;
}

const RTP_VIDEO_STATS* LiGetRTPVideoStats(void) {
    return &rtpQueue.stats;
}
