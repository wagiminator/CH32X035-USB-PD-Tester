// ===================================================================================
// USB PD SINK Handler for CH32X035 (PD 3.1 with EPR and AVS)                 * v2.0 *
// ===================================================================================
//
// Architecture:
// - The interrupt service routine only does the time critical part: it answers every
//   received SOP message with GoodCRC (within tTransmit), filters retransmissions
//   (duplicate MessageID), copies the message into a small receive queue and flags
//   GoodCRCs for messages sent by the sink.
// - Everything else runs in PD_update() in the main context: policy engine state
//   machine, requests, EPR mode entry, chunked EPR_Source_Capabilities, EPR keep alive,
//   PPS refresh, soft/hard reset handling and timeouts.
//
// Reference:               https://github.com/openwch/ch32x035
//                          USB Power Delivery Specification Rev. 3.1
// 2023 by Stefan Wagner:   https://github.com/wagiminator

#include "usbpd_sink.h"

// ===================================================================================
// Timing Parameters (in milliseconds) and Constants
// ===================================================================================
#define PD_T_CC_DEBOUNCE      5     // CC line sampling interval
#define PD_T_WAIT_CAP         620   // tTypeCSinkWaitCap (max)
#define PD_T_WAIT_CAP_HR      2500  // wait for caps after hard reset (VBUS cycle)
#define PD_T_SENDER_RESPONSE  35    // tSenderResponse (24..30ms) + margin
#define PD_T_PS_TRANSITION    600   // tPSTransition SPR (450..550ms) + margin
#define PD_T_PS_TRANSITION_EPR 1100 // tPSTransition EPR (830..1020ms) + margin
#define PD_T_ENTER_EPR        550   // tEnterEPR (450..550ms)
#define PD_T_CHUNK            60    // max wait for next chunk
#define PD_T_EPR_KEEPALIVE    375   // tSinkEPRKeepAlive (250..500ms)
#define PD_T_PPS_REQUEST      5000  // PPS refresh interval (tPPSRequest max 10s)
#define PD_T_READY_SETTLE     40    // settle time before sink initiates an AMS
#define PD_T_TXNG_MAX         200   // give up waiting for SinkTxOK after this time

#define PD_N_RETRY            2     // nRetryCount
#define PD_N_HARD_RESET       2     // nHardResetCount
#define PD_N_EPR_TRIES        2     // max EPR entry attempts per attach

#define PD_AVS_MAX_CURRENT    5000  // max current of an EPR cable in mA
#define PD_RXQ_SIZE           4     // receive queue entries
#define PD_EXT_BUF_SIZE       (USBPD_EPR_PDO_MAX * 4)

// Policy engine states
typedef enum {
  PD_ST_DETACHED = 0,               // no source attached
  PD_ST_WAIT_CAPS,                  // waiting for Source_Capabilities
  PD_ST_WAIT_ACCEPT,                // Request sent, waiting for Accept/Reject/Wait
  PD_ST_WAIT_PSRDY,                 // Accept received, waiting for PS_RDY
  PD_ST_READY,                      // explicit contract, idle
  PD_ST_WAIT_SR_ACCEPT,             // Soft_Reset sent, waiting for Accept
  PD_ST_EPR_ENTER,                  // EPR_Mode (Enter) sent, waiting for result
  PD_ST_EPR_CAPS,                   // waiting for (next chunk of) EPR_Source_Capabilities
  PD_ST_WAIT_KA_ACK                 // EPR_KeepAlive sent, waiting for EPR_KeepAlive_Ack
} pd_state_t;

// Received message
typedef struct {
  uint16_t header;
  uint8_t  data[28];
} pd_msg_t;

// Control structure
static struct {
  pd_state_t state;                 // policy engine state
  uint8_t  cc;                      // active CC line
  uint8_t  ccCount;                 // CC debounce counter
  uint8_t  rev;                     // negotiated specification revision
  uint8_t  txMsgID;                 // MessageIDCounter
  uint8_t  hardResets;              // HardResetCounter
  uint32_t timer;                   // state timer start
  uint16_t timeout;                 // state timeout
  uint32_t ccTime;                  // last CC sampling
  uint32_t readyTime;               // time of entering READY
  uint32_t reqTime;                 // time of last request
  uint32_t keepAliveTime;           // time of last EPR keep alive
  uint32_t txNGTime;                // start of SinkTxNG period (0: none)
  // capabilities
  uint8_t  pdoNum;                  // number of PDO positions
  uint32_t pdo[USBPD_EPR_PDO_MAX];  // received (EPR) source capabilities
  uint8_t  srcEPRCapable;           // source supports EPR
  uint8_t  capsVersion;             // incremented on every new capabilities set
  // contract
  uint8_t  hasContract;             // explicit contract established
  uint8_t  activePos;               // active PDO position
  uint16_t activeV;                 // active voltage
  uint8_t  reqPos;                  // requested PDO position
  uint16_t reqV;                    // requested voltage
  uint8_t  reqPending;              // user request pending
  uint8_t  result;                  // result of last request
  // EPR
  uint8_t  eprMode;                 // EPR mode active
  uint8_t  eprState;                // EPR state (pd_epr_t)
  uint8_t  eprFail;                 // EPR entry failure reason
  uint8_t  eprTries;                // EPR entry attempts
  // chunked extended message assembly
  uint8_t  extBuf[PD_EXT_BUF_SIZE];
  uint16_t extSize;
  uint16_t extRcv;
  uint8_t  extChunk;
} PD;

// Interrupt shared variables
static pd_msg_t         PD_rxq[PD_RXQ_SIZE];  // receive queue
static volatile uint8_t PD_rxHead;            // queue write index (ISR)
static volatile uint8_t PD_rxTail;            // queue read index (main)
static volatile uint8_t PD_rxLastID = 0xFF;   // MessageID of last received message
static volatile uint8_t PD_txID;              // MessageID of message in transit
static volatile uint8_t PD_txAcked;           // GoodCRC for message in transit received
static volatile uint8_t PD_hardResetRcvd;     // Hard Reset received
static volatile uint8_t PD_specRev = USBPD_REVISION_20; // revision used for GoodCRC

// Buffer for DMA (TX and RX), max message: header (2) + 7 data objects (28) + CRC (4)
__attribute__ ((aligned(4))) static uint8_t PD_TR_buffer[36];

// Millisecond clock based on SysTick
static uint32_t PD_ms;
static uint32_t PD_tick;

// ===================================================================================
// Helper Functions
// ===================================================================================

// Get milliseconds since PD_connect()
static uint32_t PD_millis(void) {
  uint32_t d = (STK->CNTL - PD_tick) / DLY_MS_TIME;
  PD_tick += d * DLY_MS_TIME;
  PD_ms   += d;
  return PD_ms;
}

