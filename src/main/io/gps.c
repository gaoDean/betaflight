/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "platform.h"

#ifdef USE_GPS

#include "build/build_config.h"
#include "build/debug.h"

#include "common/axis.h"
#include "common/gps_conversion.h"
#include "common/maths.h"
#include "common/utils.h"

#include "config/feature.h"

#include "drivers/light_led.h"
#include "drivers/time.h"

#include "io/beeper.h"
#ifdef USE_DASHBOARD
#include "io/dashboard.h"
#endif

#include "io/gps.h"
#include "io/gps_virtual.h"

#include "io/serial.h"

#include "config/config.h"

#include "fc/gps_lap_timer.h"
#include "fc/runtime_config.h"

#include "flight/gps_rescue.h"
#include "flight/imu.h"
#include "flight/pid.h"

#include "scheduler/scheduler.h"

#include "sensors/sensors.h"
#include "common/typeconversion.h"

// **********************
// GPS
// **********************
gpsLocation_t GPS_home_llh;
uint16_t GPS_distanceToHome;        // distance to home point in meters
uint32_t GPS_distanceToHomeCm;
int16_t GPS_directionToHome;        // direction to home or hol point in degrees * 10
uint32_t GPS_distanceFlownInCm;     // distance flown since armed in centimeters

#define GPS_DISTANCE_FLOWN_MIN_SPEED_THRESHOLD_CM_S 15 // 0.54 km/h 0.335 mph

gpsSolutionData_t gpsSol;
uint8_t GPS_update = 0;             // toogle to distinct a GPS position update (directly or via MSP)

uint8_t GPS_numCh;                              // Details on numCh/svinfo in gps.h
GPS_svinfo_t GPS_svinfo[GPS_SV_MAXSATS_M8N];

// GPS LOST_COMMUNICATION timeout in ms (max time between received nav solutions)
#define GPS_TIMEOUT_MS 2500
// Timeout for waiting for an ACK or NAK response to a configuration command
#define UBLOX_ACK_TIMEOUT_MS 150
// Time allowed for module to respond to baud rate change during initial configuration
#define GPS_CONFIG_BAUD_CHANGE_INTERVAL 330  // Time to wait, in ms, between 'test this baud rate' messages
#define GPS_CONFIG_CHANGE_INTERVAL 110       // Time to wait, in ms, between CONFIG steps
#define GPS_BAUDRATE_TEST_COUNT 3      // Number of times to repeat the test message when setting baudrate
#define GPS_RECV_TIME_MAX 25           // Max permitted time, in us, for the Receive Data process
// Decay the estimated max task duration by 1/(1 << GPS_TASK_DECAY_SHIFT) on every invocation
#define GPS_TASK_DECAY_SHIFT 9         // Smoothing factor for GPS task re-scheduler

static serialPort_t *gpsPort;
static float gpsDataIntervalSeconds = 0.1f;
static float gpsDataFrequencyHz = 10.0f;

static uint16_t currentGpsStamp = 0; // logical timer for received position update

typedef struct gpsInitData_s {
    uint8_t baudrateIndex; // see baudRate_e
    const char *ubx;
} gpsInitData_t;

// UBX will cycle through these until valid data is received
static const gpsInitData_t gpsInitData[] = {
    { BAUD_230400, "$PUBX,41,1,0003,0001,230400,0*1C\r\n" },
    { BAUD_115200, "$PUBX,41,1,0003,0001,115200,0*1E\r\n" },
    { BAUD_57600,  "$PUBX,41,1,0003,0001,57600,0*2D\r\n" },
    { BAUD_38400,  "$PUBX,41,1,0003,0001,38400,0*26\r\n" },
    { BAUD_19200,  "$PUBX,41,1,0003,0001,19200,0*23\r\n" },
    { BAUD_9600,   "$PUBX,41,1,0003,0001,9600,0*16\r\n" }
};

#define DEFAULT_BAUD_RATE_INDEX 0

#ifdef USE_GPS_UBLOX
#define MAX_VALSET_SIZE 128

typedef enum {
    PREAMBLE1 = 0xB5,
    PREAMBLE2 = 0x62,
    CLASS_NAV = 0x01,
    CLASS_ACK = 0x05,
    CLASS_CFG = 0x06,
    CLASS_MON = 0x0a,
    CLASS_NMEA_STD = 0xf0,
    MSG_ACK_NACK = 0x00,
    MSG_ACK_ACK = 0x01,
    MSG_NAV_POSLLH = 0x02,
    MSG_NAV_STATUS = 0x03,
    MSG_NAV_DOP = 0x04,
    MSG_NAV_SOL = 0x06,
    MSG_NAV_PVT = 0x07,
    MSG_NAV_VELNED = 0x12,
    MSG_NAV_SVINFO = 0x30,
    MSG_NAV_SAT = 0x35,
    MSG_CFG_VALSET = 0x8a,
    MSG_CFG_VALGET = 0x8b,
    MSG_CFG_MSG = 0x01,
    MSG_CFG_PRT = 0x00,
    MSG_CFG_RATE = 0x08,
    MSG_CFG_SET_RATE = 0x01,
    MSG_CFG_SBAS = 0x16,
    MSG_CFG_NAV_SETTINGS = 0x24,
    MSG_CFG_NAVX_SETTINGS = 0x23,
    MSG_CFG_PMS = 0x86,
    MSG_CFG_GNSS = 0x3E,
    MSG_MON_VER = 0x04,
    MSG_NMEA_GGA = 0x00,
    MSG_NMEA_GLL = 0x01,
    MSG_NMEA_GSA = 0x02,
    MSG_NMEA_GSV = 0x03,
    MSG_NMEA_RMC = 0x04,
    MSG_NMEA_VTG = 0x05,
} ubxProtocolBytes_e;

typedef enum {
    UBX_POWER_MODE_FULL  = 0x00,
    UBX_POWER_MODE_PSMOO = 0x01,
    UBX_POWER_MODE_PSMCT = 0x02,
} ubloxPowerMode_e;

#define UBLOX_MODE_ENABLED    0x1
#define UBLOX_MODE_TEST       0x2

#define UBLOX_USAGE_RANGE     0x1
#define UBLOX_USAGE_DIFFCORR  0x2
#define UBLOX_USAGE_INTEGRITY 0x4

#define UBLOX_GNSS_ENABLE     0x1
#define UBLOX_GNSS_DEFAULT_SIGCFGMASK 0x10000

#define UBLOX_GNSS_GPS        0x00
#define UBLOX_GNSS_SBAS       0x01
#define UBLOX_GNSS_GALILEO    0x02
#define UBLOX_GNSS_BEIDOU     0x03
#define UBLOX_GNSS_IMES       0x04
#define UBLOX_GNSS_QZSS       0x05
#define UBLOX_GNSS_GLONASS    0x06

typedef struct ubxHeader_s {
    uint8_t preamble1;
    uint8_t preamble2;
    uint8_t msg_class;
    uint8_t msg_id;
    uint16_t length;
} ubxHeader_t;

typedef struct ubxConfigblock_s {
    uint8_t gnssId;
    uint8_t resTrkCh;
    uint8_t maxTrkCh;
    uint8_t reserved1;
    uint32_t flags;
} ubxConfigblock_t;

typedef struct ubxPollMsg_s {
    uint8_t msgClass;
    uint8_t msgID;
} ubxPollMsg_t;

typedef struct ubxCfgMsg_s {
    uint8_t msgClass;
    uint8_t msgID;
    uint8_t rate;
} ubxCfgMsg_t;

typedef struct ubxCfgRate_s {
    uint16_t measRate;
    uint16_t navRate;
    uint16_t timeRef;
} ubxCfgRate_t;

typedef struct ubxCfgValSet_s {
    uint8_t version;
    uint8_t layer;
    uint8_t reserved[2];
    uint8_t cfgData[MAX_VALSET_SIZE];
} ubxCfgValSet_t;

typedef struct ubxCfgValGet_s {
    uint8_t version;
    uint8_t layer;
    uint16_t position;
    uint8_t cfgData[MAX_VALSET_SIZE];
} ubxCfgValGet_t;

typedef struct ubxCfgSbas_s {
    uint8_t mode;
    uint8_t usage;
    uint8_t maxSBAS;
    uint8_t scanmode2;
    uint32_t scanmode1;
} ubxCfgSbas_t;

typedef struct ubxCfgGnss_s {
    uint8_t msgVer;
    uint8_t numTrkChHw;
    uint8_t numTrkChUse;
    uint8_t numConfigBlocks;
    ubxConfigblock_t configblocks[7];
} ubxCfgGnss_t;

typedef struct ubxCfgPms_s {
    uint8_t version;
    uint8_t powerSetupValue;
    uint16_t period;
    uint16_t onTime;
    uint8_t reserved1[2];
} ubxCfgPms_t;

typedef struct ubxCfgNav5_s {
    uint16_t mask;
    uint8_t dynModel;
    uint8_t fixMode;
    int32_t fixedAlt;
    uint32_t fixedAltVar;
    int8_t minElev;
    uint8_t drLimit;
    uint16_t pDOP;
    uint16_t tDOP;
    uint16_t pAcc;
    uint16_t tAcc;
    uint8_t staticHoldThresh;
    uint8_t dgnssTimeout;
    uint8_t cnoThreshNumSVs;
    uint8_t cnoThresh;
    uint8_t reserved0[2];
    uint16_t staticHoldMaxDist;
    uint8_t utcStandard;
    uint8_t reserved1[5];
} ubxCfgNav5_t;

typedef struct ubxCfgNav5x_s {
    uint16_t version;
    uint16_t mask1;
    uint32_t mask2;
    uint8_t reserved0[2];
    uint8_t minSVs;
    uint8_t maxSVs;
    uint8_t minCNO;
    uint8_t reserved1;
    uint8_t iniFix3D;
    uint8_t reserved2[2];
    uint8_t ackAiding;
    uint16_t wknRollover;
    uint8_t sigAttenCompMode;
    uint8_t reserved3;
    uint8_t reserved4[2];
    uint8_t reserved5[2];
    uint8_t usePPP;
    uint8_t aopCfg;
    uint8_t reserved6[2];
    uint8_t reserved7[4];
    uint8_t reserved8[3];
    uint8_t useAdr;
} ubxCfgNav5x_t;

typedef union ubxPayload_s {
    ubxPollMsg_t poll_msg;
    ubxCfgMsg_t cfg_msg;
    ubxCfgRate_t cfg_rate;
    ubxCfgValSet_t cfg_valset;
    ubxCfgValGet_t cfg_valget;
    ubxCfgNav5_t cfg_nav5;
    ubxCfgNav5x_t cfg_nav5x;
    ubxCfgSbas_t cfg_sbas;
    ubxCfgGnss_t cfg_gnss;
    ubxCfgPms_t cfg_pms;
} ubxPayload_t;

typedef struct ubxMessage_s {
    ubxHeader_t header;
    ubxPayload_t payload;
} __attribute__((packed)) ubxMessage_t;

typedef enum {
    UBLOX_DETECT_UNIT,      //  0
    UBLOX_SLOW_NAV_RATE,    //  1.
    UBLOX_MSG_DISABLE_NMEA, //  2. Disable NMEA, config message
    UBLOX_MSG_VGS,          //  3. VGS: Course over ground and Ground speed
    UBLOX_MSG_GSV,          //  4. GSV: GNSS Satellites in View
    UBLOX_MSG_GLL,          //  5. GLL: Latitude and longitude, with time of position fix and status
    UBLOX_MSG_GGA,          //  6. GGA: Global positioning system fix data
    UBLOX_MSG_GSA,          //  7. GSA: GNSS DOP and Active Satellites
    UBLOX_MSG_RMC,          //  8. RMC: Recommended Minimum data
    UBLOX_ACQUIRE_MODEL,    //  9
//    UBLOX_CFG_ANA,          //  . ANA: if M10, enable autonomous mode : temporarily disabled.
    UBLOX_SET_SBAS,         // 10. Sets SBAS
    UBLOX_SET_PMS,          // 11. Sets Power Mode
    UBLOX_MSG_NAV_PVT,      // 12. set NAV-PVT rate
    UBLOX_MSG_SOL,          // 13. set SOL MSG rate
    UBLOX_MSG_POSLLH,       // 14. set POSLLH MSG rate
    UBLOX_MSG_STATUS,       // 15: set STATUS MSG rate
    UBLOX_MSG_VELNED,       // 16. set VELNED MSG rate
    UBLOX_MSG_DOP,          // 17. MSG_NAV_DOP
    UBLOX_SAT_INFO,         // 18. MSG_NAV_SAT message
    UBLOX_SET_NAV_RATE,     // 19. set to user requested GPS sample rate
    UBLOX_MSG_CFG_GNSS,     // 20. For not SBAS or GALILEO
    UBLOX_CONFIG_COMPLETE   // 21. Config finished, start receiving data
} ubloxStatePosition_e;

baudRate_e initBaudRateIndex;
size_t initBaudRateCycleCount;
#endif // USE_GPS_UBLOX

gpsData_t gpsData;

#ifdef USE_DASHBOARD
// Functions & data used *only* in support of the dashboard device (OLED display).
// Note this should be refactored to move dashboard functionality to the dashboard module, and only have generic hooks in the gps module...

char dashboardGpsPacketLog[GPS_PACKET_LOG_ENTRY_COUNT];             // OLED display of a char for each packet type/event received.
char *dashboardGpsPacketLogCurrentChar = dashboardGpsPacketLog;     // Current character of log being updated.
uint32_t dashboardGpsPacketCount = 0;                               // Packet received count.
uint32_t dashboardGpsNavSvInfoRcvCount = 0;                         // Count of times sat info updated.

static void shiftPacketLog(void)
{
    memmove(dashboardGpsPacketLog + 1, dashboardGpsPacketLog, (ARRAYLEN(dashboardGpsPacketLog) - 1) * sizeof(dashboardGpsPacketLog[0]));
}

static void logErrorToPacketLog(void)
{
    shiftPacketLog();
    *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_ERROR;
    gpsData.errors++;
}
#endif  // USE_DASHBOARD

static void gpsNewData(uint16_t c);
#ifdef USE_GPS_NMEA
static bool gpsNewFrameNMEA(char c);
#endif
#ifdef USE_GPS_UBLOX
static bool gpsNewFrameUBLOX(uint8_t data);
#endif

static void gpsSetState(gpsState_e state)
{
    gpsData.lastNavMessage = gpsData.now;
    sensorsClear(SENSOR_GPS);
    gpsData.state = state;
    gpsData.state_position = 0;
    gpsData.state_ts = gpsData.now;
    gpsData.ackState = UBLOX_ACK_IDLE;
}

