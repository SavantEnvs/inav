// Fuzzes INAV's DroneCAN application layer (dronecan.c + libcanard + DSDL decoders + node table +
// DNA server + async service slot), driven the way the firmware drives it: CAN frames arrive in the
// RX FIFO and dronecanUpdate() runs.
//
// Built twice from this file:
//   fuzz_dronecan_can : ops are raw CAN frames and ticks
//   fuzz_dronecan_msp : additionally the MSP2_INAV_DRONECAN_* handlers (request parser, result and
//                       node-table serialisers) that sit on top of the same state
//
// Input: byte 0 primes the async slot (bit0 = request pending, bits1-2 = service, bits3-7 = peer node),
// byte 1 = in-flight transfer id, then ops (op & 3):
//   0,1 : CAN frame: u32 id, u8 dlc (%9), dlc data bytes
//   2   : (msp build) u8 len, len bytes of MSP payload handed to mspHandleDronecanAsyncRequest /
//         mspSerialize*; (can build) CAN frame
//   3   : tick: advance (op >> 2) * 40 ms, deliver the queued frames, run dronecanUpdate()
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#include "uavcan.protocol.NodeStatus.h"
#include "uavcan.protocol.GetNodeInfo.h"
#include "uavcan.protocol.param.GetSet_res.h"
#include "uavcan.protocol.param.GetSet.h"
#include "uavcan.protocol.param.ExecuteOpcode_res.h"
#include "uavcan.protocol.RestartNode_res.h"

#include "drivers/dronecan/libcanard/canard.h"
#include "drivers/dronecan/libcanard/canard_stm32_driver.h"

#include "io/gps.h"
#include "sensors/battery_sensor_dronecan.h"
#include "fc/runtime_config.h"
#include "sensors/diagnostics.h"
#include "build/version.h"
#include "common/log.h"
#include "common/streambuf.h"

#include "drivers/dronecan/dronecan.h"
#include "drivers/dronecan/dronecan_dna_server.h"
#include "fc/fc_msp_dronecan.h"

extern uint8_t activeNodeCount;
extern dronecanNodeInfo_t nodeTable[];
extern CanardInstance canard;

bool shouldAcceptTransfer(const CanardInstance *ins, uint64_t *out_data_type_signature,
                          uint16_t data_type_id, CanardTransferType transfer_type, uint8_t source_node_id);
void onTransferReceived(CanardInstance *ins, CanardRxTransfer *transfer);

// ---- firmware environment stubs (same set the upstream unit test uses) ----

static uint32_t mock_time_ms;
uint32_t millis(void) { return mock_time_ms; }

uint32_t armingFlags;
gpsConfig_t gpsConfig_System;
gpsConfig_t gpsConfig_Copy;

bool isHardwareHealthy(void) { return true; }
void _logf(logTopic_e topic, unsigned level, const char *fmt, ...) { (void)topic; (void)level; (void)fmt; }

void dronecanGPSReceiveGNSSFix(const struct uavcan_equipment_gnss_Fix *p) { (void)p; }
void dronecanGPSReceiveGNSSFix2(const struct uavcan_equipment_gnss_Fix2 *p) { (void)p; }
void dronecanGPSReceiveGNSSAuxiliary(const struct uavcan_equipment_gnss_Auxiliary *p) { (void)p; }
void dronecanBatterySensorReceiveInfo(struct uavcan_equipment_power_BatteryInfo *p) { (void)p; }

int16_t canardSTM32CAN1_Init(uint32_t b) { (void)b; return CANARD_OK; }
uint32_t canardSTM32GetAndClearRxDropCount(void) { return 0; }
int16_t canardSTM32Transmit(const CanardCANFrame *f) { (void)f; return 1; }
void canardSTM32GetProtocolStatus(canardProtocolStatus_t *s) { memset(s, 0, sizeof(*s)); }
void canardSTM32RecoverFromBusOff(void) {}
void canardSTM32GetUniqueID(uint8_t id[16]) { memset(id, 0, 16); }

#define RX_QUEUE_LEN 32
static CanardCANFrame rx_queue[RX_QUEUE_LEN];
static int rx_count;
static int rx_pos;
int32_t canardSTM32GetRxFifoFillLevel(void) { return rx_count - rx_pos; }
int16_t canardSTM32Receive(CanardCANFrame *f)
{
    if (rx_pos >= rx_count) {
        return 0;
    }
    *f = rx_queue[rx_pos++];
    return 1;
}

