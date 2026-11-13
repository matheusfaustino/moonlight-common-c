#pragma once

#include "Video.h"

#include <stdatomic.h>

typedef struct _RTPV_QUEUE_ENTRY {
    struct _RTPV_QUEUE_ENTRY* next;
    struct _RTPV_QUEUE_ENTRY* prev;
    PRTP_PACKET packet;
    uint64_t receiveTimeUs;
    uint64_t presentationTimeUs;
    uint32_t rtpTimestamp;
    int length;
    bool isParity;
    uint8_t path;   // multilink: path a held packet arrived on
} RTPV_QUEUE_ENTRY, *PRTPV_QUEUE_ENTRY;

typedef struct _RTPV_QUEUE_LIST {
    PRTPV_QUEUE_ENTRY head;
    PRTPV_QUEUE_ENTRY tail;
    uint32_t count;
} RTPV_QUEUE_LIST, *PRTPV_QUEUE_LIST;

// A block is evaluated only after the primary has moved past it, or is unfinalized
// once ML_RING_BLOCKS newer blocks have started.
#define ML_RING_BLOCKS 8

// Multilink receive grace: while the current FEC block is incomplete, packets of later
// blocks are held instead of abandoning it, because the other path's copy of a missing
// shard may still be in flight. The block is given up once both paths have delivered a
// sequence number past it, ML_GRACE_US has elapsed since the first held packet, or
// ML_HOLD_MAX packets are held. 20 ms covers the measured Wi-Fi p99 lag (16 ms);
// ML_HOLD_MAX matches the socket buffer (RTP_RECV_PACKETS_BUFFERED).
#define ML_GRACE_US 20000
#define ML_HOLD_MAX 2048

typedef struct _ML_BLOCK {
    bool used;
    bool resolved;      // merged queue completed it or moved on to a newer block
    bool completed;     // merged queue completed it
    uint16_t lowest;
    uint16_t highest;
    uint16_t dataShards;
    uint16_t seenCount;
    uint8_t seen[32];
} ML_BLOCK;

typedef struct _RTP_VIDEO_QUEUE_STATS {
    atomic_uint packetCountVideo;
    atomic_uint packetCountFec;
    atomic_uint packetCountFecRecovered;
    atomic_uint packetCountFecFailed;
    atomic_uint packetCountOOS;
    atomic_uint packetCountInvalid;
    atomic_uint packetCountFecInvalid;
} RTP_VIDEO_QUEUE_STATS, *PRTP_VIDEO_QUEUE_STATS;

typedef struct _RTP_VIDEO_QUEUE {
    RTPV_QUEUE_LIST pendingFecBlockList;
    RTPV_QUEUE_LIST completedFecBlockList;

    uint64_t bufferFirstRecvTimeUs;
    uint32_t bufferLowestSequenceNumber;
    uint32_t bufferHighestSequenceNumber;
    uint32_t bufferFirstParitySequenceNumber;
    uint32_t bufferDataPackets;
    uint32_t bufferParityPackets;
    uint32_t receivedDataPackets;
    uint32_t receivedParityPackets;
    uint32_t receivedHighestSequenceNumber;
    uint32_t fecPercentage;
    uint32_t nextContiguousSequenceNumber;
    uint32_t missingPackets; // # of holes behind receivedHighestSequenceNumber
    bool useFastQueuePath;
    bool reportedLostFrame;

    uint32_t currentFrameNumber;

    bool multiFecCapable;
    uint8_t multiFecCurrentBlockNumber;
    uint8_t multiFecLastBlockNumber;

    uint64_t lastOosFramePresentationTimestamp;
    bool receivedOosData;

    // Multilink counterfactual: a small ring of recent FEC blocks records which shards
    // the primary path delivered, independent of which copy the merged queue used.
    // Only touched from the receive thread.
    ML_BLOCK mlRing[ML_RING_BLOCKS];
    uint32_t mlNext;              // blocks started so far; slot = mlNext % ML_RING_BLOCKS
    bool mlHavePrimary;
    uint16_t mlPrimaryHigh;       // highest sequence number the primary delivered
    uint32_t blocksPrimaryAloneUnrecoverable;
    uint32_t blocksRescuedBySecondary;
    uint32_t blocksFailedMerged;
    uint32_t blocksUnfinalized;   // primary never moved past the block within the ring window

    bool multilink;               // grace enabled; set by the receive side only
    bool mlHaveSecondary;
    uint16_t mlSecondaryHigh;     // highest sequence number the secondary delivered
    RTPV_QUEUE_LIST mlHeld;       // later-block packets held in arrival order
    uint64_t mlHoldStartUs;
    uint32_t graceHeldPackets;    // packets that were held
    uint32_t graceSavedBlocks;    // blocks completed after a later-block packet was held
    uint32_t graceGaveUpBlocks;   // blocks abandoned after holding (grace, both paths past, cap)

    RTP_VIDEO_QUEUE_STATS stats; // the above values are short-lived, this tracks stats for the life of the queue
} RTP_VIDEO_QUEUE, *PRTP_VIDEO_QUEUE;

#define RTPF_RET_QUEUED    0
#define RTPF_RET_REJECTED  1

void RtpvInitializeQueue(PRTP_VIDEO_QUEUE queue);
void RtpvCleanupQueue(PRTP_VIDEO_QUEUE queue);
// path: 0 = primary socket, 1 = multilink secondary
int RtpvAddPacket(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, PRTPV_QUEUE_ENTRY packetEntry, int path);
uint32_t RtpvGetCurrentFrameNumber(PRTP_VIDEO_QUEUE queue);
void RtpvSubmitQueuedPackets(PRTP_VIDEO_QUEUE queue);
// Finalizes (or marks unfinalized) every tracked block. Call at stream end.
void RtpvMultilinkFlush(PRTP_VIDEO_QUEUE queue);
// Pre-decrypt stale check: true if a packet of this frame can be dropped. Primary
// copies stay eligible for ML_RING_BLOCKS frames so late ones are still observed.
bool RtpvIsStaleFrame(PRTP_VIDEO_QUEUE queue, uint32_t frameNumber, int path, bool multilink);
void RtpvGetStats(PRTP_VIDEO_QUEUE queue, PRTP_VIDEO_STATS stats);