void gpsInit(void)
{
    gpsDataIntervalSeconds = 0.1f;
    gpsData.userBaudRateIndex = 0;
    gpsData.timeouts = 0;
    gpsData.state_ts = millis();
#ifdef USE_GPS_UBLOX
    gpsData.ubloxUsingFlightModel = false;
#endif
    gpsData.updateRateHz = 10; // initialise at 10hz
    gpsData.platformVersion = UBX_VERSION_UNDEF;

#ifdef USE_DASHBOARD
    gpsData.errors = 0;
    memset(dashboardGpsPacketLog, 0x00, sizeof(dashboardGpsPacketLog));
#endif

    // init gpsData structure. if we're not actually enabled, don't bother doing anything else
    gpsSetState(GPS_STATE_UNKNOWN);

    if (gpsConfig()->provider == GPS_MSP || gpsConfig()->provider == GPS_VIRTUAL) { // no serial ports used when GPS_MSP or GPS_VIRTUAL is configured
        gpsSetState(GPS_STATE_INITIALIZED);
        return;
    }

    const serialPortConfig_t *gpsPortConfig = findSerialPortConfig(FUNCTION_GPS);
    if (!gpsPortConfig) {
        return;
    }

    // set the user's intended baud rate
    initBaudRateIndex = BAUD_COUNT;
    initBaudRateCycleCount = 0;
    gpsData.userBaudRateIndex = DEFAULT_BAUD_RATE_INDEX;
    for (unsigned i = 0; i < ARRAYLEN(gpsInitData); i++) {
        if (gpsInitData[i].baudrateIndex == gpsPortConfig->gps_baudrateIndex) {
            gpsData.userBaudRateIndex = i;
            break;
        }
    }
    // the user's intended baud rate will be used as the initial baud rate when connecting
    gpsData.tempBaudRateIndex = gpsData.userBaudRateIndex;

    portMode_e mode = MODE_RXTX;
    portOptions_e options = SERIAL_NOT_INVERTED;

#if defined(GPS_NMEA_TX_ONLY)
    if (gpsConfig()->provider == GPS_NMEA) {
        mode &= ~MODE_TX;
    }
#endif
    if (serialType(gpsPortConfig->identifier) == SERIALTYPE_UART
        || serialType(gpsPortConfig->identifier) == SERIALTYPE_LPUART) {
        // TODO: SERIAL_CHECK_TX is broken on F7, disable it until it is fixed
#if !defined(STM32F7) || defined(USE_F7_CHECK_TX)
        options |= SERIAL_CHECK_TX;
#endif
    }

    // no callback - buffer will be consumed in gpsUpdate()
    gpsPort = openSerialPort(gpsPortConfig->identifier, FUNCTION_GPS, NULL, NULL, baudRates[gpsInitData[gpsData.userBaudRateIndex].baudrateIndex], mode, options);
    if (!gpsPort) {
        return;
    }

    // signal GPS "thread" to initialize when it gets to it
    gpsSetState(GPS_STATE_DETECT_BAUD);
    // NB gpsData.state_position is set to zero by gpsSetState(), requesting the fastest baud rate option first time around.
}

#ifdef USE_GPS_UBLOX
const uint8_t ubloxUTCStandardConfig_int[5] = {
    UBLOX_UTC_STANDARD_AUTO,
    UBLOX_UTC_STANDARD_USNO,
    UBLOX_UTC_STANDARD_EU,
    UBLOX_UTC_STANDARD_SU,
    UBLOX_UTC_STANDARD_NTSC
};

struct ubloxVersion_s ubloxVersionMap[] = {
    [UBX_VERSION_UNDEF] = { ~0, "UNKNOWN" },
    [UBX_VERSION_M5] = { 0x00040005, "M5" },
    [UBX_VERSION_M6] = { 0x00040007, "M6" },
    [UBX_VERSION_M7] = { 0x00070000, "M7" },
    [UBX_VERSION_M8] = { 0x00080000, "M8" },
    [UBX_VERSION_M9] = { 0x00190000, "M9" },
    [UBX_VERSION_M10] = { 0x000A0000, "M10" },
};

static uint8_t ubloxAddValSet(ubxMessage_t * tx_buffer, ubxValGetSetBytes_e key, const uint8_t * payload, const uint8_t offset)
{
    size_t len;
    switch((key >> (8 * 3)) & 0xff) {
    case 0x10:
    case 0x20:
        len = 1;
        break;
    case 0x30:
        len = 2;
        break;
    case 0x40:
        len = 4;
        break;
    case 0x50:
        len = 8;
        break;
    default:
        return 0;
    }

    if (offset + 4 + len > MAX_VALSET_SIZE) {
        return 0;
    }

    tx_buffer->payload.cfg_valset.cfgData[offset + 0] = (uint8_t)(key >> (8 * 0));
    tx_buffer->payload.cfg_valset.cfgData[offset + 1] = (uint8_t)(key >> (8 * 1));
    tx_buffer->payload.cfg_valset.cfgData[offset + 2] = (uint8_t)(key >> (8 * 2));
    tx_buffer->payload.cfg_valset.cfgData[offset + 3] = (uint8_t)(key >> (8 * 3));

    for (size_t i = 0; i < len; ++i) {
        tx_buffer->payload.cfg_valset.cfgData[offset + 4 + i] = payload[i];
    }

    return 4 + len;
}

// the following lines are not being used, because we are not currently sending ubloxValGet messages
#if 0
static size_t ubloxAddValGet(ubxMessage_t * tx_buffer, ubxValGetSetBytes_e key, size_t offset) {
    const uint8_t zeroes[8] = {0};

    return ubloxAddValSet(tx_buffer, key, zeroes, offset);
}

static size_t ubloxValGet(ubxMessage_t * tx_buffer, ubxValGetSetBytes_e key, ubloxValLayer_e layer)
{
    tx_buffer->header.preamble1 = PREAMBLE1;
    tx_buffer->header.preamble2 = PREAMBLE2;
    tx_buffer->header.msg_class = CLASS_CFG;
    tx_buffer->header.msg_id = MSG_CFG_VALGET;

    tx_buffer->payload.cfg_valget.version = 1;
    tx_buffer->payload.cfg_valget.layer = layer;
    tx_buffer->payload.cfg_valget.position = 0;

    return ubloxAddValGet(tx_buffer, key, 0);
}
#endif // not used

static uint8_t ubloxValSet(ubxMessage_t * tx_buffer, ubxValGetSetBytes_e key, uint8_t * payload, ubloxValLayer_e layer)
{
    memset(&tx_buffer->payload.cfg_valset, 0, sizeof(ubxCfgValSet_t));

    // tx_buffer->payload.cfg_valset.version = 0;
    tx_buffer->payload.cfg_valset.layer = layer;
    // tx_buffer->payload.cfg_valset.reserved[0] = 0;
    // tx_buffer->payload.cfg_valset.reserved[1] = 0;

    return ubloxAddValSet(tx_buffer, key, payload, 0);
}

static void ubloxSendByteUpdateChecksum(const uint8_t data, uint8_t *checksumA, uint8_t *checksumB)
{
    *checksumA += data;
    *checksumB += *checksumA;
    serialWrite(gpsPort, data);
}

static void ubloxSendDataUpdateChecksum(const ubxMessage_t *msg, uint8_t *checksumA, uint8_t *checksumB)
{
    // CRC includes msg_class, msg_id, length and payload
    // length is payload length only
    const uint8_t *data = (const uint8_t *)&msg->header.msg_class;
    uint16_t len = msg->header.length + sizeof(msg->header.msg_class) + sizeof(msg->header.msg_id) + sizeof(msg->header.length);

    while (len--) {
        ubloxSendByteUpdateChecksum(*data, checksumA, checksumB);
        data++;
    }
}

static void ubloxSendMessage(const ubxMessage_t *msg, bool skipAck)
{
    uint8_t checksumA = 0, checksumB = 0;
    serialWrite(gpsPort, msg->header.preamble1);
    serialWrite(gpsPort, msg->header.preamble2);
    ubloxSendDataUpdateChecksum(msg, &checksumA, &checksumB);
    serialWrite(gpsPort, checksumA);
    serialWrite(gpsPort, checksumB);
    // Save state for ACK waiting
    gpsData.ackWaitingMsgId = msg->header.msg_id; //save message id for ACK
    gpsData.ackState = skipAck ? UBLOX_ACK_GOT_ACK : UBLOX_ACK_WAITING;
    gpsData.lastMessageSent = gpsData.now;
}

static void ubloxSendClassMessage(ubxProtocolBytes_e class_id, ubxProtocolBytes_e msg_id, uint16_t length)
{
    ubxMessage_t msg;
    msg.header.preamble1 = PREAMBLE1;
    msg.header.preamble2 = PREAMBLE2;
    msg.header.msg_class = class_id;
    msg.header.msg_id = msg_id;
    msg.header.length = length;
    ubloxSendMessage(&msg, false);
}

static void ubloxSendConfigMessage(ubxMessage_t *msg, uint8_t msg_id, uint8_t length, bool skipAck)
{
    msg->header.preamble1 = PREAMBLE1;
    msg->header.preamble2 = PREAMBLE2;
    msg->header.msg_class = CLASS_CFG;
    msg->header.msg_id = msg_id;
    msg->header.length = length;
    ubloxSendMessage(msg, skipAck);
}

static void ubloxSendPollMessage(uint8_t msg_id)
{
    ubxMessage_t msg;
    msg.header.preamble1 = PREAMBLE1;
    msg.header.preamble2 = PREAMBLE2;
    msg.header.msg_class = CLASS_CFG;
    msg.header.msg_id = msg_id;
    msg.header.length = 0;
    ubloxSendMessage(&msg, false);
}

static void ubloxSendNAV5Message(uint8_t model)
{
    DEBUG_SET(DEBUG_GPS_CONNECTION, 0, model);
    ubxMessage_t tx_buffer;
    if (gpsData.ubloxM9orAbove) {
        uint8_t payload[4];
        payload[0] = (model == 0 ? 0 : model + 1);
        size_t offset = ubloxValSet(&tx_buffer, CFG_NAVSPG_DYNMODEL, payload, UBX_VAL_LAYER_RAM); // 5

        // the commented out payload lines are those which only set the M9 or above module to default values.

//         payload[0] = 3; // set 2D/3D fix mode to auto, which is the default
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_FIXMODE, payload, offset); // 10

        payload[0] = ubloxUTCStandardConfig_int[gpsConfig()->gps_ublox_utc_standard];
        offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_UTCSTANDARD, payload, offset); // 15

//         payload[0] = 0;
//         payload[1] = (uint8_t)(0 >> (8 * 1));
//         payload[2] = (uint8_t)(0 >> (8 * 2));
//         payload[3] = (uint8_t)(0 >> (8 * 3));  // all payloads are zero, the default MSL for 2D fix
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_CONSTR_ALT, payload, offset); // 23
//
//         payload[0] = (uint8_t)(10000 >> (8 * 0));
//         payload[1] = (uint8_t)(10000 >> (8 * 1));
//         payload[2] = (uint8_t)(10000 >> (8 * 2));
//         payload[3] = (uint8_t)(10000 >> (8 * 3)); // // all payloads are 1000, the default 2D variance factor
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_CONSTR_ALTVAR, payload, offset); // 31
//
//         payload[0] = 5; // sets the default minimum elevation in degrees to the default of 5
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_INFIL_MINELEV, payload, offset); // 36
//
//         payload[0] = (uint8_t)(250 >> (8 * 0));
//         payload[1] = (uint8_t)(250 >> (8 * 1)); // sets the output filter PDOP mask to default of 250
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_OUTFIL_PDOP, payload, offset); // 42
//
//         payload[0] = (uint8_t)(250 >> (8 * 0));
//         payload[1] = (uint8_t)(250 >> (8 * 1));
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_OUTFIL_TDOP, payload, offset); // 48
//
//         payload[0] = (uint8_t)(100 >> (8 * 0));
//         payload[1] = (uint8_t)(100 >> (8 * 1));
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_OUTFIL_PACC, payload, offset); // 54
//
//         payload[0] = (uint8_t)(300 >> (8 * 0));
//         payload[1] = (uint8_t)(300 >> (8 * 1));
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_OUTFIL_TACC, payload, offset); // 60
//
//         payload[0] = 0;
//         offset += ubloxAddValSet(&tx_buffer, CFG_MOT_GNSSSPEED_THRS, payload, offset); // 65
//
//         payload[0] = (uint8_t)(200 >> (8 * 0));
//         payload[1] = (uint8_t)(200 >> (8 * 1));
//         offset += ubloxAddValSet(&tx_buffer, CFG_MOT_GNSSDIST_THRS, payload, offset); // 71

//         payload[0] = (uint8_t)(60 >> (8 * 0));
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_CONSTR_DGNSSTO, payload, offset); // 76

//         payload[0] = 0;
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_INFIL_NCNOTHRS, payload, offset); // 81
//
//         payload[0] = 0;
//         offset += ubloxAddValSet(&tx_buffer, CFG_NAVSPG_INFIL_CNOTHRS, payload, offset); // 86

        ubloxSendConfigMessage(&tx_buffer, MSG_CFG_VALSET, offsetof(ubxCfgValSet_t, cfgData) + offset, true);
    } else {
        memset(&tx_buffer, 0, sizeof(ubxMessage_t));
        tx_buffer.payload.cfg_nav5.mask = 0xFFFF;
        tx_buffer.payload.cfg_nav5.dynModel = model == 0 ? 0 : model + 1; //no model with value 1
        tx_buffer.payload.cfg_nav5.fixMode = 3;
        tx_buffer.payload.cfg_nav5.fixedAlt = 0;
        tx_buffer.payload.cfg_nav5.fixedAltVar = 10000;
        tx_buffer.payload.cfg_nav5.minElev = 5;
        tx_buffer.payload.cfg_nav5.drLimit = 0;
        tx_buffer.payload.cfg_nav5.pDOP = 250;
        tx_buffer.payload.cfg_nav5.tDOP = 250;
        tx_buffer.payload.cfg_nav5.pAcc = 100;
        tx_buffer.payload.cfg_nav5.tAcc = 300;
        tx_buffer.payload.cfg_nav5.staticHoldThresh = 0;
        tx_buffer.payload.cfg_nav5.dgnssTimeout = 60;
        tx_buffer.payload.cfg_nav5.cnoThreshNumSVs = 0;
        tx_buffer.payload.cfg_nav5.cnoThresh = 0;
        tx_buffer.payload.cfg_nav5.staticHoldMaxDist = 200;
        tx_buffer.payload.cfg_nav5.utcStandard = ubloxUTCStandardConfig_int[gpsConfig()->gps_ublox_utc_standard];

        ubloxSendConfigMessage(&tx_buffer, MSG_CFG_NAV_SETTINGS, sizeof(ubxCfgNav5_t), false);
    }
}

