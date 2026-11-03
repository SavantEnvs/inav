// Fuzzes INAV's MSP-over-telemetry framing (SmartPort / CRSF): request reassembly in handleMspFrame()
// and reply chunking in sendMspReply(). Input = a sequence of ops:
//   op & 0x80 : sendMspReply with a 6 (SmartPort) or 58 (CRSF) byte frame; op & 1 picks the size
//   otherwise : op % 59 = payloadLength, followed by that many frame bytes (zero padded if truncated)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#include "common/streambuf.h"
#include "common/utils.h"

#include "fc/fc_msp.h"
#include "msp/msp.h"

#include "telemetry/msp_shared.h"

#define MAX_OPS 64

mspResult_e mspFcProcessCommand(mspPacket_t *cmd, mspPacket_t *reply, mspPostProcessFnPtr *mspPostProcessFn)
{
    UNUSED(mspPostProcessFn);

    uint8_t sum = 0;
    while (sbufBytesRemaining(&cmd->buf) > 0) {
        sum += sbufReadU8(&cmd->buf);
    }

    int length = (int)((cmd->cmd + sum) % 160);
    if (length > sbufBytesRemaining(&reply->buf)) {
        length = sbufBytesRemaining(&reply->buf);
    }
    reply->cmd = cmd->cmd;
    for (int i = 0; i < length; i++) {
        sbufWriteU8(&reply->buf, (uint8_t)(sum + i));
    }

    switch (cmd->cmd & 3) {
        case 0: return MSP_RESULT_ERROR;
        case 1: return MSP_RESULT_NO_REPLY;
        default: return MSP_RESULT_ACK;
    }
}

static void sink(uint8_t *payload, const uint8_t payloadSize)
{
    volatile uint8_t acc = 0;
    for (int i = 0; i < payloadSize; i++) {
        acc ^= payload[i];
    }
    (void)acc;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    initSharedMsp();

    int ops = 0;
    while (size > 0 && ops++ < MAX_OPS) {
        const uint8_t op = *data++;
        size--;

        if (op & 0x80) {
            sendMspReply((op & 1) ? 6 : 58, sink);
            continue;
        }

        const size_t len = op % 59;
        uint8_t *frame = (uint8_t *)calloc(len ? len : 1, 1);
        const size_t n = len < size ? len : size;
        memcpy(frame, data, n);
        data += n;
        size -= n;

        handleMspFrame(frame, (int)len);
        free(frame);
    }
    return 0;
}