void saveConfig(void) {}

const char *const shortGitRevision = "00000000";
const char *const compilerVersion = "fuzz";
const char *const targetName = "FUZZ";
const char *const buildDate = "Jan 01 2026";
const char *const buildTime = "00:00:00";

// ---- harness ----

static uint8_t canard_pool[8192];
static timeUs_t now_us;
static bool booted;

static const uint8_t services[4] = {
    DRONECAN_SERVICE_GETNODEINFO, DRONECAN_SERVICE_RESTART_NODE,
    DRONECAN_SERVICE_EXECUTE_OPCODE, DRONECAN_SERVICE_PARAM_GETSET,
};

static void tick(uint32_t delta_ms)
{
    now_us += (timeUs_t)delta_ms * 1000;
    mock_time_ms = (uint32_t)(now_us / 1000);
    dronecanUpdate(now_us);
    rx_count = 0;
    rx_pos = 0;
}

static void reset_state(void)
{
    activeNodeCount = 0;
    memset(nodeTable, 0, sizeof(dronecanNodeInfo_t) * DRONECAN_MAX_NODES);
    memset(&dronecanAsyncSlot, 0, sizeof(dronecanAsyncSlot));
    memset(dnaServerDataMutable(), 0, sizeof(dnaServerData_t));
    rx_count = 0;
    rx_pos = 0;
    canardInit(&canard, canard_pool, sizeof(canard_pool), onTransferReceived, shouldAcceptTransfer, NULL);
    canardSetLocalNodeID(&canard, 1);
    if (!booted) {
        tick(0); // STATE_DRONECAN_INIT -> STATE_DRONECAN_NORMAL
        booted = true;
    }
}

static void queue_frame(const uint8_t *d, size_t avail, size_t *used)
{
    CanardCANFrame f;
    memset(&f, 0, sizeof(f));
    uint32_t id = 0;
    size_t n = avail < 5 ? avail : 5;
    for (size_t i = 0; i < n && i < 4; i++) {
        id |= (uint32_t)d[i] << (8 * i);
    }
    f.id = (id & CANARD_CAN_EXT_ID_MASK) | CANARD_CAN_FRAME_EFF;
    size_t pos = n;
    if (n == 5) {
        f.data_len = d[4] % 9;
        size_t have = avail - 5;
        size_t take = f.data_len < have ? f.data_len : have;
        memcpy(f.data, d + 5, take);
        pos += take;
    }
    *used = pos;
    if (rx_count < RX_QUEUE_LEN) {
        rx_queue[rx_count++] = f;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    reset_state();
    if (size < 2) {
        return -1;
    }

    if (data[0] & 1) {
        dronecanAsyncSlot.state = DRONECAN_ASYNC_PENDING;
        dronecanAsyncSlot.service_id = services[(data[0] >> 1) & 3];
        dronecanAsyncSlot.node_id = (data[0] >> 3) + 1;
        dronecanAsyncSlot.transfer_id = data[1];
        dronecanAsyncSlot.requested_at_ms = mock_time_ms;
    }
    data += 2;
    size -= 2;

    int ops = 0;
    while (size > 0 && ops++ < 256) {
        const uint8_t op = *data++;
        size--;

        switch (op & 3) {
#ifdef FUZZ_MSP
        case 2: {
            if (size == 0) {
                break;
            }
            size_t len = *data++ % 64;
            size--;
            if (len > size) {
                len = size;
            }
            uint8_t *copy = (uint8_t *)malloc(len ? len : 1);
            memcpy(copy, data, len);
            data += len;
            size -= len;

            uint8_t out[1024];
            sbuf_t src, dst;
            sbufInit(&src, copy, copy + len);
            sbufInit(&dst, out, out + sizeof(out));
            mspResult_e ret = MSP_RESULT_ACK;
            mspHandleDronecanAsyncRequest(&src, &dst, &ret);

            sbufInit(&dst, out, out + sizeof(out));
            mspSerializeDronecanAsyncResult(&dst);
            sbufInit(&dst, out, out + sizeof(out));
            mspSerializeDronecanNodes(&dst);
            free(copy);
            break;
        }
#else
        case 2:
#endif
        case 0:
        case 1: {
            size_t used = 0;
            queue_frame(data, size, &used);
            data += used;
            size -= used;
            break;
        }
        default:
            tick((uint32_t)(op >> 2) * 40);
            break;
        }
    }
    tick(0);
    return 0;
}