// *** Assist Now Autonomous temporarily disabled until a subsequent PR either includes, or removes it ***
// static void ubloxSendNavX5Message(void) {
//     ubxMessage_t tx_buffer;
//
//     if (gpsData.ubloxM9orAbove) {
//         uint8_t payload[1];
//         payload[0] = 1;
//         size_t offset = ubloxValSet(&tx_buffer, CFG_ANA_USE_ANA, payload, UBX_VAL_LAYER_RAM); // 5
//
//         ubloxSendConfigMessage(&tx_buffer, MSG_CFG_VALSET, offsetof(ubxCfgValSet_t, cfgData) + offset, true);
//     } else {
//         memset(&tx_buffer, 0, sizeof(ubxMessage_t));
//
//         tx_buffer.payload.cfg_nav5x.version = 0x0002;
//
//         tx_buffer.payload.cfg_nav5x.mask1 = 0x4000;
//         tx_buffer.payload.cfg_nav5x.mask2 = 0x0;
//         tx_buffer.payload.cfg_nav5x.minSVs = 0;
//         tx_buffer.payload.cfg_nav5x.maxSVs = 0;
//         tx_buffer.payload.cfg_nav5x.minCNO = 0;
//         tx_buffer.payload.cfg_nav5x.reserved1 = 0;
//         tx_buffer.payload.cfg_nav5x.iniFix3D = 0;
//         tx_buffer.payload.cfg_nav5x.ackAiding = 0;
//         tx_buffer.payload.cfg_nav5x.wknRollover = 0;
//         tx_buffer.payload.cfg_nav5x.sigAttenCompMode = 0;
//         tx_buffer.payload.cfg_nav5x.usePPP = 0;
//
//         tx_buffer.payload.cfg_nav5x.aopCfg = 0x1; //bit 0 = useAOP
//
//         tx_buffer.payload.cfg_nav5x.useAdr = 0;
//
//         ubloxSendConfigMessage(&tx_buffer, MSG_CFG_NAVX_SETTINGS, sizeof(ubxCfgNav5x_t), false);
//     }
// }

static void ubloxSetPowerModeValSet(uint8_t powerSetupValue)
{
    ubxMessage_t tx_buffer;

    uint8_t payload[1];
    payload[0] = powerSetupValue;

    size_t offset = ubloxValSet(&tx_buffer, CFG_PM_OPERATEMODE, payload, UBX_VAL_LAYER_RAM);

    ubloxSendConfigMessage(&tx_buffer, MSG_CFG_VALSET, offsetof(ubxCfgValSet_t, cfgData) + offset, true);
}

static void ubloxSendPowerMode(void)
{
    if (gpsData.ubloxM9orAbove) {
        ubloxSetPowerModeValSet(UBX_POWER_MODE_FULL);
    } else if (gpsData.ubloxM8orAbove) {
        ubxMessage_t tx_buffer;
        tx_buffer.payload.cfg_pms.version = 0;
        tx_buffer.payload.cfg_pms.powerSetupValue = UBX_POWER_MODE_FULL;
        tx_buffer.payload.cfg_pms.period = 0;
        tx_buffer.payload.cfg_pms.onTime = 0;
        tx_buffer.payload.cfg_pms.reserved1[0] = 0;
        tx_buffer.payload.cfg_pms.reserved1[1] = 0;
        ubloxSendConfigMessage(&tx_buffer, MSG_CFG_PMS, sizeof(ubxCfgPms_t), false);
    }
    // M7 and below do not support this type of power mode, so we leave at default.
}

static void ubloxSetMessageRate(uint8_t messageClass, uint8_t messageID, uint8_t rate)
{
    ubxMessage_t tx_buffer;
    tx_buffer.payload.cfg_msg.msgClass = messageClass;
    tx_buffer.payload.cfg_msg.msgID = messageID;
    tx_buffer.payload.cfg_msg.rate = rate;
    ubloxSendConfigMessage(&tx_buffer, MSG_CFG_MSG, sizeof(ubxCfgMsg_t), false);
}

static void ubloxSetMessageRateValSet(ubxValGetSetBytes_e msgClass, uint8_t rate)
{
    ubxMessage_t tx_buffer;

    uint8_t payload[1];
    payload[0] = rate;

    size_t offset = ubloxValSet(&tx_buffer, msgClass, payload, UBX_VAL_LAYER_RAM);

    ubloxSendConfigMessage(&tx_buffer, MSG_CFG_VALSET, offsetof(ubxCfgValSet_t, cfgData) + offset, true);
}

static void ubloxDisableNMEAValSet(void)
{
    ubxMessage_t tx_buffer;

    uint8_t payload[1];

    payload[0] = 0;

//    size_t offset = ubloxValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GGA_I2C, payload, UBX_VAL_LAYER_RAM);
//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GGA_SPI, payload, offset);
//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GGA_UART1, payload, offset);
    size_t offset = ubloxValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GGA_UART1, payload, UBX_VAL_LAYER_RAM);

//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_VTG_I2C, payload, offset);
//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_VTG_SPI, payload, offset);
    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_VTG_UART1, payload, offset);

//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GSV_I2C, payload, offset);
//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GSV_SPI, payload, offset);
    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GSV_UART1, payload, offset);

//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GLL_I2C, payload, offset);
//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GLL_SPI, payload, offset);
    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GLL_UART1, payload, offset);

//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GSA_I2C, payload, offset);
//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GSA_SPI, payload, offset);
    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_GSA_UART1, payload, offset);

//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_RMC_I2C, payload, offset);
//    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_RMC_SPI, payload, offset);
    offset += ubloxAddValSet(&tx_buffer, CFG_MSGOUT_NMEA_ID_RMC_UART1, payload, offset);

    ubloxSendConfigMessage(&tx_buffer, MSG_CFG_VALSET, offsetof(ubxCfgValSet_t, cfgData) + offset, true);
}

static void ubloxSetNavRate(uint16_t measRate, uint16_t navRate, uint8_t timeRef)
{
    uint16_t measRateMilliseconds = 1000 / measRate;

    ubxMessage_t tx_buffer;
    if (gpsData.ubloxM9orAbove) {
        uint8_t offset = 0;
        uint8_t payload[2];
        payload[0] = (uint8_t)(measRateMilliseconds >> (8 * 0));
        payload[1] = (uint8_t)(measRateMilliseconds >> (8 * 1));
        //rate meas is U2
        offset = ubloxValSet(&tx_buffer, CFG_RATE_MEAS, payload, UBX_VAL_LAYER_RAM);

        payload[0] = (uint8_t)(navRate >> (8 * 0));
        payload[1] = (uint8_t)(navRate >> (8 * 1));
        //rate nav is U2
        offset += ubloxAddValSet(&tx_buffer, CFG_RATE_NAV, payload, 6);

        payload[0] = timeRef;
        //rate timeref is E1 = U1
        offset += ubloxAddValSet(&tx_buffer, CFG_RATE_TIMEREF, payload, 12);

        ubloxSendConfigMessage(&tx_buffer, MSG_CFG_VALSET, offsetof(ubxCfgValSet_t, cfgData) + offset, false);
    } else {
        tx_buffer.payload.cfg_rate.measRate = measRateMilliseconds;
        tx_buffer.payload.cfg_rate.navRate = navRate;
        tx_buffer.payload.cfg_rate.timeRef = timeRef;
        ubloxSendConfigMessage(&tx_buffer, MSG_CFG_RATE, sizeof(ubxCfgRate_t), true);
    }
}

static void ubloxSetSbas(void)
{
    ubxMessage_t tx_buffer;

    if (gpsData.ubloxM9orAbove) {
        uint8_t payload[8];
        payload[0] = gpsConfig()->sbasMode != SBAS_NONE;

        size_t offset = ubloxValSet(&tx_buffer, CFG_SBAS_USE_TESTMODE, payload, UBX_VAL_LAYER_RAM);

        payload[0] = 1;
        offset += ubloxAddValSet(&tx_buffer, CFG_SBAS_USE_RANGING, payload, offset);
        offset += ubloxAddValSet(&tx_buffer, CFG_SBAS_USE_DIFFCORR, payload, offset);

        if (gpsConfig()->sbas_integrity) {
            offset += ubloxAddValSet(&tx_buffer, CFG_SBAS_USE_INTEGRITY, payload, offset);
        }

        uint64_t mask = SBAS_SEARCH_ALL;
        switch (gpsConfig()->sbasMode) {
        case SBAS_EGNOS:
            mask = SBAS_SEARCH_PRN(123) | SBAS_SEARCH_PRN(126) | SBAS_SEARCH_PRN(136);
            break;
        case SBAS_WAAS:
            mask = SBAS_SEARCH_PRN(131) | SBAS_SEARCH_PRN(133) | SBAS_SEARCH_PRN(135) | SBAS_SEARCH_PRN(138);
            break;
        case SBAS_MSAS:
            mask = SBAS_SEARCH_PRN(129) | SBAS_SEARCH_PRN(137);
            break;
        case SBAS_GAGAN:
            mask = SBAS_SEARCH_PRN(127) | SBAS_SEARCH_PRN(128) | SBAS_SEARCH_PRN(132);
            break;
        case SBAS_AUTO:
        default:
            break;
        }

        payload[0] = (uint8_t)(mask >> (8 * 0));
        payload[1] = (uint8_t)(mask >> (8 * 1));
        payload[2] = (uint8_t)(mask >> (8 * 2));
        payload[3] = (uint8_t)(mask >> (8 * 3));
        payload[4] = (uint8_t)(mask >> (8 * 4));
        payload[5] = (uint8_t)(mask >> (8 * 5));
        payload[6] = (uint8_t)(mask >> (8 * 6));
        payload[7] = (uint8_t)(mask >> (8 * 7));

        offset += ubloxAddValSet(&tx_buffer, CFG_SBAS_PRNSCANMASK, payload, offset);

        ubloxSendConfigMessage(&tx_buffer, MSG_CFG_VALSET, offsetof(ubxCfgValSet_t, cfgData) + offset, true);
    } else {
        //NOTE: default ublox config for sbas mode is: UBLOX_MODE_ENABLED, test is disabled
        tx_buffer.payload.cfg_sbas.mode = UBLOX_MODE_TEST;
        if (gpsConfig()->sbasMode != SBAS_NONE) {
            tx_buffer.payload.cfg_sbas.mode |= UBLOX_MODE_ENABLED;
        }

        // NOTE: default ublox config for sbas mode is: UBLOX_USAGE_RANGE | UBLOX_USAGE_DIFFCORR, integrity is disabled
        tx_buffer.payload.cfg_sbas.usage = UBLOX_USAGE_RANGE | UBLOX_USAGE_DIFFCORR;
        if (gpsConfig()->sbas_integrity) {
            tx_buffer.payload.cfg_sbas.usage |= UBLOX_USAGE_INTEGRITY;
        }

        tx_buffer.payload.cfg_sbas.maxSBAS = 3;
        tx_buffer.payload.cfg_sbas.scanmode2 = 0;
        switch (gpsConfig()->sbasMode) {
        case SBAS_AUTO:
            tx_buffer.payload.cfg_sbas.scanmode1 = 0;
            break;
        case SBAS_EGNOS:
            tx_buffer.payload.cfg_sbas.scanmode1 = 0x00010048; //PRN123, PRN126, PRN136
            break;
        case SBAS_WAAS:
            tx_buffer.payload.cfg_sbas.scanmode1 = 0x0004A800; //PRN131, PRN133, PRN135, PRN138
            break;
        case SBAS_MSAS:
            tx_buffer.payload.cfg_sbas.scanmode1 = 0x00020200; //PRN129, PRN137
            break;
        case SBAS_GAGAN:
            tx_buffer.payload.cfg_sbas.scanmode1 = 0x00001180; //PRN127, PRN128, PRN132
            break;
        default:
            tx_buffer.payload.cfg_sbas.scanmode1 = 0;
            break;
        }
        ubloxSendConfigMessage(&tx_buffer, MSG_CFG_SBAS, sizeof(ubxCfgSbas_t), false);
    }
}

static void setSatInfoMessageRate(uint8_t divisor)
{
    // enable satInfoMessage at 1:5 of the nav rate if configurator is connected
    if (gpsData.ubloxM9orAbove) {
         ubloxSetMessageRateValSet(CFG_MSGOUT_UBX_NAV_SAT_UART1, divisor);
    } else if (gpsData.ubloxM8orAbove) {
        ubloxSetMessageRate(CLASS_NAV, MSG_NAV_SAT, divisor);
    } else {
        ubloxSetMessageRate(CLASS_NAV, MSG_NAV_SVINFO, divisor);
    }
}

#endif // USE_GPS_UBLOX

#ifdef USE_GPS_NMEA
static void gpsConfigureNmea(void)
{
    // minimal support for NMEA, we only:
    // - set the FC's GPS port to the user's configured rate (initial rate, e.g. 9600), and
    // - send any NMEA custom commands to the GPS Module (e.g. to change rate to 57600)
    // - set the FC's GPS port to the target rate (e.g. 57600)
    DEBUG_SET(DEBUG_GPS_CONNECTION, 4, (gpsData.state * 100 + gpsData.state_position));

    // wait 500ms between changes
    if (cmp32(gpsData.now, gpsData.state_ts) < 500) {
        return;
    }
    gpsData.state_ts = gpsData.now;

    // Check that the GPS transmit buffer is empty
    if (!isSerialTransmitBufferEmpty(gpsPort)) {
        return;
    }

    switch (gpsData.state) {

    case GPS_STATE_DETECT_BAUD:
        // Assume initial rate is set correctly via user config (e.g., 9600)
        gpsSetState(GPS_STATE_CHANGE_BAUD);
        break;

    case GPS_STATE_CHANGE_BAUD:
#if !defined(GPS_NMEA_TX_ONLY)
        if (gpsData.state_position < 1) {
            // Step 1: Set the FC's baud rate initially to the user's configured rate (assumed 9600).
            // This ensures we talk to the GPS at its default rate first.
            serialSetBaudRate(gpsPort, baudRates[gpsInitData[gpsData.userBaudRateIndex].baudrateIndex]);
            gpsData.state_position++;
        } else if (gpsData.state_position < 2) {
            // Step 2: Send NMEA custom commands (at the initial rate, e.g., 9600).
            static int commandOffset = 0;
            const char *commands = gpsConfig()->nmeaCustomCommands;
            const char *cmd = commands + commandOffset;
            // skip leading whitespaces and get first command length
            int commandLen;
            while (*cmd && (commandLen = strcspn(cmd, " \0")) == 0) {
                cmd++;  // skip separators
            }
            if (*cmd) {
                // Send the current command to the GPS
                serialWriteBuf(gpsPort, (uint8_t *)cmd, commandLen);
                serialWriteBuf(gpsPort, (uint8_t *)"\r\n", 2);
                // Move to the next command
                cmd += commandLen;
            }
            // skip trailing whitespaces
            while (*cmd && strcspn(cmd, " \0") == 0) cmd++;
            if (*cmd) {
                // more commands to send in the next iteration
                commandOffset = cmd - commands;
            } else {
                // All commands sent, move to the next step to change FC baud rate
                gpsData.state_position++; // Moves to state_position 2
                commandOffset = 0;
            }
        } else if (gpsData.state_position < 3) {
            // Step 3: Change FC baud rate to the target rate (57600).
            // NOTE: This assumes the custom command successfully changed the GPS to 57600 baud.
            // NOTE: The target baud rate 57600 is hardcoded here. Consider making this configurable if needed.
            baudRate_e targetBaudIndex = BAUD_57600;
            serialSetBaudRate(gpsPort, baudRates[targetBaudIndex]);
            gpsData.state_position++; // Moves to state_position 3
        } else { // state_position >= 3
             // Step 4: Configuration complete, transition to receiving data at the new rate.
             gpsSetState(GPS_STATE_RECEIVING_DATA);
        }
#else // !GPS_NMEA_TX_ONLY
        // If TX is disabled, just go straight to receiving data at the configured rate
        serialSetBaudRate(gpsPort, baudRates[gpsInitData[gpsData.userBaudRateIndex].baudrateIndex]);
        gpsSetState(GPS_STATE_RECEIVING_DATA);
#endif // !GPS_NMEA_TX_ONLY
        break;
    }
}
#endif // USE_GPS_NMEA

