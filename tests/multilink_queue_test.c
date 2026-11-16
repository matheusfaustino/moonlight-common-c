// Multilink counterfactual counters and receive grace in RtpVideoQueue.c, driven with
// scripted per-path delivery (FEC 0%, so a block needs all of its data shards).
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

// Shard of a frame with blocks of DATA shards each, at an explicit sequence number.
static int feedb(uint16_t seq, uint32_t frame, int block, int lastBlock, int shard, int path) {
    int len = sizeof(RTP_PACKET) + 4 + sizeof(NV_VIDEO_PACKET) + 16;
    char* buf = calloc(1, len + sizeof(RTPV_QUEUE_ENTRY));
    PRTP_PACKET p = (PRTP_PACKET)buf;
    p->header = FLAG_EXTENSION;
    p->sequenceNumber = seq;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(buf + sizeof(RTP_PACKET) + 4);
    nv->frameIndex = frame;
    nv->flags = FLAG_CONTAINS_PIC_DATA | (shard == 0 && block == 0 ? FLAG_SOF : 0) |
                (shard == DATA - 1 && block == lastBlock ? FLAG_EOF : 0);
    nv->fecInfo = (DATA << 22) | (shard << 12);
    nv->multiFecBlocks = (uint8_t)(((lastBlock << 2) | block) << 4);
    int r = RtpvAddPacket(&q, p, len, (PRTPV_QUEUE_ENTRY)(buf + len), path);
    if (r != RTPF_RET_QUEUED) free(buf);
    return r;
}

