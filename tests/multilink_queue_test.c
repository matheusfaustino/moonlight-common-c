// Multilink counterfactual counters in RtpVideoQueue.c, driven with scripted
// per-path delivery (FEC 0%, so a block needs all of its data shards).
//
// Build and run from this directory:
//   cc -std=c11 -I../src -I../nanors -I../nanors/deps/obl -I../enet/include
//      multilink_queue_test.c ../src/RtpVideoQueue.c ../nanors/rs.c
//      ../nanors/deps/obl/oblas_common.c ../nanors/deps/obl/oblas_lite.c -o /tmp/mlq && /tmp/mlq

#define _POSIX_C_SOURCE 200809L
#include "Limelight-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int AppVersionQuad[4] = {7, 1, 431, -1};
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
bool isReferenceFrameInvalidationEnabled(void) { return false; }
int delivered;

void queueRtpPacket(PRTPV_QUEUE_ENTRY e) { delivered++; free(e->packet); }
void notifyFrameLost(unsigned int f, bool s) { (void)f; (void)s; }
void connectionSawFrame(uint32_t f) { (void)f; }
void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS s) { (void)s; }
void LiRequestIdrFrame(void) {}
uint64_t PltGetMicroseconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000ULL + t.tv_nsec / 1000; }

#define DATA 4
static RTP_VIDEO_QUEUE q;

static int feed(uint32_t frame, uint32_t shard, int path) {
    int len = sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET) + 16;
    char* buf = calloc(1, len + sizeof(RTPV_QUEUE_ENTRY));
    PRTP_PACKET p = (PRTP_PACKET)buf;
    p->header = FLAG_EXTENSION;
    p->sequenceNumber = (uint16_t)((frame - 1) * DATA + shard);
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(buf + sizeof(RTP_PACKET) + 4);
    nv->frameIndex = frame;
    nv->flags = FLAG_CONTAINS_PIC_DATA | (shard == 0 ? FLAG_SOF : 0) | (shard == DATA - 1 ? FLAG_EOF : 0);
    nv->fecInfo = (DATA << 22) | (shard << 12);
    int r = RtpvAddPacket(&q, p, len, (PRTPV_QUEUE_ENTRY)(buf + len), path);
    if (r != RTPF_RET_QUEUED) free(buf);
    return r;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

#define CNT(u, r, f, n) (q.blocksPrimaryAloneUnrecoverable == (u) && q.blocksRescuedBySecondary == (r) && \
                         q.blocksFailedMerged == (f) && q.blocksUnfinalized == (n))

int main(void) {
    StreamConfig.packetSize = 16;
    RtpvInitializeQueue(&q);

    for (int i = 0; i < DATA; i++) feed(1, i, 0);                       // primary alone is enough
    CHECK(CNT(0, 0, 0, 0));

    // Frame 2: secondary fills the hole. Frame 3 then completes on the secondary alone;
    // the primary has not advanced, so frame 2 must not be decided yet.
    feed(2, 0, 0); feed(2, 1, 0); feed(2, 3, 0); feed(2, 2, 1);
    for (int i = 0; i < DATA; i++) feed(3, i, 1);
    CHECK(CNT(0, 0, 0, 0));
    feed(3, 0, 0);                                                      // primary moves past frame 2
    CHECK(CNT(1, 1, 0, 0));
    for (int i = 1; i < DATA; i++) CHECK(feed(3, i, 0) == RTPF_RET_REJECTED);
    CHECK(CNT(1, 1, 0, 0));                                             // late primary copies: frame 3 was primary-complete

    // Frame 4: secondary wins the last shard; the late primary copy is observed (frame 4 is
    // already complete) and passes the stale-frame filter, so it is not counted as a rescue.
    feed(4, 0, 0); feed(4, 1, 0); feed(4, 2, 0); feed(4, 3, 1);
    CHECK(!RtpvIsStaleFrame(&q, 4, 0, true));
    CHECK(feed(4, 3, 0) == RTPF_RET_REJECTED);
    feed(5, 0, 0); feed(5, 1, 0);
    CHECK(CNT(1, 1, 0, 0));

    // Stale-frame filter: secondary and non-multilink drop it, old primary frames drop it
    CHECK(RtpvIsStaleFrame(&q, 4, 1, true));
    CHECK(RtpvIsStaleFrame(&q, 4, 0, false));
    CHECK(!RtpvIsStaleFrame(&q, 5, 1, true));
    CHECK(!RtpvIsStaleFrame(&q, 0, 1, true));
    CHECK(RtpvIsStaleFrame(&q, 4, 0, true) == false && q.currentFrameNumber - 4 <= ML_RING_BLOCKS);

    // Frame 5: primary only delivers shards 0,1; secondary completes it. Frame 6 loses
    // shard 3 on both paths and is abandoned by frame 7.
    feed(5, 2, 1); feed(5, 3, 1);
    feed(6, 0, 0); feed(6, 1, 0); feed(6, 2, 0);
    feed(7, 0, 1);
    CHECK(CNT(2, 2, 0, 0));                                             // frame 5 decided; primary still on frame 6
    feed(7, 0, 0);
    CHECK(CNT(3, 2, 1, 0));

    // Frames 7..14 on the secondary alone; the primary stays silent. Frame 7 is evicted
    // from the ring after 8 newer blocks and is unfinalized, not rescued.
    for (int i = 1; i < DATA; i++) feed(7, i, 1);
    for (uint32_t f = 8; f <= 14; f++) for (int i = 0; i < DATA; i++) feed(f, i, 1);
    CHECK(CNT(3, 2, 1, 0));
    feed(15, 0, 1);
    CHECK(CNT(3, 2, 1, 1));
    RtpvMultilinkFlush(&q);
    CHECK(CNT(3, 2, 1, 9));

    RtpvCleanupQueue(&q);
    printf("ok\n");
    return 0;
}