#ifdef USE_GPS_UBLOX

static void gpsConfigureUblox(void)
{

    // Wait until GPS transmit buffer is empty
    if (!isSerialTransmitBufferEmpty(gpsPort)) {
        return;
    }

    switch (gpsData.state) {
    case GPS_STATE_DETECT_BAUD:

        DEBUG_SET(DEBUG_GPS_CONNECTION, 3, baudRates[gpsInitData[gpsData.tempBaudRateIndex].baudrateIndex] / 100);

        // check to see if there has been a response to the version command
        // initially the FC will be at the user-configured baud rate.
        if (gpsData.platformVersion > UBX_VERSION_UNDEF) {
            // set the GPS module's serial port to the user's intended baud rate
            serialPrint(gpsPort, gpsInitData[gpsData.userBaudRateIndex].ubx);
            // use this baud rate for re-connections
            gpsData.tempBaudRateIndex = gpsData.userBaudRateIndex;
            // we're done here, let's move the the next state
            gpsSetState(GPS_STATE_CHANGE_BAUD);
            return;
        }

        // Send MON-VER messages at GPS_CONFIG_BAUD_CHANGE_INTERVAL for GPS_BAUDRATE_TEST_COUNT times
        static bool messageSent = false;
        static uint8_t messageCounter = 0;
        DEBUG_SET(DEBUG_GPS_CONNECTION, 2, initBaudRateCycleCount * 100 + messageCounter);

        if (messageCounter < GPS_BAUDRATE_TEST_COUNT) {
            if (!messageSent) {
                gpsData.platformVersion = UBX_VERSION_UNDEF;
                ubloxSendClassMessage(CLASS_MON, MSG_MON_VER, 0);
                gpsData.ackState = UBLOX_ACK_IDLE; // ignore ACK for this message
                messageSent = true;
            }
            if (cmp32(gpsData.now, gpsData.state_ts) > GPS_CONFIG_BAUD_CHANGE_INTERVAL) {
                gpsData.state_ts = gpsData.now;
                messageSent = false;
                messageCounter++;
            }
            return;
        }
        messageCounter = 0;
        gpsData.state_ts = gpsData.now;

        // failed to connect at that rate after five attempts
        // try other GPS baudrates, starting at 9600 and moving up
        if (gpsData.tempBaudRateIndex == 0) {
            gpsData.tempBaudRateIndex = ARRAYLEN(gpsInitData) - 1; // slowest baud rate (9600)
        } else {
            gpsData.tempBaudRateIndex--;
        }
        // set the FC baud rate to the new temp baud rate
        serialSetBaudRate(gpsPort, baudRates[gpsInitData[gpsData.tempBaudRateIndex].baudrateIndex]);
        initBaudRateCycleCount++;

        break;

    case GPS_STATE_CHANGE_BAUD:
        // give time for the GPS module's serial port to settle
        // very important for M8 to give the module a lot of time before sending commands
        // M10 only need about 200ms but M8 and below sometimes need as long as 1000ms
        if (cmp32(gpsData.now, gpsData.state_ts) < (3 * GPS_CONFIG_BAUD_CHANGE_INTERVAL)) {
            return;
        }
        // set the FC's serial port to the configured rate
        serialSetBaudRate(gpsPort, baudRates[gpsInitData[gpsData.userBaudRateIndex].baudrateIndex]);
        DEBUG_SET(DEBUG_GPS_CONNECTION, 3, baudRates[gpsInitData[gpsData.userBaudRateIndex].baudrateIndex] / 100);
        // then start sending configuration settings
        gpsSetState(GPS_STATE_CONFIGURE);
        break;

    case GPS_STATE_CONFIGURE:
        // Either use specific config file for GPS or let dynamically upload config
        if (gpsConfig()->autoConfig == GPS_AUTOCONFIG_OFF) {
            gpsSetState(GPS_STATE_RECEIVING_DATA);
            break;
        }

        // Add delay to stabilize the connection
        if (cmp32(gpsData.now, gpsData.state_ts) < 1000) {
            return;
        }

        if (gpsData.ackState == UBLOX_ACK_IDLE) {

            // short delay before between commands, including the first command
            static uint32_t last_state_position_time = 0;
            if (last_state_position_time == 0) {
                    last_state_position_time = gpsData.now;
            }
            if (cmp32(gpsData.now, last_state_position_time) < GPS_CONFIG_CHANGE_INTERVAL) {
                return;
            }
            last_state_position_time = gpsData.now;

            switch (gpsData.state_position) {
            // if a UBX command is sent, ack is supposed to give position++ once the reply happens
            case UBLOX_DETECT_UNIT: // 400 in debug
                if (gpsData.platformVersion == UBX_VERSION_UNDEF) {
                    ubloxSendClassMessage(CLASS_MON, MSG_MON_VER, 0);
                } else {
                    gpsData.state_position++;
                }
                break;
            case UBLOX_SLOW_NAV_RATE: // 401 in debug
                ubloxSetNavRate(1, 1, 1); // throttle nav data rate to one per second, until configured
                break;
                case UBLOX_MSG_DISABLE_NMEA:
                if (gpsData.ubloxM9orAbove) {
                    ubloxDisableNMEAValSet();
                    gpsData.state_position = UBLOX_MSG_RMC; // skip UBX NMEA entries - goes to RMC plus one, or ACQUIRE_MODEL
                } else {
                    gpsData.state_position++;
                }
                break;
            case UBLOX_MSG_VGS: //Disable NMEA Messages
                ubloxSetMessageRate(CLASS_NMEA_STD, MSG_NMEA_VTG, 0); // VGS: Course over ground and Ground speed
                break;
            case UBLOX_MSG_GSV:
                ubloxSetMessageRate(CLASS_NMEA_STD, MSG_NMEA_GSV, 0); // GSV: GNSS Satellites in View
                break;
            case UBLOX_MSG_GLL:
                ubloxSetMessageRate(CLASS_NMEA_STD, MSG_NMEA_GLL, 0); // GLL: Latitude and longitude, with time of position fix and status
                break;
            case UBLOX_MSG_GGA:
                ubloxSetMessageRate(CLASS_NMEA_STD, MSG_NMEA_GGA, 0); // GGA: Global positioning system fix data
                break;
            case UBLOX_MSG_GSA:
                ubloxSetMessageRate(CLASS_NMEA_STD, MSG_NMEA_GSA, 0); // GSA: GNSS DOP and Active Satellites
                break;
            case UBLOX_MSG_RMC:
                ubloxSetMessageRate(CLASS_NMEA_STD, MSG_NMEA_RMC, 0); // RMC: Recommended Minimum data
                break;
            case UBLOX_ACQUIRE_MODEL:
                ubloxSendNAV5Message(gpsConfig()->gps_ublox_acquire_model);
                break;
//                   *** temporarily disabled
//                   case UBLOX_CFG_ANA:
//                      i f (gpsData.ubloxM7orAbove) { // NavX5 support existed in M5 - in theory we could remove that check
//                           ubloxSendNavX5Message();
//                       } else {
//                           gpsData.state_position++;
//                       }
//                       break;
            case UBLOX_SET_SBAS:
                ubloxSetSbas();
                break;
            case UBLOX_SET_PMS:
                if (gpsData.ubloxM8orAbove) {
                    ubloxSendPowerMode();
                } else {
                    gpsData.state_position++;
                }
                break;
            case UBLOX_MSG_NAV_PVT: //Enable NAV-PVT Messages
                if (gpsData.ubloxM9orAbove) {
                    ubloxSetMessageRateValSet(CFG_MSGOUT_UBX_NAV_PVT_UART1, 1);
                } else if (gpsData.ubloxM7orAbove) {
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_PVT, 1);
                } else {
                    gpsData.state_position++;
                }
                break;
            // if NAV-PVT is enabled, we don't need the older nav messages
            case UBLOX_MSG_SOL:
                if (gpsData.ubloxM9orAbove) {
                    // SOL is deprecated above M8
                    gpsData.state_position++;
                } else if (gpsData.ubloxM7orAbove) {
                    // use NAV-PVT, so don't use NAV-SOL
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_SOL, 0);
                } else {
                    // Only use NAV-SOL below M7
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_SOL, 1);
                }
                break;
            case UBLOX_MSG_POSLLH:
                if (gpsData.ubloxM7orAbove) {
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_POSLLH, 0);
                } else {
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_POSLLH, 1);
                }
                break;
            case UBLOX_MSG_STATUS:
                if (gpsData.ubloxM7orAbove) {
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_STATUS, 0);
                } else {
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_STATUS, 1);
                }
                break;
            case UBLOX_MSG_VELNED:
                if (gpsData.ubloxM7orAbove) {
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_VELNED, 0);
                } else {
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_VELNED, 1);
                }
                break;
            case UBLOX_MSG_DOP:
                // nav-pvt has what we need and is available M7 and above
                if (gpsData.ubloxM9orAbove) {
                    ubloxSetMessageRateValSet(CFG_MSGOUT_UBX_NAV_DOP_UART1, 0);
                } else if (gpsData.ubloxM7orAbove) {
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_DOP, 0);
                } else {
                    ubloxSetMessageRate(CLASS_NAV, MSG_NAV_DOP, 1);
                }
                break;
            case UBLOX_SAT_INFO:
                // enable by default, turned off when armed and receiving data to reduce in-flight traffic
                setSatInfoMessageRate(5);
                break;
            case UBLOX_SET_NAV_RATE:
                // set the nav solution rate to the user's configured update rate
                gpsData.updateRateHz = gpsConfig()->gps_update_rate_hz;
                ubloxSetNavRate(gpsData.updateRateHz, 1, 1);
                break;
            case UBLOX_MSG_CFG_GNSS:
                if ((gpsConfig()->sbasMode == SBAS_NONE) || (gpsConfig()->gps_ublox_use_galileo)) {
                    ubloxSendPollMessage(MSG_CFG_GNSS); // poll messages wait for ACK
                } else {
                    gpsData.state_position++;
                }
                break;
            case UBLOX_CONFIG_COMPLETE:
                gpsSetState(GPS_STATE_RECEIVING_DATA);
                break;
            // TO DO: (separate PR) add steps that remove I2C or SPI data on ValSet aware units, eg
            // ubloxSetMessageRateValSet(CFG_MSGOUT_UBX_NAV_SAT_I2C, 0);
            // ubloxSetMessageRateValSet(CFG_MSGOUT_UBX_NAV_SAT_SPI, 0);
            default:
                break;
            }
        }

        // check the ackState after changing CONFIG state, or every iteration while not UBLOX_ACK_IDLE
        switch (gpsData.ackState) {
        case UBLOX_ACK_IDLE:
            break;
        case UBLOX_ACK_WAITING:
            if (cmp32(gpsData.now, gpsData.lastMessageSent) > UBLOX_ACK_TIMEOUT_MS){
                // give up, treat it like receiving ack
                gpsData.ackState = UBLOX_ACK_GOT_ACK;
            }
            break;
        case UBLOX_ACK_GOT_ACK:
            // move forward one position, and clear the ack state
            gpsData.state_position++;
            gpsData.ackState = UBLOX_ACK_IDLE;
            break;
        case UBLOX_ACK_GOT_NACK:
            // this is the tricky bit
            // and we absolutely must get the unit type right
            if (gpsData.state_position == UBLOX_DETECT_UNIT) {
                gpsSetState(GPS_STATE_CONFIGURE);
                gpsData.ackState = UBLOX_ACK_IDLE;
            } else {
                // otherwise, for testing: just ignore nacks
                gpsData.state_position++;
                gpsData.ackState = UBLOX_ACK_IDLE;
            }
            break;
        default:
            break;
        }
    }
}
#endif // USE_GPS_UBLOX

static void gpsConfigureHardware(void)
{
    switch (gpsConfig()->provider) {
    case GPS_NMEA:
#ifdef USE_GPS_NMEA
        gpsConfigureNmea();
#endif
        break;

    case GPS_UBLOX:
#ifdef USE_GPS_UBLOX
        gpsConfigureUblox();
#endif
        break;
    default:
        break;
    }
}

static void updateGpsIndicator(timeUs_t currentTimeUs)
{
    static uint32_t GPSLEDTime;
    if (cmp32(currentTimeUs, GPSLEDTime) >= 0 && STATE(GPS_FIX) && (gpsSol.numSat >= gpsRescueConfig()->minSats)) {
        GPSLEDTime = currentTimeUs + 150000;
        LED1_TOGGLE;
    }
}

static void calculateNavInterval (void)
{
    // calculate the interval between nav packets, handling iTow wraparound at the end of the week
    const uint32_t weekDurationMs = 7 * 24 * 3600 * 1000;
    const uint32_t navDeltaTimeMs = (weekDurationMs + gpsSol.time - gpsData.lastNavSolTs) % weekDurationMs;
    gpsData.lastNavSolTs = gpsSol.time;
    // constrain the interval between 50ms / 20hz or 2.5s, when we would get a connection failure anyway
    gpsSol.navIntervalMs = constrain(navDeltaTimeMs, 50, 2500);
}