// Frame 1 has three blocks (seqs 0-3, 4-7, 8-11), like the startup IDR. The primary
// loses seq 6 and starts block 3 (seq 8) before the secondary's copy of seq 6 arrives.
// Returns shards delivered.
static int raceSecondaryLateCopy(bool multilink) {
    RtpvInitializeQueue(&q);
    q.multilink = multilink;
    delivered = 0;
    feedb(0, 1, 0, 2, 0, 1);
    for (int i = 0; i < DATA; i++) feedb((uint16_t)i, 1, 0, 2, i, 0);
    feedb(4, 1, 1, 2, 0, 0); feedb(5, 1, 1, 2, 1, 0); feedb(7, 1, 1, 2, 3, 0);
    feedb(8, 1, 2, 2, 0, 0);                                           // primary starts block 3
    for (int i = 1; i < 3 * DATA; i++) feedb((uint16_t)i, 1, i / DATA, 2, i % DATA, 1);
    for (int i = 1; i < DATA; i++) feedb((uint16_t)(8 + i), 1, 2, 2, i, 0);
    return delivered;
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

    // Receive grace. Without it the late copy is useless and frame 1 is lost.
    CHECK(raceSecondaryLateCopy(false) == 0);
    RtpvCleanupQueue(&q);
    CHECK(raceSecondaryLateCopy(true) == 3 * DATA);
    CHECK(q.graceHeldPackets == 1 && q.graceSavedBlocks == 1 && q.graceGaveUpBlocks == 0);
    CHECK(q.stats.packetCountFecFailed == 0 && q.currentFrameNumber == 2);

    // Seq 14 lost on both paths: given up once both paths are past it (the secondary by
    // more than ML_REORDER_SLACK), not after the grace.
    feedb(12, 3, 0, 0, 0, 0); feedb(13, 3, 0, 0, 1, 0); feedb(15, 3, 0, 0, 3, 0);
    feedb(12, 3, 0, 0, 0, 1); feedb(13, 3, 0, 0, 1, 1); feedb(15, 3, 0, 0, 3, 1);
    feedb(16, 4, 0, 0, 0, 0);
    CHECK(q.mlHeld.count == 1 && q.currentFrameNumber == 3);
    feedb(16, 4, 0, 0, 0, 1);
    CHECK(q.mlHeld.count == 2 && q.graceGaveUpBlocks == 0);            // within the slack
    q.mlSecondaryHigh = 15 + ML_REORDER_SLACK + 1;                      // secondary delivered further on
    feedb(17, 4, 0, 0, 1, 1);
    CHECK(q.mlHeld.count == 0 && q.graceGaveUpBlocks == 1 && q.stats.packetCountFecFailed == 1);
    CHECK(q.currentFrameNumber == 4 && q.pendingFecBlockList.count == 2);
    for (int i = 1; i < DATA; i++) feedb((uint16_t)(16 + i), 4, 0, 0, i, 0);

    // Grace expiry: the secondary never passes frame 5, so it is given up after ML_GRACE_US.
    feedb(20, 5, 0, 0, 0, 0); feedb(21, 5, 0, 0, 1, 0); feedb(23, 5, 0, 0, 3, 0);
    feedb(24, 6, 0, 0, 0, 0);
    CHECK(q.mlHeld.count == 1);
    q.mlHoldStartUs -= ML_GRACE_US;
    feedb(25, 6, 0, 0, 1, 0);
    CHECK(q.mlHeld.count == 0 && q.graceGaveUpBlocks == 2 && q.currentFrameNumber == 6);
    feedb(26, 6, 0, 0, 2, 0); feedb(27, 6, 0, 0, 3, 0);
    CHECK(q.currentFrameNumber == 7);

    // Secondary ahead (parity mode case): its frame-8 shard must not abandon frame 7.
    int before = delivered;
    feedb(28, 7, 0, 0, 0, 0); feedb(29, 7, 0, 0, 1, 0);
    feedb(32, 8, 0, 0, 0, 1);
    feedb(30, 7, 0, 0, 2, 0); feedb(31, 7, 0, 0, 3, 0);
    for (int i = 1; i < DATA; i++) feedb((uint16_t)(32 + i), 8, 0, 0, i, 0);
    CHECK(delivered - before == 2 * DATA && q.currentFrameNumber == 9 && q.graceGaveUpBlocks == 2);

    // OOS stats are live
    CHECK(q.stats.packetCountOOS > 0);
    RtpvCleanupQueue(&q);

    // Jittered secondary: its frame-2 shard overtakes its copy of the shard the primary
    // lost. Being only just past the block must not give it up.
    RtpvInitializeQueue(&q);
    q.multilink = true;
    delivered = 0;
    feedb(0, 1, 0, 0, 0, 0); feedb(1, 1, 0, 0, 1, 0); feedb(3, 1, 0, 0, 3, 0);
    feedb(0, 1, 0, 0, 0, 1); feedb(1, 1, 0, 0, 1, 1);
    feedb(4, 2, 0, 0, 0, 0);
    feedb(3, 1, 0, 0, 3, 1); feedb(4, 2, 0, 0, 0, 1);                  // secondary one past
    CHECK(q.mlHeld.count == 2 && q.graceGaveUpBlocks == 0);
    feedb(2, 1, 0, 0, 2, 1);                                           // the reordered copy
    CHECK(q.mlHeld.count == 0 && q.graceSavedBlocks == 1 && q.currentFrameNumber == 2);
    for (int i = 1; i < DATA; i++) feedb((uint16_t)(4 + i), 2, 0, 0, i, 0);
    CHECK(delivered == 2 * DATA && q.stats.packetCountFecFailed == 0);
    CHECK(q.graceHeldPackets == 2 && q.mlReplayRejected[1] == 1);      // late secondary dup of seq 4

    // Silent secondary (cleared by the receive side): a primary loss is given up at once.
    q.mlHaveSecondary = false;
    q.mlSecondaryHigh = 40000;                                         // stale, wrapped
    feedb(8, 3, 0, 0, 0, 0); feedb(9, 3, 0, 0, 1, 0); feedb(11, 3, 0, 0, 3, 0);
    feedb(12, 4, 0, 0, 0, 0);
    CHECK(q.mlHeld.count == 0 && q.stats.packetCountFecFailed == 1 && q.currentFrameNumber == 4);
    feedb(13, 4, 0, 0, 1, 1);                                          // secondary resumes
    CHECK(q.mlHaveSecondary && q.mlSecondaryHigh == 13);
    RtpvCleanupQueue(&q);

    // Replay counterfactual: the secondary is ahead by two frames and late primary copies of
    // frame 2 are held behind frame 3's first shard. Frame 2 was primary-complete, so it must
    // not be counted as primary-alone-unrecoverable.
    RtpvInitializeQueue(&q);
    q.multilink = true;
    feedb(0, 1, 0, 0, 0, 0); feedb(1, 1, 0, 0, 1, 0); feedb(0, 1, 0, 0, 0, 1);
    for (int i = 0; i < DATA; i++) feedb((uint16_t)(4 + i), 2, 0, 0, i, 1);
    feedb(8, 3, 0, 0, 0, 1);
    for (int i = 0; i < DATA; i++) feedb((uint16_t)(4 + i), 2, 0, 0, i, 0);
    feedb(8, 3, 0, 0, 0, 0);
    CHECK(q.mlHeld.count == 10);
    feedb(2, 1, 0, 0, 2, 0); feedb(3, 1, 0, 0, 3, 0);                  // primary finishes frame 1
    CHECK(q.mlHeld.count == 0 && q.currentFrameNumber == 3);
    CHECK(CNT(0, 0, 0, 0));
    RtpvCleanupQueue(&q);

    // Pre-decrypt dedup. Only recorded (authenticated) IVs are skipped; an unseen or
    // forged IV always goes to decrypt; a slot reused by a newer IV forgets the old one.
    RtpvInitializeQueue(&q);
    CHECK(!RtpvMultilinkSkipDuplicate(&q, 0, 1));                       // empty table, IV 0
    RtpvMultilinkRecordDecrypted(&q, 7, 3);
    CHECK(RtpvMultilinkSkipDuplicate(&q, 7, 1) && q.mlHaveSecondary && q.mlSecondaryHigh == 3);
    CHECK(!RtpvMultilinkSkipDuplicate(&q, 8, 0) && !RtpvMultilinkSkipDuplicate(&q, 7 + ML_SEEN_SLOTS, 0));
    RtpvMultilinkRecordDecrypted(&q, 7 + ML_SEEN_SLOTS, 9);
    CHECK(!RtpvMultilinkSkipDuplicate(&q, 7, 0));
    RtpvCleanupQueue(&q);

    // The secondary decrypts frame 1 first; the primary's copies are skipped before decrypt
    // but still observed, so frame 1 is primary-complete, not a rescue.
    RtpvInitializeQueue(&q);
    q.multilink = true;
    for (int i = 0; i < DATA; i++) { feedb((uint16_t)i, 1, 0, 0, i, 1); RtpvMultilinkRecordDecrypted(&q, 100 + i, (uint16_t)i); }
    for (int i = 0; i < DATA; i++) CHECK(RtpvMultilinkSkipDuplicate(&q, 100 + i, 0));
    for (int i = 0; i < DATA; i++) feedb((uint16_t)(DATA + i), 2, 0, 0, i, 0);
    CHECK(q.currentFrameNumber == 3 && CNT(0, 0, 0, 0));
    RtpvCleanupQueue(&q);

    // Seq 2 lost on both paths. The primary's frames 2-4 are decrypted (recorded) and held;
    // the secondary's copies of them are all skipped duplicates. The skips must still move
    // the secondary past frame 1 and release the held frames, with no further packet.
    RtpvInitializeQueue(&q);
    q.multilink = true;
    feedb(0, 1, 0, 0, 0, 0); feedb(1, 1, 0, 0, 1, 0); feedb(3, 1, 0, 0, 3, 0);
    feedb(0, 1, 0, 0, 0, 1);
    for (int f = 2; f <= 4; f++) for (int i = 0; i < DATA; i++) {
        uint16_t seq = (uint16_t)((f - 1) * DATA + i);
        feedb(seq, (uint32_t)f, 0, 0, i, 0);
        RtpvMultilinkRecordDecrypted(&q, 1000 + seq, seq);
    }
    CHECK(q.mlHeld.count == 3 * DATA);
    for (int seq = DATA; seq < 4 * DATA; seq++) CHECK(RtpvMultilinkSkipDuplicate(&q, 1000 + seq, 1));
    CHECK(q.mlHeld.count == 0 && q.graceGaveUpBlocks == 1 && q.currentFrameNumber == 5);
    RtpvCleanupQueue(&q);
    printf("ok\n");
    return 0;
}
