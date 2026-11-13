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

int main(void) {
    StreamConfig.packetSize = 16;
    RtpvInitializeQueue(&q);

    for (int i = 0; i < DATA; i++) feed(1, i, 0);                       // primary alone is enough
    feed(2, 0, 0); feed(2, 1, 0); feed(2, 3, 0); feed(2, 2, 1);         // secondary fills the hole
    feed(3, 0, 0); feed(3, 1, 0); feed(3, 2, 1);                        // shard 3 lost on both
    feed(4, 0, 1);                                                      // next frame abandons frame 3
    CHECK(q.blocksPrimaryAloneUnrecoverable == 2 && q.blocksRescuedBySecondary == 1 && q.blocksFailedMerged == 1);

    // Frame 4: secondary wins shard 0, the primary copy is a rejected duplicate but still counts for the primary
    CHECK(feed(4, 0, 0) == RTPF_RET_REJECTED);
    feed(4, 1, 0); feed(4, 2, 0); feed(4, 3, 0);
    CHECK(q.blocksPrimaryAloneUnrecoverable == 2);
    CHECK(delivered == 3 * DATA);

    // Frame 5: secondary wins the last shard, then the primary copy arrives late: not a rescue
    feed(5, 0, 0); feed(5, 1, 0); feed(5, 2, 0); feed(5, 3, 1);
    CHECK(feed(5, 3, 0) == RTPF_RET_REJECTED);
    RtpvMultilinkFlush(&q);
    CHECK(q.blocksPrimaryAloneUnrecoverable == 2 && q.blocksRescuedBySecondary == 1 && q.blocksFailedMerged == 1);

    // Frame 6: the primary never delivers shard 3, so flush counts a rescue
    feed(6, 0, 0); feed(6, 1, 0); feed(6, 2, 0); feed(6, 3, 1);
    RtpvMultilinkFlush(&q);
    CHECK(q.blocksPrimaryAloneUnrecoverable == 3 && q.blocksRescuedBySecondary == 2 && q.blocksFailedMerged == 1);

    RtpvCleanupQueue(&q);
    printf("ok\n");
    return 0;
}