#if defined(USE_VIRTUAL_GPS)
static void updateVirtualGPS(void)
{
    const uint32_t updateInterval = 100; // 100ms 10Hz update time interval
    static uint32_t nextUpdateTime = 0;

    if (cmp32(gpsData.now, nextUpdateTime) > 0) {
        if (gpsData.state == GPS_STATE_INITIALIZED) {
            gpsSetState(GPS_STATE_RECEIVING_DATA);
        }

        getVirtualGPS(&gpsSol);
        gpsSol.time = gpsData.now;

        gpsData.lastNavMessage = gpsData.now;
        sensorsSet(SENSOR_GPS);

        if (gpsSol.numSat > 3) {
            gpsSetFixState(GPS_FIX);
        } else {
            gpsSetFixState(0);
        }
        GPS_update ^= GPS_DIRECT_TICK;

        calculateNavInterval();
        onGpsNewData();

        nextUpdateTime = gpsData.now + updateInterval;
    }
}
#endif

void gpsUpdate(timeUs_t currentTimeUs)
{
    static timeDelta_t gpsStateDurationFractionUs[GPS_STATE_COUNT];
    timeDelta_t executeTimeUs;
    gpsState_e gpsCurrentState = gpsData.state;
    gpsData.now = millis();

    switch (gpsConfig()->provider) {
    case GPS_UBLOX:
    case GPS_NMEA:
        if (!gpsPort) {
            break;
        }
        DEBUG_SET(DEBUG_GPS_CONNECTION, 7, serialRxBytesWaiting(gpsPort));
        static uint8_t wait = 0;
        static bool isFast = false;
        while (serialRxBytesWaiting(gpsPort)) {
            wait = 0;
            if (!isFast) {
                rescheduleTask(TASK_SELF, TASK_PERIOD_HZ(TASK_GPS_RATE_FAST));
                isFast = true;
            }
            if (cmpTimeUs(micros(), currentTimeUs) > GPS_RECV_TIME_MAX) {
                break;
            }
            // Add every byte to _buffer, when enough bytes are received, convert data to values
            gpsNewData(serialRead(gpsPort));
        }
        if (wait < 1) {
            wait++;
        } else if (wait == 1) {
            wait++;
            // wait one iteration be sure the buffer is empty, then reset to the slower task interval
            isFast = false;
            rescheduleTask(TASK_SELF, TASK_PERIOD_HZ(TASK_GPS_RATE));
        }
        break;

    case GPS_MSP:
        if (GPS_update & GPS_MSP_UPDATE) { // GPS data received via MSP
            if (gpsData.state == GPS_STATE_INITIALIZED) {
                gpsSetState(GPS_STATE_RECEIVING_DATA);
            }

            // Data is available
            DEBUG_SET(DEBUG_GPS_CONNECTION, 3, gpsData.now - gpsData.lastNavMessage); // interval since last Nav data was received
            gpsData.lastNavMessage = gpsData.now;
            sensorsSet(SENSOR_GPS);

            GPS_update ^= GPS_DIRECT_TICK;
            calculateNavInterval();
            onGpsNewData();

            GPS_update &= ~GPS_MSP_UPDATE;
        } else {
            DEBUG_SET(DEBUG_GPS_CONNECTION, 2, gpsData.now - gpsData.lastNavMessage); // time since last Nav data, updated each GPS task interval
            // check for no data/gps timeout/cable disconnection etc
            if (cmp32(gpsData.now, gpsData.lastNavMessage) > GPS_TIMEOUT_MS) {
                gpsSetState(GPS_STATE_LOST_COMMUNICATION);
            }
        }
        break;
#if defined(USE_VIRTUAL_GPS)
    case GPS_VIRTUAL:
        updateVirtualGPS();
        break;
#endif
    }

    switch (gpsData.state) {
    case GPS_STATE_UNKNOWN:
    case GPS_STATE_INITIALIZED:
        break;

    case GPS_STATE_DETECT_BAUD:
    case GPS_STATE_CHANGE_BAUD:
    case GPS_STATE_CONFIGURE:
        gpsConfigureHardware();
        break;

    case GPS_STATE_LOST_COMMUNICATION:
        gpsData.timeouts++;
        // previously we would attempt a different baud rate here if gps auto-baud was enabled.  that code has been removed.
        gpsSol.numSat = 0;
        DISABLE_STATE(GPS_FIX);
        gpsSetState(GPS_STATE_DETECT_BAUD);
        break;

    case GPS_STATE_RECEIVING_DATA:
#ifdef USE_GPS_UBLOX
        if (gpsConfig()->provider == GPS_UBLOX || gpsConfig()->provider == GPS_NMEA) {      // TODO  Send ublox message to nmea GPS?
            if (gpsConfig()->autoConfig == GPS_AUTOCONFIG_ON) {
                // when we are connected up, and get a 3D fix, enable the 'flight' fix model
                if (!gpsData.ubloxUsingFlightModel && STATE(GPS_FIX)) {
                    gpsData.ubloxUsingFlightModel = true;
                    ubloxSendNAV5Message(gpsConfig()->gps_ublox_flight_model);
                }
            }
        }
#endif
        DEBUG_SET(DEBUG_GPS_CONNECTION, 2, gpsData.now - gpsData.lastNavMessage); // time since last Nav data, updated each GPS task interval
        // check for no data/gps timeout/cable disconnection etc
        if (cmp32(gpsData.now, gpsData.lastNavMessage) > GPS_TIMEOUT_MS) {
            gpsSetState(GPS_STATE_LOST_COMMUNICATION);
        }
        break;
    }

    DEBUG_SET(DEBUG_GPS_CONNECTION, 4, (gpsData.state * 100 + gpsData.state_position));
    DEBUG_SET(DEBUG_GPS_CONNECTION, 6, gpsData.ackState);

    if (sensors(SENSOR_GPS)) {
        updateGpsIndicator(currentTimeUs);
    }

    static bool hasBeeped = false;
    if (!ARMING_FLAG(ARMED)) {
        if (!gpsConfig()->gps_set_home_point_once) {
        // clear the home fix icon between arms if the user configuration is to reset home point between arms
            DISABLE_STATE(GPS_FIX_HOME);
        }
        // while disarmed, beep when requirements for a home fix are met
        // ?? should we also beep if home fix requirements first appear after arming?
        if (!hasBeeped && STATE(GPS_FIX) && gpsSol.numSat >= gpsRescueConfig()->minSats) {
            beeper(BEEPER_READY_BEEP);
            hasBeeped = true;
        }
    }

    DEBUG_SET(DEBUG_GPS_DOP, 0, gpsSol.numSat);
    DEBUG_SET(DEBUG_GPS_DOP, 1, gpsSol.dop.pdop);
    DEBUG_SET(DEBUG_GPS_DOP, 2, gpsSol.dop.hdop);
    DEBUG_SET(DEBUG_GPS_DOP, 3, gpsSol.dop.vdop);

    executeTimeUs = micros() - currentTimeUs;
    if (executeTimeUs > (gpsStateDurationFractionUs[gpsCurrentState] >> GPS_TASK_DECAY_SHIFT)) {
        gpsStateDurationFractionUs[gpsCurrentState] += (2 << GPS_TASK_DECAY_SHIFT);
    } else {
        // Slowly decay the max time
        gpsStateDurationFractionUs[gpsCurrentState]--;
    }
    schedulerSetNextStateTime(gpsStateDurationFractionUs[gpsCurrentState] >> GPS_TASK_DECAY_SHIFT);

    DEBUG_SET(DEBUG_GPS_CONNECTION, 5, executeTimeUs);
//    keeping temporarily, to be used when debugging the scheduler stuff
//    DEBUG_SET(DEBUG_GPS_CONNECTION, 6, (gpsStateDurationFractionUs[gpsCurrentState] >> GPS_TASK_DECAY_SHIFT));
}

static void gpsNewData(uint16_t c)
{
    DEBUG_SET(DEBUG_GPS_CONNECTION, 1, gpsSol.navIntervalMs);
    if (!gpsNewFrame(c)) {
        // no new nav solution data
        return;
    }
    if (gpsData.state == GPS_STATE_RECEIVING_DATA) {
        DEBUG_SET(DEBUG_GPS_CONNECTION, 3, gpsData.now - gpsData.lastNavMessage); // interval since last Nav data was received
        gpsData.lastNavMessage = gpsData.now;
        sensorsSet(SENSOR_GPS);
        // use the baud rate debug once receiving data
    }
    GPS_update ^= GPS_DIRECT_TICK;
    onGpsNewData();
}

#ifdef USE_GPS_UBLOX
static ubloxVersion_e ubloxParseVersion(const uint32_t version)
{
    for (size_t i = 0; i < ARRAYLEN(ubloxVersionMap); ++i) {
        if (version == ubloxVersionMap[i].hw) {
            return (ubloxVersion_e) i;
        }
    }

    return UBX_VERSION_UNDEF;// (ubloxVersion_e) version;
}
#endif

bool gpsNewFrame(uint8_t c)
{
    switch (gpsConfig()->provider) {
    case GPS_NMEA:          // NMEA
#ifdef USE_GPS_NMEA
        return gpsNewFrameNMEA(c);
#endif
        break;
    case GPS_UBLOX:         // UBX binary
#ifdef USE_GPS_UBLOX
        return gpsNewFrameUBLOX(c);
#endif
        break;
    default:
        break;
    }
    return false;
}

// Check for healthy communications
bool gpsIsHealthy(void)
{
    return (gpsData.state == GPS_STATE_RECEIVING_DATA);
}

/* This is a light implementation of a GPS frame decoding
   This should work with most of modern GPS devices configured to output 5 frames.
   It assumes there are some NMEA GGA frames to decode on the serial bus
   Now verifies checksum correctly before applying data

   Here we use only the following data :
     - latitude
     - longitude
     - GPS fix is/is not ok
     - GPS num sat (4 is enough to be +/- reliable)
     // added by Mis
     - GPS altitude (for OSD displaying)
     - GPS speed (for OSD displaying)
*/

#define NO_FRAME   0
#define FRAME_GGA  1
#define FRAME_RMC  2
#define FRAME_GSV  3
#define FRAME_GSA  4

// This code is used for parsing NMEA data

/* Alex optimization
  The latitude or longitude is coded this way in NMEA frames
  dm.f   coded as degrees + minutes + minute decimal
  Where:
    - d can be 1 or more char long. generally: 2 char long for latitude, 3 char long for longitude
    - m is always 2 char long
    - f can be 1 or more char long
  This function converts this format in a unique unsigned long where 1 degree = 10 000 000

  EOS increased the precision here, even if we think that the gps is not precise enough, with 10e5 precision it has 76cm resolution
  with 10e7 it's around 1 cm now. Increasing it further is irrelevant, since even 1cm resolution is unrealistic, however increased
  resolution also increased precision of nav calculations
static uint32_t GPS_coord_to_degrees(char *coordinateString)
{
    char *p = s, *d = s;
    uint8_t min, deg = 0;
    uint16_t frac = 0, mult = 10000;

    while (*p) {                // parse the string until its end
        if (d != s) {
            frac += (*p - '0') * mult;  // calculate only fractional part on up to 5 digits  (d != s condition is true when the . is located)
            mult /= 10;
        }
        if (*p == '.')
            d = p;              // locate '.' char in the string
        p++;
    }
    if (p == s)
        return 0;
    while (s < d - 2) {
        deg *= 10;              // convert degrees : all chars before minutes ; for the first iteration, deg = 0
        deg += *(s++) - '0';
    }
    min = *(d - 1) - '0' + (*(d - 2) - '0') * 10;       // convert minutes : 2 previous char before '.'
    return deg * 10000000UL + (min * 100000UL + frac) * 10UL / 6;
}
*/

// helper functions
#ifdef USE_GPS_NMEA
static uint32_t grab_fields(char *src, uint8_t mult)
{                               // convert string to uint32
    uint32_t i;
    uint32_t tmp = 0;
    int isneg = 0;
    for (i = 0; src[i] != 0; i++) {
        if ((i == 0) && (src[0] == '-')) { // detect negative sign
            isneg = 1;
            continue; // jump to next character if the first one was a negative sign
        }
        if (src[i] == '.') {
            i++;
            if (mult == 0) {
                break;
            } else {
                src[i + mult] = 0;
            }
        }
        tmp *= 10;
        if (src[i] >= '0' && src[i] <= '9') {
            tmp += src[i] - '0';
        }
        if (i >= 15) {
            return 0; // out of bounds
        }
    }
    return isneg ? -tmp : tmp;    // handle negative altitudes
}

typedef struct gpsDataNmea_s {
    int32_t latitude;
    int32_t longitude;
    uint8_t numSat;
    int32_t altitudeCm;
    uint16_t speed;
    uint16_t pdop;
    uint16_t hdop;
    uint16_t vdop;
    uint16_t ground_course;
    uint32_t time;
    uint32_t date;
} gpsDataNmea_t;

static void parseFieldNmea(gpsDataNmea_t *data, char *str, uint8_t gpsFrame, uint8_t idx)
{
    static uint8_t svMessageNum = 0;
    uint8_t svSatNum = 0, svPacketIdx = 0, svSatParam = 0;

    switch (gpsFrame) {

    case FRAME_GGA:        //************* GPGGA FRAME parsing
        switch (idx) {
        case 1:
            data->time = ((uint8_t)(str[5] - '0') * 10 + (uint8_t)(str[7] - '0')) * 100;
            break;
        case 2:
            data->latitude = GPS_coord_to_degrees(str);
            break;
        case 3:
            if (str[0] == 'S') data->latitude *= -1;
            break;
        case 4:
            data->longitude = GPS_coord_to_degrees(str);
            break;
        case 5:
            if (str[0] == 'W') data->longitude *= -1;
            break;
        case 6:
            gpsSetFixState(str[0] > '0');
            break;
        case 7:
            data->numSat = grab_fields(str, 0);
            break;
        case 9:
            data->altitudeCm = grab_fields(str, 1) * 10;     // altitude in centimeters. Note: NMEA delivers altitude with 1 or 3 decimals. It's safer to cut at 0.1m and multiply by 10
            break;
        }
        break;

    case FRAME_RMC:        //************* GPRMC FRAME parsing
        switch (idx) {
        case 1:
            data->time = grab_fields(str, 2); // UTC time hhmmss.ss
            break;
        case 7:
            data->speed = ((grab_fields(str, 1) * 5144L) / 1000L);    // speed in cm/s added by Mis
            break;
        case 8:
            data->ground_course = (grab_fields(str, 1));      // ground course deg * 10
            break;
        case 9:
            data->date = grab_fields(str, 0); // date dd/mm/yy
            break;
        }
        break;

    case FRAME_GSV:
        switch (idx) {
        /*case 1:
            // Total number of messages of this type in this cycle
            break; */
        case 2:
            // Message number
            svMessageNum = grab_fields(str, 0);
            break;
        case 3:
            // Total number of SVs visible
            GPS_numCh = MIN(grab_fields(str, 0), GPS_SV_MAXSATS_LEGACY);
            break;
        }
        if (idx < 4)
            break;

        svPacketIdx = (idx - 4) / 4 + 1; // satellite number in packet, 1-4
        svSatNum    = svPacketIdx + (4 * (svMessageNum - 1)); // global satellite number
        svSatParam  = idx - 3 - (4 * (svPacketIdx - 1)); // parameter number for satellite

        if (svSatNum > GPS_SV_MAXSATS_LEGACY)
            break;

        switch (svSatParam) {
        case 1:
            // SV PRN number
            GPS_svinfo[svSatNum - 1].chn  = svSatNum;
            GPS_svinfo[svSatNum - 1].svid = grab_fields(str, 0);
            break;
        /*case 2:
            // Elevation, in degrees, 90 maximum
            break;
        case 3:
            // Azimuth, degrees from True North, 000 through 359
            break; */
        case 4:
            // SNR, 00 through 99 dB (null when not tracking)
            GPS_svinfo[svSatNum - 1].cno = grab_fields(str, 0);
            GPS_svinfo[svSatNum - 1].quality = 0; // only used by ublox
            break;
        }

#ifdef USE_DASHBOARD
        dashboardGpsNavSvInfoRcvCount++;
#endif
        break;

    case FRAME_GSA:
        switch (idx) {
        case 15:
            data->pdop = grab_fields(str, 2);  // pDOP * 100
            break;
        case 16:
            data->hdop = grab_fields(str, 2);  // hDOP * 100
            break;
        case 17:
            data->vdop = grab_fields(str, 2);  // vDOP * 100
            break;
        }
    break;
    }
}

