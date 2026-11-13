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
} RTPV_QUEUE_ENTRY, *PRTPV_QUEUE_ENTRY;

typedef struct _RTPV_QUEUE_LIST {
    PRTPV_QUEUE_ENTRY head;
    PRTPV_QUEUE_ENTRY tail;
    uint32_t count;
} RTPV_QUEUE_LIST, *PRTPV_QUEUE_LIST;

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

    // Multilink counterfactual: which shards of the current FEC block the primary
    // path delivered (even if the secondary copy won the race), plus lifetime
    // per-block counters. Only touched from the receive thread.
    uint8_t primarySeen[32];
    uint32_t primarySeenCount;
    bool multilinkBlockOpen;      // a block is tracked and not yet counted
    bool multilinkBlockCompleted; // the open block was completed by the merged queue
    uint32_t blocksPrimaryAloneUnrecoverable;
    uint32_t blocksRescuedBySecondary;
    uint32_t blocksFailedMerged;

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
// Counts the open block if the merged queue completed it. Call at stream end.
void RtpvMultilinkFlush(PRTP_VIDEO_QUEUE queue);
void RtpvGetStats(PRTP_VIDEO_QUEUE queue, PRTP_VIDEO_STATS stats);