// Read/write little endian 32-bit values from/to byte buffers (alignment safe)
static uint32_t PD_get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void PD_put32(uint8_t* p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void PD_memcpy(uint8_t* dest, const uint8_t* src, uint8_t n) {
  while(n--) *dest++ = *src++;
}

// Set state with timeout
static void PD_setState(pd_state_t state, uint16_t timeout) {
  PD.state   = state;
  PD.timeout = timeout;
  PD.timer   = PD_millis();
}

// ===================================================================================
// PHY Functions
// ===================================================================================

// Enter reception mode
static void PD_RX_mode(void) {
  USBPD->BMC_CLK_CNT =  USBPD_TMR_RX;
  USBPD->CONTROL     = (USBPD->CONTROL & ~USBPD_PD_TX_EN) | USBPD_BMC_START;
}

// Start transmission of PD_TR_buffer with specified ordered set
static void PD_startTX(uint8_t sop, uint8_t length) {
  if((USBPD->CONFIG & USBPD_CC_SEL) == USBPD_CC_SEL) USBPD->PORT_CC2 |= USBPD_CC_LVE;
  else                                               USBPD->PORT_CC1 |= USBPD_CC_LVE;
  USBPD->BMC_CLK_CNT = USBPD_TMR_TX;
  USBPD->TX_SEL      = sop;
  USBPD->BMC_TX_SZ   = length;
  USBPD->STATUS      = 0;
  USBPD->CONTROL    |= USBPD_BMC_START | USBPD_PD_TX_EN;
}

// Wait until transmitter is idle (max 3ms)
static void PD_waitTXidle(void) {
  uint32_t start = STK->CNTL;
  while((USBPD->CONTROL & USBPD_PD_TX_EN) && ((STK->CNTL - start) < 3 * DLY_MS_TIME));
}

// Detect CC connection; returns 0:No connection, 1:CC1 connection, 2:CC2 connection
static uint8_t PD_checkCC(void) {
  uint8_t ccLine = USBPD_CCNONE;

  USBPD->PORT_CC1 &= ~(USBPD_CC_CE | USBPD_PA_CC_AI);
  USBPD->PORT_CC1 |= USBPD_CC_CMP_22;
  if(USBPD->PORT_CC1 & USBPD_PA_CC_AI) ccLine = USBPD_CC1;

  USBPD->PORT_CC2 &= ~(USBPD_CC_CE | USBPD_PA_CC_AI);
  USBPD->PORT_CC2 |= USBPD_CC_CMP_22;
  if(USBPD->PORT_CC2 & USBPD_PA_CC_AI) ccLine = USBPD_CC2;

  return ccLine;
}

// Check collision avoidance (PD 3.x): Rp = 3.0A (SinkTxOK) allows sink to initiate AMS
static uint8_t PD_sinkTxOK(void) {
  uint8_t ok;
  volatile uint16_t* port;
  if(PD.rev < USBPD_REVISION_30) return 1;
  port  = (PD.cc == USBPD_CC2) ? &USBPD->PORT_CC2 : &USBPD->PORT_CC1;
  *port = (*port & ~(USBPD_CC_CE | USBPD_PA_CC_AI)) | USBPD_CC_CMP_123;
  DLY_us(2);
  ok = (*port & USBPD_PA_CC_AI) ? 1 : 0;
  *port = (*port & ~(USBPD_CC_CE | USBPD_PA_CC_AI)) | USBPD_CC_CMP_22;
  return ok;
}

// Check if sink may initiate an AMS now (SinkTxOK or timeout for non-compliant sources)
static uint8_t PD_mayInitiate(uint32_t now) {
  if((now - PD.readyTime) < PD_T_READY_SETTLE) return 0;
  if(PD_sinkTxOK()) {
    PD.txNGTime = 0;
    return 1;
  }
  if(!PD.txNGTime) PD.txNGTime = now | 1;
  return ((now - PD.txNGTime) > PD_T_TXNG_MAX);
}

// ===================================================================================
// Protocol Layer (Transmit)
// ===================================================================================

// Send a message and wait for GoodCRC (with retries); returns 1 on success
static uint8_t PD_send(uint8_t type, uint8_t ndo, uint8_t extended, const uint8_t* payload) {
  USBPD_MessageHeader_t mh;
  uint8_t  retry;
  uint32_t start;

  mh.d16 = 0;
  mh.MessageHeader.MessageType           = type;
  mh.MessageHeader.SpecificationRevision = PD.rev;
  mh.MessageHeader.MessageID             = PD.txMsgID;
  mh.MessageHeader.NumberOfDataObjects   = ndo;
  mh.MessageHeader.Extended              = extended;

  for(retry = 0; retry <= PD_N_RETRY; retry++) {
    // Do not overwrite the DMA buffer while a GoodCRC is sent or a message is pending
    PD_waitTXidle();
    start = STK->CNTL;
    while((USBPD->STATUS & USBPD_IF_RX_ACT) && ((STK->CNTL - start) < DLY_MS_TIME));
    PD_waitTXidle();

    NVIC_DisableIRQ(USBPD_IRQn);
    PD_TR_buffer[0] = mh.d16;
    PD_TR_buffer[1] = mh.d16 >> 8;
    if(ndo) PD_memcpy(&PD_TR_buffer[2], payload, ndo << 2);
    PD_txID    = PD.txMsgID;
    PD_txAcked = 0;
    PD_startTX(USBPD_TX_SOP0, 2 + (ndo << 2));
    NVIC_EnableIRQ(USBPD_IRQn);

    // Wait for GoodCRC: max frame time (~1.5ms) + tReceive (1.1ms)
    start = STK->CNTL;
    while(!PD_txAcked && ((STK->CNTL - start) < 3 * DLY_MS_TIME));
    if(PD_txAcked) {
      PD.txMsgID = (PD.txMsgID + 1) & 7;
      return 1;
    }
  }
  PD.txMsgID = (PD.txMsgID + 1) & 7;
  return 0;
}

// Send control message
static uint8_t PD_sendControl(uint8_t type) {
  return PD_send(type, 0, 0, 0);
}

// Answer unsupported messages (PD 3.x: Not_Supported, PD 2.0: ignore)
static void PD_sendNotSupported(void) {
  if(PD.rev >= USBPD_REVISION_30) PD_sendControl(USBPD_CONTROL_MSG_NOT_SUPPORTED);
}

// Reset protocol layer counters
static void PD_protocolReset(void) {
  PD.txMsgID    = 0;
  PD_rxLastID   = 0xFF;
  PD.extSize    = 0;
  PD.extRcv     = 0;
  PD.reqPending = 0;
}

// Reset after hard reset (sent or received): back to default contract
static void PD_hardResetReset(void) {
  PD_protocolReset();
  PD.hasContract = 0;
  PD.activePos   = 1;
  PD.activeV     = 5000;
  PD.eprMode     = 0;
  if(PD.eprState != PD_EPR_FAILED) PD.eprState = PD_EPR_OFF;
  PD.pdoNum      = 0;
  if((PD.state != PD_ST_READY) && (PD.state != PD_ST_DETACHED)) PD.result = PD_RES_RESET;
  PD_setState(PD_ST_WAIT_CAPS, PD_T_WAIT_CAP_HR);
}

// Send Hard Reset
static void PD_sendHardReset(void) {
  uint32_t start;
  PD.hardResets++;
  PD_waitTXidle();
  NVIC_DisableIRQ(USBPD_IRQn);
  PD_startTX(USBPD_TX_HARD_RESET, 0);
  NVIC_EnableIRQ(USBPD_IRQn);
  start = STK->CNTL;
  while((USBPD->CONTROL & USBPD_PD_TX_EN) && ((STK->CNTL - start) < 3 * DLY_MS_TIME));
  PD_hardResetReset();
}

// Send Soft Reset (falls back to Hard Reset if not acknowledged)
static void PD_sendSoftReset(void) {
  PD_protocolReset();
  PD.eprMode = 0;
  if(PD_sendControl(USBPD_CONTROL_MSG_SOFT_RESET))
    PD_setState(PD_ST_WAIT_SR_ACCEPT, PD_T_SENDER_RESPONSE);
  else
    PD_sendHardReset();
}

// ===================================================================================
// PDO Decoding
// ===================================================================================

// Get raw PDO of position (1..13)
static uint32_t PD_getRawPDO(uint8_t pdonum) {
  if((pdonum < 1) || (pdonum > PD.pdoNum)) return 0;
  return PD.pdo[pdonum - 1];
}

// Get type of PDO
uint8_t PD_getPDOType(uint8_t pdonum) {
  USBPD_PDO_t pdo;
  pdo.d32 = PD_getRawPDO(pdonum);
  if(!pdo.d32) return PD_PDO_EMPTY;
  switch(pdo.d32 >> 30) {
    case USBPD_PDO_TYPE_FIXED:    return PD_PDO_FIXED;
    case USBPD_PDO_TYPE_BATTERY:  return PD_PDO_BATTERY;
    case USBPD_PDO_TYPE_VARIABLE: return PD_PDO_VARIABLE;
    default:
      switch((pdo.d32 >> 28) & 3) {
        case USBPD_APDO_SPR_PPS:  return PD_PDO_PPS;
        case USBPD_APDO_EPR_AVS:  return PD_PDO_AVS;
        default:                  return PD_PDO_UNKNOWN;
      }
  }
}

// Get minimum voltage of specified PDO
uint16_t PD_getPDOMinVoltage(uint8_t pdonum) {
  USBPD_PDO_t pdo;
  pdo.d32 = PD_getRawPDO(pdonum);
  switch(PD_getPDOType(pdonum)) {
    case PD_PDO_FIXED:    return POWER_DECODE_50MV(pdo.SourceFixedPDO.VoltageIn50mVunits);
    case PD_PDO_VARIABLE: return POWER_DECODE_50MV(pdo.SourceVariablePDO.MinVoltageIn50mVunits);
    case PD_PDO_BATTERY:  return POWER_DECODE_50MV(pdo.SourceBatteryPDO.MinVoltageIn50mVunits);
    case PD_PDO_PPS:      return POWER_DECODE_100MV(pdo.SourcePPSPDO.MinVoltageIn100mVincrements);
    case PD_PDO_AVS:      return POWER_DECODE_100MV(pdo.SourceEPRPDO.MinVoltageIn100mVincrements);
    default:              return 0;
  }
}

// Get maximum voltage of specified PDO
uint16_t PD_getPDOMaxVoltage(uint8_t pdonum) {
  USBPD_PDO_t pdo;
  pdo.d32 = PD_getRawPDO(pdonum);
  switch(PD_getPDOType(pdonum)) {
    case PD_PDO_FIXED:    return POWER_DECODE_50MV(pdo.SourceFixedPDO.VoltageIn50mVunits);
    case PD_PDO_VARIABLE: return POWER_DECODE_50MV(pdo.SourceVariablePDO.MaxVoltageIn50mVunits);
    case PD_PDO_BATTERY:  return POWER_DECODE_50MV(pdo.SourceBatteryPDO.MaxVoltageIn50mVunits);
    case PD_PDO_PPS:      return POWER_DECODE_100MV(pdo.SourcePPSPDO.MaxVoltageIn100mVincrements);
    case PD_PDO_AVS:      return POWER_DECODE_100MV(pdo.SourceEPRPDO.MaxVoltageIn100mVincrements);
    default:              return 0;
  }
}

// Get voltage of specified fixed power PDO
uint16_t PD_getPDOVoltage(uint8_t pdonum) {
  return PD_getPDOMinVoltage(pdonum);
}

// Get max power of PDO in W
uint16_t PD_getPDOMaxPower(uint8_t pdonum) {
  USBPD_PDO_t pdo;
  pdo.d32 = PD_getRawPDO(pdonum);
  switch(PD_getPDOType(pdonum)) {
    case PD_PDO_BATTERY:  return (pdo.SourceBatteryPDO.MaxPowerIn250mWunits >> 2);
    case PD_PDO_AVS:      return pdo.SourceEPRPDO.PDPIn1Wincrements;
    case PD_PDO_FIXED:
    case PD_PDO_VARIABLE:
    case PD_PDO_PPS:      return ((uint32_t)PD_getPDOMaxVoltage(pdonum) * PD_getPDOMaxCurrent(pdonum)) / 1000000;
    default:              return 0;
  }
}

// Get max current of specified PDO in mA (AVS, battery: at minimum voltage)
uint16_t PD_getPDOMaxCurrent(uint8_t pdonum) {
  USBPD_PDO_t pdo;
  uint32_t    i;
  uint16_t    v = PD_getPDOMinVoltage(pdonum);
  pdo.d32 = PD_getRawPDO(pdonum);
  switch(PD_getPDOType(pdonum)) {
    case PD_PDO_FIXED:    return POWER_DECODE_10MA(pdo.SourceFixedPDO.MaxCurrentIn10mAunits);
    case PD_PDO_VARIABLE: return POWER_DECODE_10MA(pdo.SourceVariablePDO.MaxCurrentIn10mAunits);
    case PD_PDO_PPS:      return POWER_DECODE_50MA(pdo.SourcePPSPDO.MaxCurrentIn50mAincrements);
    case PD_PDO_BATTERY:
      if(!v) return 0;
      return ((uint32_t)pdo.SourceBatteryPDO.MaxPowerIn250mWunits * 250000) / v;
    case PD_PDO_AVS:
      if(!v) return 0;
      i = ((uint32_t)pdo.SourceEPRPDO.PDPIn1Wincrements * 1000000) / v;
      return (i > PD_AVS_MAX_CURRENT) ? PD_AVS_MAX_CURRENT : i;
    default:              return 0;
  }
}

// Get voltage step of PDO in mV (0: not adjustable)
uint16_t PD_getVoltageStep(uint8_t pdonum) {
  switch(PD_getPDOType(pdonum)) {
    case PD_PDO_PPS: return PD_PPS_STEP;
    case PD_PDO_AVS: return PD_AVS_STEP;
    default:         return 0;
  }
}

// Get highest voltage of PDO which is usable with the configured voltage limit
uint16_t PD_getPDOLimitVoltage(uint8_t pdonum) {
  uint16_t v    = PD_getPDOMaxVoltage(pdonum);
  uint16_t step = PD_getVoltageStep(pdonum);
  if(v > PD_VOLTAGE_LIMIT) {
    v = PD_VOLTAGE_LIMIT;
    if(step) v -= v % step;
  }
  return v;
}

// Check if PDO can be requested (type, voltage limit, EPR mode)
uint8_t PD_isPDOSelectable(uint8_t pdonum) {
  switch(PD_getPDOType(pdonum)) {
    case PD_PDO_FIXED:
    case PD_PDO_VARIABLE:
    case PD_PDO_BATTERY:
      // the whole voltage range must be within the limit
      if(PD_getPDOMaxVoltage(pdonum) > PD_VOLTAGE_LIMIT) return 0;
      break;
    case PD_PDO_PPS:
      if(PD_getPDOMinVoltage(pdonum) > PD_VOLTAGE_LIMIT) return 0;
      break;
    case PD_PDO_AVS:
      if(!PD.eprMode) return 0;
      if(PD_getPDOMinVoltage(pdonum) > PD_VOLTAGE_LIMIT) return 0;
      break;
    default:
      return 0;
  }
  if((pdonum >= USBPD_EPR_FIRST_POS) && !PD.eprMode) return 0;
  return 1;
}

// Clamp requested voltage to PDO range, voltage limit and step size
static uint16_t PD_clampVoltage(uint8_t pdonum, uint16_t voltage) {
  uint16_t vmin = PD_getPDOMinVoltage(pdonum);
  uint16_t vmax = PD_getPDOLimitVoltage(pdonum);
  uint16_t step = PD_getVoltageStep(pdonum);
  if(!step) return vmin;                    // fixed, variable, battery
  voltage -= voltage % step;
  if(voltage < vmin) voltage = vmin;
  if(voltage > vmax) voltage = vmax;
  return voltage;
}

// Count PDOs of specified type
static uint8_t PD_countType(uint8_t type) {
  uint8_t i, cnt = 0;
  for(i = 1; i <= PD.pdoNum; i++) if(PD_getPDOType(i) == type) cnt++;
  return cnt;
}

// ===================================================================================
// Policy Engine (Main Context)
// ===================================================================================

// Send Request or EPR_Request for specified PDO and voltage
static void PD_sendRequest(uint8_t pdonum, uint16_t voltage) {
  USBPD_SINKRDO_t rdo;
  USBPD_PDO_t     pdo;
  uint8_t         payload[8];
  uint32_t        current;

  pdo.d32 = PD_getRawPDO(pdonum);
  rdo.d32 = 0;

  switch(PD_getPDOType(pdonum)) {
    case PD_PDO_PPS:
      rdo.SinkPPSRDO.OutputVoltage               = voltage / 20;
      rdo.SinkPPSRDO.OperatingCurrentIn50mAunits = pdo.SourcePPSPDO.MaxCurrentIn50mAincrements;
      break;
    case PD_PDO_AVS:
      current = ((uint32_t)pdo.SourceEPRPDO.PDPIn1Wincrements * 1000000) / voltage;
      if(current > PD_AVS_MAX_CURRENT) current = PD_AVS_MAX_CURRENT;
      rdo.SinkAVSRDO.OutputVoltage               = (voltage / 25) & ~3u; // 100mV granularity
      rdo.SinkAVSRDO.OperatingCurrentIn50mAunits = current / 50;
      break;
    case PD_PDO_BATTERY:
      rdo.SinkBatteryRDO.MaxOperatingPower250mWunits = pdo.SourceBatteryPDO.MaxPowerIn250mWunits;
      rdo.SinkBatteryRDO.OperatingPowerIn250mWunits  = pdo.SourceBatteryPDO.MaxPowerIn250mWunits;
      break;
    default: // fixed and variable supply
      rdo.SinkFixedVariableRDO.MaxOperatingCurrent10mAunits = pdo.SourceFixedPDO.MaxCurrentIn10mAunits;
      rdo.SinkFixedVariableRDO.OperatingCurrentIn10mAunits  = pdo.SourceFixedPDO.MaxCurrentIn10mAunits;
      break;
  }
  // common fields (identical bit positions in all RDO types)
  rdo.SinkFixedVariableRDO.ObjectPosition           = pdonum;
  rdo.SinkFixedVariableRDO.NoUSBSuspend             = 1;
  rdo.SinkFixedVariableRDO.USBCommunicationsCapable = 1;
  rdo.SinkFixedVariableRDO.EPRModeCapable           = (PD_EPR_ENABLE && (PD.rev >= USBPD_REVISION_30)) ? 1 : 0;

  PD.reqPos  = pdonum;
  PD.reqV    = voltage;
  PD.reqTime = PD_millis();
  PD_put32(&payload[0], rdo.d32);

  if(PD.eprMode) {
    PD_put32(&payload[4], pdo.d32);           // EPR_Request: RDO + copy of the PDO
    if(!PD_send(USBPD_DATA_MSG_EPR_REQUEST, 2, 0, payload)) { PD_sendSoftReset(); return; }
  }
  else {
    if(!PD_send(USBPD_DATA_MSG_REQUEST, 1, 0, payload))     { PD_sendSoftReset(); return; }
  }
  PD_setState(PD_ST_WAIT_ACCEPT, PD_T_SENDER_RESPONSE);
}

// Answer new (EPR) source capabilities with a request (keep contract if possible)
static void PD_answerCaps(void) {
  uint8_t  pos = 1;
  uint16_t v   = 5000;
  if(PD.hasContract && PD_isPDOSelectable(PD.activePos)) {
    pos = PD.activePos;
    v   = PD_clampVoltage(pos, PD.activeV);
  }
  else v = PD_getPDOMinVoltage(1);
  PD_sendRequest(pos, v);
}

// Store SPR Source_Capabilities
static void PD_storeSourceCaps(const pd_msg_t* msg, uint8_t ndo) {
  USBPD_MessageHeader_t mh;
  USBPD_PDO_t pdo;
  uint8_t i;

  mh.d16 = msg->header;
  PD.rev = (mh.MessageHeader.SpecificationRevision >= USBPD_REVISION_30) ? USBPD_REVISION_30
                                                                          : USBPD_REVISION_20;
  PD_specRev = PD.rev;
  PD.pdoNum  = ndo;
  for(i = 0; i < ndo; i++) PD.pdo[i] = PD_get32(&msg->data[i << 2]);
  for(     ; i < USBPD_EPR_PDO_MAX; i++) PD.pdo[i] = 0;

  PD.capsVersion++;
  pdo.d32 = PD.pdo[0];
  PD.srcEPRCapable = (pdo.SourceFixedPDO.FixedSupply == USBPD_PDO_TYPE_FIXED)
                   && pdo.SourceFixedPDO.EPRModeCapable && (PD.rev >= USBPD_REVISION_30);

  // SPR caps mean the source is (now) in SPR mode; allow (re-)entry if tries are left
  PD.eprMode = 0;
  if(!PD.srcEPRCapable)                  PD.eprState = PD_EPR_UNSUPPORTED;
  else if(PD.eprState != PD_EPR_FAILED)  PD.eprState = (PD.eprTries < PD_N_EPR_TRIES) ? PD_EPR_OFF
                                                                                      : PD_EPR_FAILED;
}

// Handle a chunk of EPR_Source_Capabilities
static void PD_handleEPRCapsChunk(const pd_msg_t* msg) {
  USBPD_ExtendedHeader_t eh;
  uint8_t  payload[4];
  uint8_t  n, i;

  eh.d16 = msg->data[0] | (msg->data[1] << 8);
  if(!eh.ExtendedHeader.Chunked || eh.ExtendedHeader.RequestChunk) return;

  if(eh.ExtendedHeader.ChunkNumber == 0) {
    PD.extSize  = eh.ExtendedHeader.DataSize;
    if(PD.extSize > PD_EXT_BUF_SIZE) PD.extSize = PD_EXT_BUF_SIZE;
    PD.extRcv   = 0;
    PD.extChunk = 0;
  }
  else if((eh.ExtendedHeader.ChunkNumber != PD.extChunk) || !PD.extSize) {
    PD.extSize = 0;                            // unexpected chunk
    return;
  }

  // Copy chunk data
  n = PD.extSize - PD.extRcv;
  if(n > USBPD_MAX_CHUNK_SIZE) n = USBPD_MAX_CHUNK_SIZE;
  PD_memcpy(&PD.extBuf[PD.extRcv], &msg->data[2], n);
  PD.extRcv += n;

  // Request next chunk
  if(PD.extRcv < PD.extSize) {
    PD.extChunk = eh.ExtendedHeader.ChunkNumber + 1;
    eh.d16 = 0;
    eh.ExtendedHeader.Chunked      = 1;
    eh.ExtendedHeader.RequestChunk = 1;
    eh.ExtendedHeader.ChunkNumber  = PD.extChunk;
    payload[0] = eh.d16; payload[1] = eh.d16 >> 8; payload[2] = 0; payload[3] = 0;
    if(!PD_send(USBPD_EXT_MSG_EPR_SRC_CAP, 1, 1, payload)) PD_sendSoftReset();
    else PD_setState(PD_ST_EPR_CAPS, PD_T_CHUNK);
    return;
  }

  // Message complete: store capabilities (positions 1..7 SPR, 8.. EPR)
  PD.pdoNum = PD.extSize >> 2;
  for(i = 0; i < PD.pdoNum; i++) PD.pdo[i] = PD_get32(&PD.extBuf[i << 2]);
  for(     ; i < USBPD_EPR_PDO_MAX; i++) PD.pdo[i] = 0;
  PD.extSize  = 0;
  PD.eprMode  = 1;
  PD.eprState = PD_EPR_ACTIVE;
  PD.capsVersion++;
  PD.keepAliveTime = PD_millis();
  PD_answerCaps();
}

// Handle received message
static void PD_handleMessage(const pd_msg_t* msg) {
  USBPD_MessageHeader_t mh;
  USBPD_EPRModeDO_t     epr;
  USBPD_SinkPDO_t       snk;
  USBPD_RevisionDO_t    rev;
  uint8_t  payload[4];
  uint8_t  ndo;
  uint32_t now = PD_millis();

  mh.d16 = msg->header;
  ndo    = mh.MessageHeader.NumberOfDataObjects;

  // ----- Extended messages -----
  if(mh.MessageHeader.Extended) {
    switch(mh.MessageHeader.MessageType) {
      case USBPD_EXT_MSG_EPR_SRC_CAP:
        PD_handleEPRCapsChunk(msg);
        break;
      case USBPD_EXT_MSG_EXTENDED_CONTROL:
        if(msg->data[2] == USBPD_ECDB_EPR_KEEPALIVE_ACK) {
          if(PD.state == PD_ST_WAIT_KA_ACK) PD.state = PD_ST_READY;
          break;
        }
        PD_sendNotSupported();
        break;
      default: {
        // answer only the first chunk of unsupported extended messages
        USBPD_ExtendedHeader_t eh;
        eh.d16 = msg->data[0] | (msg->data[1] << 8);
        if(!eh.ExtendedHeader.ChunkNumber && !eh.ExtendedHeader.RequestChunk) PD_sendNotSupported();
        break;
      }
    }
    return;
  }

  // ----- Control messages -----
  if(ndo == 0) {
    switch(mh.MessageHeader.MessageType) {

      case USBPD_CONTROL_MSG_ACCEPT:
        if(PD.state == PD_ST_WAIT_ACCEPT)
          PD_setState(PD_ST_WAIT_PSRDY, PD.eprMode ? PD_T_PS_TRANSITION_EPR : PD_T_PS_TRANSITION);
        else if(PD.state == PD_ST_WAIT_SR_ACCEPT)
          PD_setState(PD_ST_WAIT_CAPS, PD_T_WAIT_CAP);
        break;

      case USBPD_CONTROL_MSG_REJECT:
      case USBPD_CONTROL_MSG_WAIT:
        if(PD.state == PD_ST_WAIT_ACCEPT) {
          PD.result = (mh.MessageHeader.MessageType == USBPD_CONTROL_MSG_WAIT) ? PD_RES_WAIT
                                                                              : PD_RES_REJECT;
          PD_setState(PD_ST_READY, 0);         // keep previous contract
          PD.readyTime = now;
        }
        break;

      case USBPD_CONTROL_MSG_PS_RDY:
        if(PD.state == PD_ST_WAIT_PSRDY) {
          PD.activePos   = PD.reqPos;
          PD.activeV     = PD.reqV;
          PD.hasContract = 1;
          PD.hardResets  = 0;
          PD.result      = PD_RES_OK;
          PD_setState(PD_ST_READY, 0);
          PD.readyTime = now;
        }
        break;

      case USBPD_CONTROL_MSG_SOFT_RESET:
        PD_protocolReset();
        PD_rxLastID = 0;                       // Soft_Reset itself had MessageID 0
        PD.eprMode  = 0;
        if(PD.state != PD_ST_READY) PD.result = PD_RES_RESET;
        if(PD_sendControl(USBPD_CONTROL_MSG_ACCEPT)) PD_setState(PD_ST_WAIT_CAPS, PD_T_WAIT_CAP);
        else PD_sendHardReset();
        break;

      case USBPD_CONTROL_MSG_GET_SNK_CAP:
        snk.d32 = 0;
        snk.SinkFixedPDO.VoltageIn50mVunits       = 5000 / 50;
        snk.SinkFixedPDO.OperationalCurrentIn10mA = 3000 / 10;
        snk.SinkFixedPDO.HigherCapability         = 1;
        snk.SinkFixedPDO.USBCommunicationsCapable = 1;
        PD_put32(payload, snk.d32);
        PD_send(USBPD_DATA_MSG_SNK_CAP, 1, 0, payload);
        break;

      case USBPD_CONTROL_MSG_GET_REVISION:
        if(PD.rev < USBPD_REVISION_30) { PD_sendNotSupported(); break; }
        rev.d32 = 0;
        rev.Revision.RevisionMajor = 3;
        rev.Revision.RevisionMinor = 1;
        rev.Revision.VersionMajor  = 1;
        rev.Revision.VersionMinor  = 0;
        PD_put32(payload, rev.d32);
        PD_send(USBPD_DATA_MSG_REVISION, 1, 0, payload);
        break;

      case USBPD_CONTROL_MSG_NOT_SUPPORTED:
        if(PD.state == PD_ST_EPR_ENTER) {      // source does not support EPR_Mode
          PD.eprState = PD_EPR_FAILED;
          PD.eprFail  = USBPD_EPR_FAIL_UNKNOWN;
          PD_setState(PD_ST_READY, 0);
          PD.readyTime = now;
        }
        break;

      case USBPD_CONTROL_MSG_DR_SWAP:          // swaps are not supported
      case USBPD_CONTROL_MSG_PR_SWAP:
      case USBPD_CONTROL_MSG_VCONN_SWAP:
        if(PD.rev >= USBPD_REVISION_30) PD_sendNotSupported();
        else PD_sendControl(USBPD_CONTROL_MSG_REJECT);
        break;

      case USBPD_CONTROL_MSG_GOODCRC:          // handled in ISR
      case USBPD_CONTROL_MSG_PING:             // ignore
      case USBPD_CONTROL_MSG_GOTOMIN:          // no GiveBack support, ignore
        break;

      default:                                 // Get_Status, Get_Source_Info, ...
        PD_sendNotSupported();
        break;
    }
    return;
  }

  // ----- Data messages -----
  switch(mh.MessageHeader.MessageType) {

    case USBPD_DATA_MSG_SRC_CAP:
      PD_storeSourceCaps(msg, ndo);
      PD_answerCaps();
      break;

    case USBPD_DATA_MSG_EPR_MODE:
      epr.d32 = PD_get32(msg->data);
      switch(epr.EPRMode.Action) {
        case USBPD_EPR_ACTION_ENTER_ACK:
          if(PD.state == PD_ST_EPR_ENTER) PD_setState(PD_ST_EPR_ENTER, PD_T_ENTER_EPR);
          break;
        case USBPD_EPR_ACTION_ENTER_OK:
          if(PD.state == PD_ST_EPR_ENTER) {
            PD.eprMode = 1;
            PD.extSize = 0;
            PD_setState(PD_ST_EPR_CAPS, PD_T_SENDER_RESPONSE + PD_T_CHUNK);
          }
          break;
        case USBPD_EPR_ACTION_ENTER_FAIL:
          PD.eprState = PD_EPR_FAILED;
          PD.eprFail  = epr.EPRMode.Data;
          PD.eprMode  = 0;
          PD_setState(PD_ST_READY, 0);
          PD.readyTime = now;
          break;
        case USBPD_EPR_ACTION_EXIT:            // source leaves EPR, SPR caps follow
          PD.eprMode  = 0;
          PD.eprState = PD_EPR_OFF;
          PD_setState(PD_ST_WAIT_CAPS, PD_T_WAIT_CAP);
          break;
        default:
          break;
      }
      break;

    case USBPD_DATA_MSG_ALERT:                 // informational only
    case USBPD_DATA_MSG_BIST:                  // compliance test modes not supported
    case USBPD_DATA_MSG_SRC_INFO:
    case USBPD_DATA_MSG_REVISION:
    case USBPD_DATA_MSG_BATTERY_STATUS:
      break;

    default:                                   // VDM, Enter_USB, Request (as sink), ...
      PD_sendNotSupported();
      break;
  }
}

// Handle state timeouts and sink initiated messages
static void PD_handleTimers(uint32_t now) {
  uint8_t payload[4];
  USBPD_ExtendedHeader_t eh;
  USBPD_EPRModeDO_t      epr;

  switch(PD.state) {

    case PD_ST_WAIT_CAPS:
      if((now - PD.timer) > PD.timeout) {
        if(PD.hardResets < PD_N_HARD_RESET) PD_sendHardReset();
        else PD.timer = now;                   // no PD source: keep waiting silently
      }
      break;

    case PD_ST_WAIT_ACCEPT:
      if((now - PD.timer) > PD.timeout) { PD.result = PD_RES_TIMEOUT; PD_sendHardReset(); }
      break;

    case PD_ST_WAIT_PSRDY:
      if((now - PD.timer) > PD.timeout) { PD.result = PD_RES_TIMEOUT; PD_sendHardReset(); }
      break;

    case PD_ST_WAIT_SR_ACCEPT:
      if((now - PD.timer) > PD.timeout) PD_sendHardReset();
      break;

    case PD_ST_WAIT_KA_ACK:                    // missing ack is tolerated
      if((now - PD.timer) > PD.timeout) PD.state = PD_ST_READY;
      break;

    case PD_ST_EPR_ENTER:
    case PD_ST_EPR_CAPS:
      if((now - PD.timer) > PD.timeout) {
        PD.eprState = PD_EPR_FAILED;
        PD.eprFail  = USBPD_EPR_FAIL_UNKNOWN;
        PD_sendSoftReset();                    // source resends SPR caps
      }
      break;

    case PD_ST_READY:
      if(!PD.hasContract) break;

      // EPR keep alive
      if(PD.eprMode && ((now - PD.keepAliveTime) >= PD_T_EPR_KEEPALIVE)) {
        if(!PD_mayInitiate(now)) break;
        eh.d16 = 0;
        eh.ExtendedHeader.Chunked  = 1;
        eh.ExtendedHeader.DataSize = 2;
        payload[0] = eh.d16; payload[1] = eh.d16 >> 8;
        payload[2] = USBPD_ECDB_EPR_KEEPALIVE; payload[3] = 0;
        PD.keepAliveTime = now;
        if(PD_send(USBPD_EXT_MSG_EXTENDED_CONTROL, 1, 1, payload))
          PD_setState(PD_ST_WAIT_KA_ACK, PD_T_SENDER_RESPONSE);
        else PD_sendSoftReset();
        break;
      }

      // User request pending
      if(PD.reqPending) {
        if(!PD_mayInitiate(now)) break;
        PD.reqPending = 0;
        PD_sendRequest(PD.reqPos, PD.reqV);
        break;
      }

      // Enter EPR mode
      if(PD_EPR_ENABLE && PD.srcEPRCapable && !PD.eprMode && (PD.eprState == PD_EPR_OFF)) {
        if(!PD_mayInitiate(now)) break;
        PD.eprTries++;
        PD.eprState = PD_EPR_ENTERING;
        epr.d32 = 0;
        epr.EPRMode.Action = USBPD_EPR_ACTION_ENTER;
        epr.EPRMode.Data   = PD_EPR_SINK_PDP;
        PD_put32(payload, epr.d32);
        if(PD_send(USBPD_DATA_MSG_EPR_MODE, 1, 0, payload))
          PD_setState(PD_ST_EPR_ENTER, PD_T_ENTER_EPR);
        else {
          PD.eprState = PD_EPR_FAILED;
          PD_sendSoftReset();
        }
        break;
      }

      // PPS keep alive (source falls back after tPPSTimeout without request)
      if((PD_getPDOType(PD.activePos) == PD_PDO_PPS) && ((now - PD.reqTime) >= PD_T_PPS_REQUEST)) {
        if(!PD_mayInitiate(now)) break;
        PD_sendRequest(PD.activePos, PD.activeV);
      }
      break;

    default:
      break;
  }
}

// Check CC attach/detach
static void PD_checkAttach(uint32_t now) {
  uint8_t ccLine;
  if((now - PD.ccTime) < PD_T_CC_DEBOUNCE) return;
  PD.ccTime = now;
  ccLine = PD_checkCC();

  if(PD.state == PD_ST_DETACHED) {
    if(ccLine == USBPD_CCNONE) { PD.ccCount = 0; return; }
    if(ccLine != PD.cc) { PD.cc = ccLine; PD.ccCount = 0; return; }
    if(++PD.ccCount <= 5) return;
    // attached
    PD.ccCount = 0;
    if(ccLine == USBPD_CC2) USBPD->CONFIG |=  USBPD_CC_SEL;
    else                    USBPD->CONFIG &= ~USBPD_CC_SEL;
    PD_protocolReset();
    PD_rxHead = PD_rxTail = 0;
    PD_hardResetRcvd = 0;
    PD_RX_mode();
    NVIC_SetPriority(USBPD_IRQn, 0x00);
    NVIC_EnableIRQ(USBPD_IRQn);
    PD_setState(PD_ST_WAIT_CAPS, PD_T_WAIT_CAP);
  }
  else {
    if(ccLine != USBPD_CCNONE) { PD.ccCount = 0; return; }
    if(++PD.ccCount <= 5) return;
    // detached
    NVIC_DisableIRQ(USBPD_IRQn);
    PD.ccCount     = 0;
    PD.cc          = USBPD_CCNONE;
    PD.hasContract = 0;
    PD.pdoNum      = 0;
    PD.eprMode     = 0;
    PD.eprState    = PD_EPR_OFF;
    PD.eprTries    = 0;
    PD.hardResets  = 0;
    PD.activePos   = 1;
    PD.activeV     = 5000;
    PD.rev         = USBPD_REVISION_20;
    PD_specRev     = USBPD_REVISION_20;
    PD.state       = PD_ST_DETACHED;
  }
}

// ===================================================================================
// USB PD SINK Front End Functions
// ===================================================================================

// Non-blocking protocol handler, call at least every 5ms
void PD_update(void) {
  uint32_t now = PD_millis();

  PD_checkAttach(now);
  if(PD.state == PD_ST_DETACHED) return;

  if(PD_hardResetRcvd) {
    PD_hardResetRcvd = 0;
    PD_hardResetReset();
  }

  while(PD_rxTail != PD_rxHead) {
    PD_handleMessage(&PD_rxq[PD_rxTail]);
    PD_rxTail = (PD_rxTail + 1) % PD_RXQ_SIZE;
  }

  PD_handleTimers(PD_millis());
}

// Check if the policy engine is idle (contract established, nothing pending)
static uint8_t PD_isIdle(void) {
  if(PD.state != PD_ST_READY || !PD.hasContract || PD.reqPending) return 0;
  if(PD_EPR_ENABLE && PD.srcEPRCapable && !PD.eprMode && (PD.eprState == PD_EPR_OFF)) return 0;
  return 1;
}

// Process until idle (return 1) or timeout (return 0)
static uint8_t PD_waitIdle(uint16_t timeout) {
  uint32_t start = PD_millis();
  while((PD_millis() - start) < timeout) {
    PD_update();
    if(PD_isIdle()) return 1;
    DLY_ms(1);
  }
  return 0;
}

// Negotiate current settings and wait until finished (return 1) or timeout (return 0)
uint8_t PD_negotiate(void) {
  return PD_waitIdle(1500);
}

// Initialize PD registers and states, then connect
uint8_t PD_connect(void) {
  RCC->APB2PCENR |= RCC_AFIOEN | RCC_IOPCEN;
  RCC->AHBPCENR  |= RCC_USBPD;
  GPIOB->CFGHR    = (GPIOB->CFGHR & ~( (uint32_t)0b1111<<(((14)&7)<<2) | (uint32_t)0b1111<<(((15)&7)<<2)))
                                  |  ( (uint32_t)0b0100<<(((14)&7)<<2) | (uint32_t)0b0100<<(((15)&7)<<2));
  #ifdef USB_VDD
    #if USB_VDD > 0
      AFIO->CTLR |= USBPD_IN_HVT;
    #else
      AFIO->CTLR |= USBPD_IN_HVT | USBPD_PHY_V33;
    #endif
  #else
    RCC->APB1PCENR |= RCC_PWREN;
    PWR->CTLR |= PWR_CTLR_PLS;
    if(PWR->CSR & PWR_CSR_PVDO) AFIO->CTLR |= USBPD_IN_HVT | USBPD_PHY_V33;
    else                        AFIO->CTLR |= USBPD_IN_HVT;
  #endif

  USBPD->DMA      = (uint32_t)PD_TR_buffer;
  USBPD->CONFIG   = USBPD_IE_RX_ACT | USBPD_IE_RX_RESET | USBPD_IE_TX_END  | USBPD_PD_DMA_EN;
  USBPD->STATUS   = USBPD_BUF_ERR   | USBPD_IF_RX_BIT   | USBPD_IF_RX_BYTE
                  | USBPD_IF_RX_ACT | USBPD_IF_RX_RESET | USBPD_IF_TX_END;
  USBPD->PORT_CC1 = USBPD_CC_CMP_66 | USBPD_CC_PD;
  USBPD->PORT_CC2 = USBPD_CC_CMP_66 | USBPD_CC_PD;

  PD_tick      = STK->CNTL;
  PD_ms        = 0;
  PD.state     = PD_ST_DETACHED;
  PD.cc        = USBPD_CCNONE;
  PD.rev       = USBPD_REVISION_20;
  PD.activePos = 1;
  PD.activeV   = 5000;
  PD.ccTime    = 0;

  // Wait for contract (incl. hard reset recovery and EPR entry)
  PD_waitIdle(8000);
  return PD.hasContract;
}

// Get total number of PDO positions (EPR: incl. empty slots)
uint8_t PD_getPDONum(void) {
  return PD.pdoNum;
}

// Get number of fixed power PDOs
uint8_t PD_getFixedNum(void) {
  return PD_countType(PD_PDO_FIXED);
}

// Get number of programmable power PDOs
uint8_t PD_getPPSNum(void) {
  return PD_countType(PD_PDO_PPS);
}

// Get number of adjustable voltage supply PDOs
uint8_t PD_getAVSNum(void) {
  return PD_countType(PD_PDO_AVS);
}

// Set specified PDO and voltage; returns 0:failed, 1:success
uint8_t PD_setPDO(uint8_t pdonum, uint16_t voltage) {
  if(!PD_waitIdle(1500) || !PD_isPDOSelectable(pdonum)) {
    PD.result = PD_RES_INVALID;
    return 0;
  }
  voltage       = PD_clampVoltage(pdonum, voltage);
  PD.reqPos     = pdonum;
  PD.reqV       = voltage;
  PD.result     = PD_RES_NONE;
  PD.reqPending = 1;
  if(!PD_waitIdle(PD_T_PS_TRANSITION_EPR + 500) && (PD.result == PD_RES_NONE))
    PD.result = PD_RES_TIMEOUT;
  // success only if the contract really is the requested one (not a recovered old one)
  if((PD.result == PD_RES_OK) && ((PD.activePos != pdonum) || (PD.activeV != voltage)))
    PD.result = PD_RES_RESET;
  return (PD.result == PD_RES_OK);
}

// Set specified voltage (in millivolts) if available; returns 0:failed, 1:success
uint8_t PD_setVoltage(uint16_t voltage) {
  uint8_t i;
  // prefer fixed PDOs
  for(i = 1; i <= PD.pdoNum; i++) {
    if((PD_getPDOType(i) == PD_PDO_FIXED) && PD_isPDOSelectable(i)
       && (PD_getPDOVoltage(i) == voltage)) return PD_setPDO(i, voltage);
  }
  // then programmable/adjustable PDOs
  for(i = 1; i <= PD.pdoNum; i++) {
    if(PD_getVoltageStep(i) && PD_isPDOSelectable(i)
       && (PD_getPDOMinVoltage(i) <= voltage) && (PD_getPDOLimitVoltage(i) >= voltage))
      return PD_setPDO(i, voltage);
  }
  return 0;
}

// Get active PDO
uint8_t PD_getPDO(void) {
  return PD.activePos;
}

// Get active voltage
uint16_t PD_getVoltage(void) {
  return PD.activeV;
}

// Get active max current
uint16_t PD_getCurrent(void) {
  uint32_t i;
  if(PD_getPDOType(PD.activePos) == PD_PDO_AVS) {
    if(!PD.activeV) return 0;
    i = ((uint32_t)PD_getPDOMaxPower(PD.activePos) * 1000000) / PD.activeV;
    return (i > PD_AVS_MAX_CURRENT) ? PD_AVS_MAX_CURRENT : i;
  }
  return PD_getPDOMaxCurrent(PD.activePos);
}

// Check if an explicit contract is established
uint8_t PD_isConnected(void) {
  return PD.hasContract;
}

// Get version counter of the capabilities (changes whenever new caps were received)
uint8_t PD_getCapsVersion(void) {
  return PD.capsVersion;
}

// Get result of last request
uint8_t PD_getResult(void) {
  return PD.result;
}

// Get negotiated PD revision (2 or 3)
uint8_t PD_getRevision(void) {
  return PD.rev + 1;
}

// Check if source is EPR capable
uint8_t PD_isEPRCapable(void) {
  return PD.srcEPRCapable;
}

// Check if EPR mode is active
uint8_t PD_isEPR(void) {
  return PD.eprMode;
}

// Get EPR state
uint8_t PD_getEPRState(void) {
  return PD.eprState;
}

// Get EPR enter failure reason
uint8_t PD_getEPRFail(void) {
  return PD.eprFail;
}

// ===================================================================================
// USB PD Interrupt Service Routine
// ===================================================================================
void USBPD_IRQHandler(void) __attribute__((interrupt));
void USBPD_IRQHandler(void) {
  USBPD_MessageHeader_t mh;
  uint8_t len, next, i;

  // Receive complete interrupt
  if(USBPD->STATUS & USBPD_IF_RX_ACT) {
    if(((USBPD->STATUS & USBPD_BMC_AUX) == USBPD_BMC_AUX_SOP0) && (USBPD->BMC_BYTE_CNT >= 6)) {
      mh.d16 = PD_TR_buffer[0] | (PD_TR_buffer[1] << 8);
      len    = 2 + (mh.MessageHeader.NumberOfDataObjects << 2);

      if(!mh.MessageHeader.Extended && !mh.MessageHeader.NumberOfDataObjects
         && (mh.MessageHeader.MessageType == USBPD_CONTROL_MSG_GOODCRC)) {
        // GoodCRC for our message
        if(mh.MessageHeader.MessageID == PD_txID) PD_txAcked = 1;
      }

      else if(USBPD->BMC_BYTE_CNT >= len + 4) {
        // Store message (skip retransmissions with same MessageID, except Soft_Reset)
        uint8_t softReset = !mh.MessageHeader.Extended && !mh.MessageHeader.NumberOfDataObjects
                         && (mh.MessageHeader.MessageType == USBPD_CONTROL_MSG_SOFT_RESET);
        if(softReset || (mh.MessageHeader.MessageID != PD_rxLastID)) {
          next = (PD_rxHead + 1) % PD_RXQ_SIZE;
          if(next != PD_rxTail) {
            PD_rxq[PD_rxHead].header = mh.d16;
            for(i = 2; i < len; i++) PD_rxq[PD_rxHead].data[i - 2] = PD_TR_buffer[i];
            PD_rxHead = next;
          }
        }
        PD_rxLastID = mh.MessageHeader.MessageID;

        // Answer with GoodCRC (after interframe gap)
        DLY_us(30);
        {
          USBPD_MessageHeader_t gh;
          gh.d16 = 0;
          gh.MessageHeader.MessageID             = mh.MessageHeader.MessageID;
          gh.MessageHeader.MessageType           = USBPD_CONTROL_MSG_GOODCRC;
          gh.MessageHeader.SpecificationRevision = PD_specRev;
          PD_TR_buffer[0] = gh.d16;
          PD_TR_buffer[1] = gh.d16 >> 8;
          PD_startTX(USBPD_TX_SOP0, 2);
        }
      }
    }
    USBPD->STATUS = USBPD_IF_RX_ACT;                // write 1 to clear only this flag
  }

  // Transmit complete interrupt
  if(USBPD->STATUS & USBPD_IF_TX_END) {
    USBPD->PORT_CC1 &= ~USBPD_CC_LVE;
    USBPD->PORT_CC2 &= ~USBPD_CC_LVE;
    PD_RX_mode();
    USBPD->STATUS = USBPD_IF_TX_END;
  }

  // Hard reset received
  if(USBPD->STATUS & USBPD_IF_RX_RESET) {
    USBPD->STATUS = USBPD_IF_RX_RESET;
    PD_rxLastID   = 0xFF;
    PD_hardResetRcvd = 1;
  }
}