static bool writeGpsSolutionNmea(gpsSolutionData_t *sol, const gpsDataNmea_t *data, uint8_t gpsFrame)
{
    int navDeltaTimeMs = 100;
    const uint32_t msInTenSeconds = 10000;
    switch (gpsFrame) {

    case FRAME_GGA:
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_NMEA_GGA;
#endif
        if (STATE(GPS_FIX)) {
            sol->llh.lat = data->latitude;
            sol->llh.lon = data->longitude;
            sol->numSat = data->numSat;
            sol->llh.altCm = data->altitudeCm;
        }
        navDeltaTimeMs = (msInTenSeconds + data->time - gpsData.lastNavSolTs) % msInTenSeconds;
        gpsData.lastNavSolTs = data->time;
        sol->navIntervalMs = constrain(navDeltaTimeMs, 50, 2500);
        // return only one true statement to trigger one "newGpsDataReady" flag per GPS loop
        return true;

    case FRAME_GSA:
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_NMEA_GSA;
#endif
        sol->dop.pdop = data->pdop;
        sol->dop.hdop = data->hdop;
        sol->dop.vdop = data->vdop;
        return false;

    case FRAME_RMC:
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_NMEA_RMC;
#endif
        sol->groundSpeed = data->speed;
        sol->groundCourse = data->ground_course;
#ifdef USE_RTC_TIME
        // This check will miss 00:00:00.00, but we shouldn't care - next report will be valid
        if (!rtcHasTime() && data->date != 0 && data->time != 0) {
            dateTime_t temp_time;
            temp_time.year = (data->date % 100) + 2000;
            temp_time.month = (data->date / 100) % 100;
            temp_time.day = (data->date / 10000) % 100;
            temp_time.hours = (data->time / 1000000) % 100;
            temp_time.minutes = (data->time / 10000) % 100;
            temp_time.seconds = (data->time / 100) % 100;
            temp_time.millis = (data->time & 100) * 10;
            rtcSetDateTime(&temp_time);
        }
#endif
        return false;

    default:
        return false;
    }
}

static bool gpsNewFrameNMEA(char c)
{
    static gpsDataNmea_t gps_msg;
    static char string[15];
    static uint8_t param = 0, offset = 0, parity = 0;
    static uint8_t checksum_param, gps_frame = NO_FRAME;
    bool receivedNavMessage = false;

    switch (c) {

    case '$':
        param = 0;
        offset = 0;
        parity = 0;
        break;

    case ',':
    case '*':
        string[offset] = 0;
        if (param == 0) {  // frame identification (5 chars, e.g. "GPGGA", "GNGGA", "GLGGA", ...)
            gps_frame = NO_FRAME;
            if (strcmp(&string[2], "GGA") == 0) {
                gps_frame = FRAME_GGA;
            } else if (strcmp(&string[2], "RMC") == 0) {
                gps_frame = FRAME_RMC;
            } else if (strcmp(&string[2], "GSV") == 0) {
                gps_frame = FRAME_GSV;
            } else if (strcmp(&string[2], "GSA") == 0) {
                gps_frame = FRAME_GSA;
            }
        }

        // parse string and write data into gps_msg
        parseFieldNmea(&gps_msg, string, gps_frame, param);

        param++;
        offset = 0;
        if (c == '*')
            checksum_param = 1;
        else
            parity ^= c;
        break;

    case '\r':
    case '\n':
        if (checksum_param) {   //parity checksum
#ifdef USE_DASHBOARD
            shiftPacketLog();
#endif
            uint8_t checksum = 16 * ((string[0] >= 'A') ? string[0] - 'A' + 10 : string[0] - '0') + ((string[1] >= 'A') ? string[1] - 'A' + 10 : string[1] - '0');
            if (checksum == parity) {
#ifdef USE_DASHBOARD
                *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_IGNORED;
                dashboardGpsPacketCount++;
#endif
                receivedNavMessage = writeGpsSolutionNmea(&gpsSol, &gps_msg, gps_frame);  // // write gps_msg into gpsSol
            }
#ifdef USE_DASHBOARD
            else {
                *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_ERROR;
            }
#endif
        }
        checksum_param = 0;
        break;

    default:
        if (offset < 15)
            string[offset++] = c;
        if (!checksum_param)
            parity ^= c;
        break;
    }

    return receivedNavMessage;
}
#endif // USE_GPS_NMEA

#ifdef USE_GPS_UBLOX
// UBX support
typedef struct ubxNavPosllh_s {
    uint32_t time;              // GPS msToW
    int32_t longitude;
    int32_t latitude;
    int32_t altitude_ellipsoid;
    int32_t altitudeMslMm;
    uint32_t horizontal_accuracy;
    uint32_t vertical_accuracy;
} ubxNavPosllh_t;

typedef struct ubxNavStatus_s {
    uint32_t time;              // GPS msToW
    uint8_t fix_type;
    uint8_t fix_status;
    uint8_t differential_status;
    uint8_t res;
    uint32_t time_to_first_fix;
    uint32_t uptime;            // milliseconds
} ubxNavStatus_t;

typedef struct ubxNavDop_s {
    uint32_t itow;              // GPS Millisecond Time of Week
    uint16_t gdop;              // Geometric DOP
    uint16_t pdop;              // Position DOP
    uint16_t tdop;              // Time DOP
    uint16_t vdop;              // Vertical DOP
    uint16_t hdop;              // Horizontal DOP
    uint16_t ndop;              // Northing DOP
    uint16_t edop;              // Easting DOP
} ubxNavDop_t;

typedef struct ubxNavSol_s {
    uint32_t time;
    int32_t time_nsec;
    int16_t week;
    uint8_t fix_type;
    uint8_t fix_status;
    int32_t ecef_x;
    int32_t ecef_y;
    int32_t ecef_z;
    uint32_t position_accuracy_3d;
    int32_t ecef_x_velocity;
    int32_t ecef_y_velocity;
    int32_t ecef_z_velocity;
    uint32_t speed_accuracy;
    uint16_t position_DOP;
    uint8_t res;
    uint8_t satellites;
    uint32_t res2;
} ubxNavSol_t;

typedef struct ubxNavPvt_s {
    uint32_t time;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
    uint8_t valid;
    uint32_t tAcc;
    int32_t nano;
    uint8_t fixType;
    uint8_t flags;
    uint8_t flags2;
    uint8_t numSV;
    int32_t lon;
    int32_t lat;
    int32_t height;
    int32_t hMSL;
    uint32_t hAcc;
    uint32_t vAcc;
    int32_t velN;
    int32_t velE;
    int32_t velD;
    int32_t gSpeed;
    int32_t headMot;
    uint32_t sAcc;
    uint32_t headAcc;
    uint16_t pDOP;
    uint8_t flags3;
    uint8_t reserved0[5];
    int32_t headVeh;
    int16_t magDec;
    uint16_t magAcc;
} ubxNavPvt_t;

typedef struct ubxNavVelned_s {
    uint32_t time;              // GPS msToW
    int32_t ned_north;
    int32_t ned_east;
    int32_t ned_down;
    uint32_t speed_3d;
    uint32_t speed_2d;
    int32_t heading_2d;
    uint32_t speed_accuracy;
    uint32_t heading_accuracy;
} ubxNavVelned_t;

typedef struct ubxNavSvinfoChannel_s {
    uint8_t chn;                // Channel number, 255 for SVx not assigned to channel
    uint8_t svid;               // Satellite ID
    uint8_t flags;              // Bitmask
    uint8_t quality;            // Bitfield
    uint8_t cno;                // Carrier to Noise Ratio (Signal Strength) // dbHz, 0-55.
    uint8_t elev;               // Elevation in integer degrees
    int16_t azim;               // Azimuth in integer degrees
    int32_t prRes;              // Pseudo range residual in centimetres
} ubxNavSvinfoChannel_t;

typedef struct ubxNavSatSv_s {
    uint8_t gnssId;
    uint8_t svId;               // Satellite ID
    uint8_t cno;                // Carrier to Noise Ratio (Signal Strength) // dbHz, 0-55.
    int8_t elev;                // Elevation in integer degrees
    int16_t azim;               // Azimuth in integer degrees
    int16_t prRes;              // Pseudo range residual in decimetres
    uint32_t flags;             // Bitmask
} ubxNavSatSv_t;

typedef struct ubxNavSvinfo_s {
    uint32_t time;              // GPS Millisecond time of week
    uint8_t numCh;              // Number of channels
    uint8_t globalFlags;        // Bitmask, Chip hardware generation 0:Antaris, 1:u-blox 5, 2:u-blox 6
    uint16_t reserved2;         // Reserved
    ubxNavSvinfoChannel_t channel[GPS_SV_MAXSATS_M8N]; // 32 satellites * 12 bytes
} ubxNavSvinfo_t;

typedef struct ubxNavSat_s {
    uint32_t time;              // GPS Millisecond time of week
    uint8_t version;
    uint8_t numSvs;
    uint8_t reserved0[2];
    ubxNavSatSv_t svs[GPS_SV_MAXSATS_M8N];
} ubxNavSat_t;

typedef struct ubxAck_s {
    uint8_t clsId;               // Class ID of the acknowledged message
    uint8_t msgId;               // Message ID of the acknowledged message
} ubxAck_t;

typedef enum {
    FIX_NONE = 0,
    FIX_DEAD_RECKONING = 1,
    FIX_2D = 2,
    FIX_3D = 3,
    FIX_GPS_DEAD_RECKONING = 4,
    FIX_TIME = 5
} ubsNavFixType_e;

typedef enum {
    NAV_STATUS_FIX_VALID = 1,
    NAV_STATUS_TIME_WEEK_VALID = 4,
    NAV_STATUS_TIME_SECOND_VALID = 8
} ubxNavStatusBits_e;

typedef enum {
    NAV_VALID_DATE = 1,
    NAV_VALID_TIME = 2
} ubxNavPvtValid_e;

// Do we have a new valid fix?
static bool ubxHaveNewValidFix = false;

// Do we have new position information?
static bool ubxHaveNewPosition = false;

// Do we have new speed information?
static bool ubxHaveNewSpeed = false;

// From the UBX protocol docs, the largest payload we receive is NAV-SAT, which
// is calculated as 8 + 12*numCh. Max reported sats can be up to 56.
// We're using the max for M8 (32) for our sizing, since Configurator only
// supports a max of 32 sats and we want to limit the payload buffer space used.
#define UBLOX_PAYLOAD_SIZE (8 + 12 * GPS_SV_MAXSATS_M8N)
#define UBLOX_MAX_PAYLOAD_SANITY_SIZE 776   // Any returned payload length greater than a 64 sat NAV-SAT is considered unreasonable, and probably corrupted data.

// Received message frame fields.
// - Preamble sync character 1 & 2 are not saved, only detected for parsing.
// - Message class & message ID indicate the type of message receieved.
static uint8_t ubxRcvMsgClass;
static uint8_t ubxRcvMsgID;
// - Payload length assembled from the length LSB & MSB bytes.
static uint16_t ubxRcvMsgPayloadLength;
// - Payload, each message type has its own payload field layout, represented by the elements of this union.
//   Note that the size of the buffer is determined by the longest possible payload, currently UBX-NAV-SAT.
//   See size define comments above. Warning, this is fragile! If another message type becomes the largest
//   payload instead of UBX-NAV-SAT, UBLOX_PAYLOAD_SIZE above needs to be adjusted!
static union {
    ubxNavPosllh_t ubxNavPosllh;
    ubxNavStatus_t ubxNavStatus;
    ubxNavDop_t ubxNavDop;
    ubxNavSol_t ubxNavSol;
    ubxNavVelned_t ubxNavVelned;
    ubxNavPvt_t ubxNavPvt;
    ubxNavSvinfo_t ubxNavSvinfo;
    ubxNavSat_t ubxNavSat;
    ubxCfgGnss_t ubxCfgGnss;
    ubxMonVer_t ubxMonVer;
    ubxAck_t ubxAck;
    uint8_t rawBytes[UBLOX_PAYLOAD_SIZE];  // Used for adding raw bytes to the payload. WARNING: This byte array must be as large as the largest payload for any message type above!
} ubxRcvMsgPayload;
// - Checksum A & B. Uses the 8-bit Fletcher algorithm (TCP standard RFC 1145).
static uint8_t ubxRcvMsgChecksumA;
static uint8_t ubxRcvMsgChecksumB;

// Message frame parsing state machine control.
typedef enum {
    UBX_PARSE_PREAMBLE_SYNC_1,
    UBX_PARSE_PREAMBLE_SYNC_2,
    UBX_PARSE_MESSAGE_CLASS,
    UBX_PARSE_MESSAGE_ID,
    UBX_PARSE_PAYLOAD_LENGTH_LSB,
    UBX_PARSE_PAYLOAD_LENGTH_MSB,
    UBX_PARSE_PAYLOAD_CONTENT,
    UBX_PARSE_CHECKSUM_A,
    UBX_PARSE_CHECKSUM_B
} ubxFrameParseState_e;
static ubxFrameParseState_e ubxFrameParseState = UBX_PARSE_PREAMBLE_SYNC_1;
static uint16_t ubxFrameParsePayloadCounter;

// SCEDEBUG To help debug which message is slow to process
// static uint8_t lastUbxRcvMsgClass;
// static uint8_t lastUbxRcvMsgID;

// Combines message class & ID for a single value to switch on.
#define CLSMSG(cls, msg) (((cls) << 8) | (msg))

static bool UBLOX_parse_gps(void)
{
//    lastUbxRcvMsgClass = ubxRcvMsgClass;
//    lastUbxRcvMsgID = ubxRcvMsgID;

#ifdef USE_DASHBOARD
    *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_IGNORED;
#endif
    switch (CLSMSG(ubxRcvMsgClass, ubxRcvMsgID)) {

    case CLSMSG(CLASS_MON, MSG_MON_VER):
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_UBLOX_MONVER;
#endif
        gpsData.platformVersion = ubloxParseVersion(strtoul(ubxRcvMsgPayload.ubxMonVer.hwVersion, NULL, 16));
        gpsData.ubloxM7orAbove = gpsData.platformVersion >= UBX_VERSION_M7;
        gpsData.ubloxM8orAbove = gpsData.platformVersion >= UBX_VERSION_M8;
        gpsData.ubloxM9orAbove = gpsData.platformVersion >= UBX_VERSION_M9;
        break;
    case CLSMSG(CLASS_NAV, MSG_NAV_POSLLH):
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_UBLOX_POSLLH;
#endif
        //i2c_dataset.time                = _buffer.ubxNavPosllh.time;
        gpsSol.llh.lon = ubxRcvMsgPayload.ubxNavPosllh.longitude;
        gpsSol.llh.lat = ubxRcvMsgPayload.ubxNavPosllh.latitude;
        gpsSol.llh.altCm = ubxRcvMsgPayload.ubxNavPosllh.altitudeMslMm / 10;  //alt in cm
        gpsSol.time = ubxRcvMsgPayload.ubxNavPosllh.time;
        calculateNavInterval();
        gpsSetFixState(ubxHaveNewValidFix);
        ubxHaveNewPosition = true;
        break;
    case CLSMSG(CLASS_NAV, MSG_NAV_STATUS):
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_UBLOX_STATUS;
#endif
        ubxHaveNewValidFix = (ubxRcvMsgPayload.ubxNavStatus.fix_status & NAV_STATUS_FIX_VALID) && (ubxRcvMsgPayload.ubxNavStatus.fix_type == FIX_3D);
        if (!ubxHaveNewValidFix)
            DISABLE_STATE(GPS_FIX);
        break;
    case CLSMSG(CLASS_NAV, MSG_NAV_DOP):
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_UBLOX_DOP;
#endif
        gpsSol.dop.pdop = ubxRcvMsgPayload.ubxNavDop.pdop;
        gpsSol.dop.hdop = ubxRcvMsgPayload.ubxNavDop.hdop;
        gpsSol.dop.vdop = ubxRcvMsgPayload.ubxNavDop.vdop;
        break;
    case CLSMSG(CLASS_NAV, MSG_NAV_SOL):
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_UBLOX_SOL;
#endif
        ubxHaveNewValidFix = (ubxRcvMsgPayload.ubxNavSol.fix_status & NAV_STATUS_FIX_VALID) && (ubxRcvMsgPayload.ubxNavSol.fix_type == FIX_3D);
        if (!ubxHaveNewValidFix)
            DISABLE_STATE(GPS_FIX);
        gpsSol.numSat = ubxRcvMsgPayload.ubxNavSol.satellites;
#ifdef USE_RTC_TIME
        //set clock, when gps time is available
        if (!rtcHasTime() && (ubxRcvMsgPayload.ubxNavSol.fix_status & NAV_STATUS_TIME_SECOND_VALID) && (ubxRcvMsgPayload.ubxNavSol.fix_status & NAV_STATUS_TIME_WEEK_VALID)) {
            //calculate rtctime: week number * ms in a week + ms of week + fractions of second + offset to UNIX reference year - 18 leap seconds
            rtcTime_t temp_time = (((int64_t) ubxRcvMsgPayload.ubxNavSol.week) * 7 * 24 * 60 * 60 * 1000) + ubxRcvMsgPayload.ubxNavSol.time + (ubxRcvMsgPayload.ubxNavSol.time_nsec / 1000000) + 315964800000LL - 18000;
            rtcSet(&temp_time);
        }
#endif
        break;
    case CLSMSG(CLASS_NAV, MSG_NAV_VELNED):
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_UBLOX_VELNED;
#endif
        gpsSol.speed3d = ubxRcvMsgPayload.ubxNavVelned.speed_3d;       // cm/s
        gpsSol.groundSpeed = ubxRcvMsgPayload.ubxNavVelned.speed_2d;   // cm/s
        gpsSol.groundCourse = (uint16_t) (ubxRcvMsgPayload.ubxNavVelned.heading_2d / 10000);     // Heading 2D deg * 100000 rescaled to deg * 10
        ubxHaveNewSpeed = true;
        break;
    case CLSMSG(CLASS_NAV, MSG_NAV_PVT):
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_UBLOX_SOL;
#endif
        ubxHaveNewValidFix = (ubxRcvMsgPayload.ubxNavPvt.flags & NAV_STATUS_FIX_VALID) && (ubxRcvMsgPayload.ubxNavPvt.fixType == FIX_3D);
        gpsSol.time = ubxRcvMsgPayload.ubxNavPvt.time;
        calculateNavInterval();
        gpsSol.llh.lon = ubxRcvMsgPayload.ubxNavPvt.lon;
        gpsSol.llh.lat = ubxRcvMsgPayload.ubxNavPvt.lat;
        gpsSol.llh.altCm = ubxRcvMsgPayload.ubxNavPvt.hMSL / 10;  //alt in cm
        gpsSetFixState(ubxHaveNewValidFix);
        ubxHaveNewPosition = true;
        gpsSol.numSat = ubxRcvMsgPayload.ubxNavPvt.numSV;
        gpsSol.acc.hAcc = ubxRcvMsgPayload.ubxNavPvt.hAcc;
        gpsSol.acc.vAcc = ubxRcvMsgPayload.ubxNavPvt.vAcc;
        gpsSol.acc.sAcc = ubxRcvMsgPayload.ubxNavPvt.sAcc;
        gpsSol.speed3d = (uint16_t) sqrtf(powf(ubxRcvMsgPayload.ubxNavPvt.gSpeed / 10, 2.0f) + powf(ubxRcvMsgPayload.ubxNavPvt.velD / 10, 2.0f));
        gpsSol.groundSpeed = ubxRcvMsgPayload.ubxNavPvt.gSpeed / 10;    // cm/s
        gpsSol.groundCourse = (uint16_t) (ubxRcvMsgPayload.ubxNavPvt.headMot / 10000);     // Heading 2D deg * 100000 rescaled to deg * 10
        gpsSol.dop.pdop = ubxRcvMsgPayload.ubxNavPvt.pDOP;
        ubxHaveNewSpeed = true;
#ifdef USE_RTC_TIME
        //set clock, when gps time is available
        if (!rtcHasTime() && (ubxRcvMsgPayload.ubxNavPvt.valid & NAV_VALID_DATE) && (ubxRcvMsgPayload.ubxNavPvt.valid & NAV_VALID_TIME)) {
            dateTime_t dt;
            dt.year = ubxRcvMsgPayload.ubxNavPvt.year;
            dt.month = ubxRcvMsgPayload.ubxNavPvt.month;
            dt.day = ubxRcvMsgPayload.ubxNavPvt.day;
            dt.hours = ubxRcvMsgPayload.ubxNavPvt.hour;
            dt.minutes = ubxRcvMsgPayload.ubxNavPvt.min;
            dt.seconds = ubxRcvMsgPayload.ubxNavPvt.sec;
            dt.millis = (ubxRcvMsgPayload.ubxNavPvt.nano > 0) ? ubxRcvMsgPayload.ubxNavPvt.nano / 1000000 : 0; // up to 5ms of error
            rtcSetDateTime(&dt);
        }
#endif
        break;
    case CLSMSG(CLASS_NAV, MSG_NAV_SVINFO):
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_UBLOX_SVINFO;
#endif
        GPS_numCh = MIN(ubxRcvMsgPayload.ubxNavSvinfo.numCh, GPS_SV_MAXSATS_LEGACY);
        // If we're receiving UBX-NAV-SVINFO messages, we detected a module version M7 or older.
        // We can receive far more sats than we can handle for Configurator, which is the primary consumer for sat info.
        // We're using the max for legacy (16) for our sizing, this smaller sizing triggers Configurator to know it's
        // an M7 or earlier module and to use the older sat list format.
        // We simply ignore any sats above that max, the down side is we may not see sats used for the solution, but
        // the intent in Configurator is to see if sats are being acquired and their strength, so this is not an issue.
        for (unsigned i = 0; i < ARRAYLEN(GPS_svinfo); i++) {
            if (i < GPS_numCh) {
                GPS_svinfo[i].chn = ubxRcvMsgPayload.ubxNavSvinfo.channel[i].chn;
                GPS_svinfo[i].svid = ubxRcvMsgPayload.ubxNavSvinfo.channel[i].svid;
                GPS_svinfo[i].quality = ubxRcvMsgPayload.ubxNavSvinfo.channel[i].quality;
                GPS_svinfo[i].cno = ubxRcvMsgPayload.ubxNavSvinfo.channel[i].cno;
            } else {
                GPS_svinfo[i] = (GPS_svinfo_t){0};
            }
        }
#ifdef USE_DASHBOARD
        dashboardGpsNavSvInfoRcvCount++;
#endif
        break;
    case CLSMSG(CLASS_NAV, MSG_NAV_SAT):
#ifdef USE_DASHBOARD
        *dashboardGpsPacketLogCurrentChar = DASHBOARD_LOG_UBLOX_SVINFO; // The display log only shows SVINFO for both SVINFO and SAT.
#endif
        GPS_numCh = MIN(ubxRcvMsgPayload.ubxNavSat.numSvs, GPS_SV_MAXSATS_M8N);
        // If we're receiving UBX-NAV-SAT messages, we detected a module M8 or newer.
        // We can receive far more sats than we can handle for Configurator, which is the primary consumer for sat info.
        // We're using the max for M8 (32) for our sizing, since Configurator only supports a max of 32 sats and we
        // want to limit the payload buffer space used.
        // We simply ignore any sats above that max, the down side is we may not see sats used for the solution, but
        // the intent in Configurator is to see if sats are being acquired and their strength, so this is not an issue.
        for (unsigned i = 0; i < ARRAYLEN(GPS_svinfo); i++) {
            if (i < GPS_numCh) {
                GPS_svinfo[i].chn = ubxRcvMsgPayload.ubxNavSat.svs[i].gnssId;
                GPS_svinfo[i].svid = ubxRcvMsgPayload.ubxNavSat.svs[i].svId;
                GPS_svinfo[i].cno = ubxRcvMsgPayload.ubxNavSat.svs[i].cno;
                GPS_svinfo[i].quality = ubxRcvMsgPayload.ubxNavSat.svs[i].flags;
            } else {
                GPS_svinfo[i] = (GPS_svinfo_t){ .chn = 255 };
            }
        }

        // Setting the number of channels higher than GPS_SV_MAXSATS_LEGACY is the only way to tell BF Configurator we're sending the
        // enhanced sat list info without changing the MSP protocol. Also, we're sending the complete list each time even if it's empty, so
        // BF Conf can erase old entries shown on screen when channels are removed from the list.
        // TODO: GPS_numCh = MAX(GPS_numCh, GPS_SV_MAXSATS_LEGACY + 1);
        GPS_numCh = GPS_SV_MAXSATS_M8N;
#ifdef USE_DASHBOARD
        dashboardGpsNavSvInfoRcvCount++;
#endif
        break;
    case CLSMSG(CLASS_CFG, MSG_CFG_GNSS):
        {
            const uint16_t messageSize = 4 + (ubxRcvMsgPayload.ubxCfgGnss.numConfigBlocks * sizeof(ubxConfigblock_t));
            ubxMessage_t tx_buffer;

            // prevent buffer overflow on invalid numConfigBlocks
            const int size = MIN(messageSize, sizeof(tx_buffer.payload));
            memcpy(&tx_buffer.payload, &ubxRcvMsgPayload, size);

            for (int i = 0; i < ubxRcvMsgPayload.ubxCfgGnss.numConfigBlocks; i++) {
                if (ubxRcvMsgPayload.ubxCfgGnss.configblocks[i].gnssId == UBLOX_GNSS_SBAS) {
                    if (gpsConfig()->sbasMode == SBAS_NONE) {
                        tx_buffer.payload.cfg_gnss.configblocks[i].flags &= ~UBLOX_GNSS_ENABLE; // Disable SBAS
                    }
                }

                if (ubxRcvMsgPayload.ubxCfgGnss.configblocks[i].gnssId == UBLOX_GNSS_GALILEO) {
                    if (gpsConfig()->gps_ublox_use_galileo) {
                        tx_buffer.payload.cfg_gnss.configblocks[i].flags |= UBLOX_GNSS_ENABLE; // Enable Galileo
                    } else {
                        tx_buffer.payload.cfg_gnss.configblocks[i].flags &= ~UBLOX_GNSS_ENABLE; // Disable Galileo
                    }
                }
            }

            ubloxSendConfigMessage(&tx_buffer, MSG_CFG_GNSS, messageSize, false);
        }
        break;
    case CLSMSG(CLASS_ACK, MSG_ACK_ACK):
        if ((gpsData.ackState == UBLOX_ACK_WAITING) && (ubxRcvMsgPayload.ubxAck.msgId == gpsData.ackWaitingMsgId)) {
            gpsData.ackState = UBLOX_ACK_GOT_ACK;
        }
        break;
    case CLSMSG(CLASS_ACK, MSG_ACK_NACK):
        if ((gpsData.ackState == UBLOX_ACK_WAITING) && (ubxRcvMsgPayload.ubxAck.msgId == gpsData.ackWaitingMsgId)) {
            gpsData.ackState = UBLOX_ACK_GOT_NACK;
        }
        break;

    default:
        return false;
    }
#undef CLSMSG

    // we only return true when we get new position and speed data
    // this ensures we don't use stale data
    if (ubxHaveNewPosition && ubxHaveNewSpeed) {
        ubxHaveNewSpeed = ubxHaveNewPosition = false;
        return true;
    }
    return false;
}

static bool gpsNewFrameUBLOX(uint8_t data)
{
    bool newPositionDataReceived = false;

    switch (ubxFrameParseState) {
    case UBX_PARSE_PREAMBLE_SYNC_1:
        if (PREAMBLE1 == data) {
            // Might be a new UBX message, go on to look for next preamble byte.
            ubxFrameParseState = UBX_PARSE_PREAMBLE_SYNC_2;
            break;
        }
        // Not a new UBX message, stay in this state for the next incoming byte.
        break;
    case UBX_PARSE_PREAMBLE_SYNC_2:
        if (PREAMBLE2 == data) {
            // Matches the two-byte preamble, seems to be a legit message, go on to process the rest of the message.
            ubxFrameParseState = UBX_PARSE_MESSAGE_CLASS;
            break;
        }
        // False start, if this byte is not a preamble 1, restart new message parsing.
        // If this byte is a preamble 1, we might have gotten two in a row, so stay here and look for preamble 2 again.
        if (PREAMBLE1 != data) {
            ubxFrameParseState = UBX_PARSE_PREAMBLE_SYNC_1;
        }
        break;
    case UBX_PARSE_MESSAGE_CLASS:
        ubxRcvMsgChecksumB = ubxRcvMsgChecksumA = data;   // Reset & start the checksum A & B accumulators.
        ubxRcvMsgClass = data;
        ubxFrameParseState = UBX_PARSE_MESSAGE_ID;
        break;
    case UBX_PARSE_MESSAGE_ID:
        ubxRcvMsgChecksumB += (ubxRcvMsgChecksumA += data);   // Accumulate both checksums.
        ubxRcvMsgID = data;
        ubxFrameParseState = UBX_PARSE_PAYLOAD_LENGTH_LSB;
        break;
    case UBX_PARSE_PAYLOAD_LENGTH_LSB:
        ubxRcvMsgChecksumB += (ubxRcvMsgChecksumA += data);
        ubxRcvMsgPayloadLength = data; // Payload length LSB.
        ubxFrameParseState = UBX_PARSE_PAYLOAD_LENGTH_MSB;
        break;
    case UBX_PARSE_PAYLOAD_LENGTH_MSB:
        ubxRcvMsgChecksumB += (ubxRcvMsgChecksumA += data);   // Accumulate both checksums.
        ubxRcvMsgPayloadLength += (uint16_t)(data << 8);   //Payload length MSB.
        if (ubxRcvMsgPayloadLength == 0) {
            // No payload for this message, skip to checksum checking.
            ubxFrameParseState = UBX_PARSE_CHECKSUM_A;
            break;
        }
        if (ubxRcvMsgPayloadLength > UBLOX_MAX_PAYLOAD_SANITY_SIZE) {
            // Payload length is not reasonable, treat as a bad packet, restart new message parsing.
            // Note that we do not parse the rest of the message, better to leave it and look for a new message.
#ifdef USE_DASHBOARD
            logErrorToPacketLog();
#endif
            if (PREAMBLE1 == data) {
                // If this byte is a preamble 1 value, it might be a new message, so look for preamble 2 instead of starting over.
                ubxFrameParseState = UBX_PARSE_PREAMBLE_SYNC_2;
            } else {
                ubxFrameParseState = UBX_PARSE_PREAMBLE_SYNC_1;
            }
            break;
        }
        // Payload length seems legit, go on to receive the payload content.
        ubxFrameParsePayloadCounter = 0;
        ubxFrameParseState = UBX_PARSE_PAYLOAD_CONTENT;
        break;
    case UBX_PARSE_PAYLOAD_CONTENT:
        ubxRcvMsgChecksumB += (ubxRcvMsgChecksumA += data);   // Accumulate both checksums.
        if (ubxFrameParsePayloadCounter < UBLOX_PAYLOAD_SIZE) {
            // Only add bytes to the buffer if we haven't reached the max supported payload size.
            // Note that we still read & checksum every byte so the checksum calculates correctly.
            ubxRcvMsgPayload.rawBytes[ubxFrameParsePayloadCounter] = data;
        }
        if (++ubxFrameParsePayloadCounter >= ubxRcvMsgPayloadLength) {
            // All bytes for payload length processed.
            ubxFrameParseState = UBX_PARSE_CHECKSUM_A;
            break;
        }
        // More payload content left, stay in this state.
        break;
    case UBX_PARSE_CHECKSUM_A:
        if (ubxRcvMsgChecksumA == data) {
            // Checksum A matches, go on to checksum B.
            ubxFrameParseState = UBX_PARSE_CHECKSUM_B;
            break;
        }
        // Bad checksum A, restart new message parsing.
        // Note that we do not parse checksum B, new message processing will handle skipping it if needed.
#ifdef USE_DASHBOARD
        logErrorToPacketLog();
#endif
        if (PREAMBLE1 == data) {
            // If this byte is a preamble 1 value, it might be a new message, so look for preamble 2 instead of starting over.
            ubxFrameParseState = UBX_PARSE_PREAMBLE_SYNC_2;
        } else {
            ubxFrameParseState = UBX_PARSE_PREAMBLE_SYNC_1;
        }
        break;
    case UBX_PARSE_CHECKSUM_B:
        if (ubxRcvMsgChecksumB == data) {
            // Checksum B also matches, successfully received a new full packet!
#ifdef USE_DASHBOARD
            dashboardGpsPacketCount++;  // Packet counter used by dashboard device.
            shiftPacketLog();           // Make space for message handling to add the message type char to the dashboard device packet log.
#endif
            // Handle the parsed message. Note this is a questionable inverted call dependency, but something for a later refactoring.
            newPositionDataReceived = UBLOX_parse_gps();     // True only when we have new position data from the parsed message.
            ubxFrameParseState = UBX_PARSE_PREAMBLE_SYNC_1;  // Restart new message parsing.
            break;
        }
            // Bad checksum B, restart new message parsing.
#ifdef USE_DASHBOARD
        logErrorToPacketLog();
#endif
        if (PREAMBLE1 == data) {
            // If this byte is a preamble 1 value, it might be a new message, so look for preamble 2 instead of starting over.
            ubxFrameParseState = UBX_PARSE_PREAMBLE_SYNC_2;
        } else {
            ubxFrameParseState = UBX_PARSE_PREAMBLE_SYNC_1;
        }
        break;
    }

    // Note this function returns if UBLOX_parse_gps() found new position data, NOT whether this function successfully parsed the frame or not.
    return newPositionDataReceived;
}
#endif // USE_GPS_UBLOX

static void gpsHandlePassthrough(uint8_t data)
{
    gpsNewData(data);
#ifdef USE_DASHBOARD
    if (featureIsEnabled(FEATURE_DASHBOARD)) {
        // Should be handled via a generic callback hook, so the GPS module doesn't have to be coupled to the dashboard module.
        dashboardUpdate(micros());
    }
#endif
}

// forward GPS data to specified port (used by CLI)
// return false if forwarding failed
// curently only way to stop forwarding is to reset the board
bool gpsPassthrough(serialPort_t *gpsPassthroughPort)
{
    if (!gpsPort) {
        // GPS port is not open for some reason - no GPS, MSP GPS, ..
        return false;
    }
    waitForSerialPortToFinishTransmitting(gpsPort);
    waitForSerialPortToFinishTransmitting(gpsPassthroughPort);

    if (!(gpsPort->mode & MODE_TX)) {
        // try to switch TX mode on
        serialSetMode(gpsPort, gpsPort->mode | MODE_TX);
    }

#ifdef USE_DASHBOARD
    if (featureIsEnabled(FEATURE_DASHBOARD)) {
        // Should be handled via a generic callback hook, so the GPS module doesn't have to be coupled to the dashboard module.
        dashboardShowFixedPage(PAGE_GPS);
    }
#endif

    serialPassthrough(gpsPort, gpsPassthroughPort, &gpsHandlePassthrough, NULL);
    // allow exitting passthrough mode in future
    return true;
}

float GPS_cosLat = 1.0f;  // this is used to offset the shrinking longitude as we go towards the poles
                          // longitude difference * scale is approximate distance in degrees

void GPS_calc_longitude_scaling(int32_t lat)
{
    GPS_cosLat = cos_approx(DEGREES_TO_RADIANS((float)lat / GPS_DEGREES_DIVIDER));
}

////////////////////////////////////////////////////////////////////////////////////
// Calculate the distance flown from gps position data
//
static void GPS_calculateDistanceFlown(bool initialize)
{
    static gpsLocation_t lastLLH = {0};

    if (initialize) {
        GPS_distanceFlownInCm = 0;
    } else if (STATE(GPS_FIX_HOME) && ARMING_FLAG(ARMED)) {
        uint16_t speed = gpsConfig()->gps_use_3d_speed ? gpsSol.speed3d : gpsSol.groundSpeed;
        // Only add up movement when speed is faster than minimum threshold
        if (speed > GPS_DISTANCE_FLOWN_MIN_SPEED_THRESHOLD_CM_S) {
            uint32_t dist;
            GPS_distance_cm_bearing(&gpsSol.llh, &lastLLH, gpsConfig()->gps_use_3d_speed, &dist, NULL);
            GPS_distanceFlownInCm += dist;
        }
    }
    lastLLH = gpsSol.llh;
}

void GPS_reset_home_position(void)
// runs, if GPS is defined, on arming via tryArm() in core.c, and on gyro cal via processRcStickPositions() in rc_controls.c
{
    if (!STATE(GPS_FIX_HOME) || !gpsConfig()->gps_set_home_point_once) {
        if (STATE(GPS_FIX) && gpsSol.numSat >= gpsRescueConfig()->minSats) {
            // those checks are always true for tryArm, but may not be true for gyro cal
            GPS_home_llh = gpsSol.llh;
            GPS_calc_longitude_scaling(gpsSol.llh.lat);
            ENABLE_STATE(GPS_FIX_HOME);
            // no point beeping success here since:
            // when triggered by tryArm, the arming beep is modified to indicate the GPS home fix status on arming, and
            // when triggered by gyro cal, the gyro cal beep takes priority over the GPS beep, so we won't hear the GPS beep
            // PS: to test for gyro cal, check for !ARMED, since we cannot be here while disarmed other than via gyro cal
        }
    }

#ifdef USE_GPS_UBLOX
    // disable Sat Info requests on arming
    if (gpsConfig()->provider == GPS_UBLOX) {
        setSatInfoMessageRate(0);
    }
#endif
    GPS_calculateDistanceFlown(true); // Initialize
}

////////////////////////////////////////////////////////////////////////////////////
// Get distance between two points in cm using spherical to Cartesian transform
// One one latitude unit, or one longitude unit at the equator, equals 1.113195 cm.
// Get bearing from pos1 to pos2, returns values with 0.01 degree precision
void GPS_distance_cm_bearing(const gpsLocation_t *from, const gpsLocation_t* to, bool dist3d, uint32_t *pDist, int32_t *pBearing)
{
    // TO DO : handle crossing the 180 degree meridian, as in `GPS_distance2d()`
    float dLat = (to->lat - from->lat) * EARTH_ANGLE_TO_CM;
    float dLon = (to->lon - from->lon) * GPS_cosLat * EARTH_ANGLE_TO_CM; // convert to local angle
    float dAlt = dist3d ? to->altCm - from->altCm : 0;

    if (pDist)
        *pDist = sqrtf(sq(dLat) + sq(dLon) + sq(dAlt));

    if (pBearing) {
        int32_t bearing = 9000.0f - RADIANS_TO_DEGREES(atan2_approx(dLat, dLon)) * 100.0f;      // Convert the output to 100xdeg / adjust to clockwise from North
        if (bearing < 0)
            bearing += 36000;
        *pBearing = bearing;
    }
}

static void GPS_calculateDistanceAndDirectionToHome(void)
{
    if (STATE(GPS_FIX_HOME)) {
        uint32_t dist;
        int32_t dir;
        GPS_distance_cm_bearing(&gpsSol.llh, &GPS_home_llh, false, &dist, &dir);
        GPS_distanceToHome = dist / 100; // m
        GPS_distanceToHomeCm = dist; // cm
        GPS_directionToHome = dir / 10; // degrees * 10 or decidegrees
    } else {
        // If we don't have home set, do not display anything
        GPS_distanceToHome = 0;
        GPS_distanceToHomeCm = 0;
        GPS_directionToHome = 0;
    }
}

// return distance vector in local, cartesian ENU coordinates
// note that parameter order is from, to
void GPS_distance2d(const gpsLocation_t *from, const gpsLocation_t *to, vector2_t *distance)
{
    int32_t deltaLon = to->lon - from->lon;
    // In case we crossed the 180° meridian:
    const int32_t deg180 = 180 * GPS_DEGREES_DIVIDER; // number of integer longitude steps in 180 degrees
    if (deltaLon > deg180) {
        deltaLon -= deg180;  // 360 * GPS_DEGREES_DIVIDER overflows int32_t, so use 180 twice
        deltaLon -= deg180;
    } else if (deltaLon <= -deg180) {
        deltaLon += deg180;
        deltaLon += deg180;
    }
    distance->x = deltaLon * GPS_cosLat * EARTH_ANGLE_TO_CM; // East-West distance, positive East
    distance->y = (float)(to->lat - from->lat) * EARTH_ANGLE_TO_CM;  // North-South distance, positive North
}

void onGpsNewData(void)
{
    if (!STATE(GPS_FIX)) {
        // if we don't have a 3D fix don't give data to GPS rescue
        return;
    }

    currentGpsStamp++; // new GPS data available

    gpsDataIntervalSeconds = gpsSol.navIntervalMs * 0.001f; // range for navIntervalMs is constrained to 50 - 2500
    gpsDataFrequencyHz = 1.0f / gpsDataIntervalSeconds;

    GPS_calculateDistanceAndDirectionToHome();
    if (ARMING_FLAG(ARMED)) {
        GPS_calculateDistanceFlown(false);
    }

#ifdef USE_GPS_LAP_TIMER
    gpsLapTimerNewGpsData();
#endif // USE_GPS_LAP_TIMER

}

// check if new data has been received since last check
// if client stamp is initialized to 0, gpsHasNewData will return false until first GPS position update
// if client stamp is initialized to ~0, gpsHasNewData will return true on first call
bool gpsHasNewData(uint16_t* stamp) {
    if (*stamp != currentGpsStamp) {
        *stamp = currentGpsStamp;
        return true;
    } else {
        return false;
    }
}

void gpsSetFixState(bool state)
{
    if (state) {
        ENABLE_STATE(GPS_FIX);
        ENABLE_STATE(GPS_FIX_EVER);
    } else {
        DISABLE_STATE(GPS_FIX);
    }
}

float getGpsDataIntervalSeconds(void)
{
    return gpsDataIntervalSeconds;
}

float getGpsDataFrequencyHz(void)
{
    return gpsDataFrequencyHz;
}

baudRate_e getGpsPortActualBaudRateIndex(void)
{
    return lookupBaudRateIndex(serialGetBaudRate(gpsPort));
}

#endif // USE_GPS
