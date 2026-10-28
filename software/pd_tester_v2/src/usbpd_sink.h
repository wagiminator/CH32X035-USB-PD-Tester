// ===================================================================================
// USB PD SINK Handler for CH32X035 (PD 3.1 with EPR and AVS)                 * v2.0 *
// ===================================================================================
//
// Functions available:
// --------------------
// PD_connect()             Initialize USB-PD, connect and (optionally) enter EPR mode,
//                          returns 0 if failed
// PD_update()              Non-blocking protocol handler, call at least every 5ms
// PD_negotiate()           Run PD_update() until idle, returns 0 on timeout
// PD_setPDO(p, mV)         Request PDO p (1..13) with voltage mV (PPS/AVS), returns 0 if failed
// PD_setVoltage(mV)        Request specified voltage in millivolts, returns 0 if failed
//
// PD_getPDONum()           Get number of PDO positions (incl. empty EPR padding slots)
// PD_getFixedNum()         Get number of fixed power PDOs
// PD_getPPSNum()           Get number of programmable power PDOs (SPR PPS)
// PD_getAVSNum()           Get number of adjustable voltage PDOs (EPR AVS)
//
// PD_getPDOType(p)         Get type of PDO p (PD_PDO_FIXED, PD_PDO_PPS, PD_PDO_AVS, ...)
// PD_getPDOVoltage(p)      Get voltage of specified fixed power PDO
// PD_getPDOMinVoltage(p)   Get min voltage of specified PDO
// PD_getPDOMaxVoltage(p)   Get max voltage of specified PDO (as offered by the source)
// PD_getPDOMaxCurrent(p)   Get max current of specified PDO in mA (AVS: at min voltage)
// PD_getPDOMaxPower(p)     Get max power of specified PDO in W (battery, AVS)
// PD_getPDOLimitVoltage(p) Get highest voltage of PDO usable with PD_VOLTAGE_LIMIT
// PD_isPDOSelectable(p)    Check if PDO can be requested (type, voltage limit, EPR mode)
// PD_getVoltageStep(p)     Get voltage step size of PDO in mV (PPS: 20mV, AVS: 100mV)
//
// PD_getPDO()              Get active PDO
// PD_getVoltage()          Get active voltage
// PD_getCurrent()          Get active max current
// PD_isConnected()         Check if an explicit contract is established
// PD_getCapsVersion()      Get counter that changes with every new capabilities set
// PD_getResult()           Get result of last request (PD_RES_...)
// PD_getRevision()         Get negotiated PD revision (2 or 3)
// PD_isEPRCapable()        Check if source is EPR capable
// PD_isEPR()               Check if EPR mode is active
// PD_getEPRState()         Get EPR state (PD_EPR_...)
// PD_getEPRFail()          Get reason of failed EPR entry (USBPD_EPR_FAIL_...)
//
//
// - PD_update() must be called at least every 5 ms (EPR keep alive, PPS refresh, responses)
//
// Reference:               https://github.com/openwch/ch32x035
//                          USB Power Delivery Specification Rev. 3.1
// 2023 by Stefan Wagner:   https://github.com/wagiminator

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "system.h"
#include "usbpd.h"

// ===================================================================================
// Parameters and Checks
// ===================================================================================
#if SYS_USE_VECTORS == 0
  #error Interrupt vector table must be enabled (SYS_USE_VECTORS in system.h)!
#endif

#if   F_CPU == 48000000
  #define USBPD_TMR_TX    (80-1)        // timer value for USB PD BMC TX @ F_CPU=48MHz
  #define USBPD_TMR_RX    (120-1)       // timer value for USB PD BMC RX @ F_CPU=48MHz
#elif F_CPU == 24000000
  #define USBPD_TMR_TX    (40-1)        // timer value for USB PD BMC TX @ F_CPU=24MHz
  #define USBPD_TMR_RX    (60-1)        // timer value for USB PD BMC RX @ F_CPU=24MHz
#elif F_CPU == 12000000
  #define USBPD_TMR_TX    (20-1)        // timer value for USB PD BMC TX @ F_CPU=12MHz
  #define USBPD_TMR_RX    (30-1)        // timer value for USB PD BMC RX @ F_CPU=12MHz
#elif F_CPU ==  6000000
  #define USBPD_TMR_TX    (10-1)        // timer value for USB PD BMC TX @ F_CPU=6MHz
  #define USBPD_TMR_RX    (15-1)        // timer value for USB PD BMC RX @ F_CPU=6MHz
#else
  #error Unsupported system frequency for USBPD!
#endif

// Defaults for user settings (can be overridden in config.h)
#ifndef PD_VOLTAGE_LIMIT
  #define PD_VOLTAGE_LIMIT    20000     // max allowed voltage in mV
#endif
#ifndef PD_EPR_ENABLE
  #define PD_EPR_ENABLE       1         // 1: enter EPR mode if supported
#endif
#ifndef PD_EPR_SINK_PDP
  #define PD_EPR_SINK_PDP     140       // sink operational PDP in W for EPR entry
#endif
#ifndef PD_PPS_STEP
  #define PD_PPS_STEP         20        // PPS voltage step in mV
#endif
#ifndef PD_AVS_STEP
  #define PD_AVS_STEP         100       // AVS voltage step in mV
#endif

#if (PD_VOLTAGE_LIMIT < 5000) || (PD_VOLTAGE_LIMIT > 48000)
  #error PD_VOLTAGE_LIMIT must be between 5000 and 48000 (mV)!
#endif
#if (PD_EPR_SINK_PDP < 1) || (PD_EPR_SINK_PDP > 240)
  #error PD_EPR_SINK_PDP must be between 1 and 240 (W)!
#endif
#if (PD_PPS_STEP < 20) || (PD_PPS_STEP % 20)
  #error PD_PPS_STEP must be a multiple of 20 (mV)!
#endif
#if (PD_AVS_STEP < 100) || (PD_AVS_STEP % 100)
  #error PD_AVS_STEP must be a multiple of 100 (mV)!
#endif

// ===================================================================================
// Type defines
// ===================================================================================
typedef enum {
  PD_PDO_EMPTY = 0,                     // unused position (EPR padding)
  PD_PDO_FIXED,                         // fixed supply (SPR or EPR)
  PD_PDO_VARIABLE,                      // variable supply
  PD_PDO_BATTERY,                       // battery supply
  PD_PDO_PPS,                           // SPR programmable power supply
  PD_PDO_AVS,                           // EPR adjustable voltage supply
  PD_PDO_UNKNOWN                        // unknown/unsupported APDO
} pd_pdo_type_t;

typedef enum {
  PD_RES_NONE = 0,                      // no request yet
  PD_RES_OK,                            // accepted, power supply ready
  PD_RES_REJECT,                        // rejected by source
  PD_RES_WAIT,                          // source answered with Wait
  PD_RES_TIMEOUT,                       // no (complete) answer from source
  PD_RES_INVALID,                       // PDO/voltage not selectable
  PD_RES_RESET                          // source reset during negotiation
} pd_result_t;

typedef enum {
  PD_EPR_OFF = 0,                       // EPR disabled or not attempted yet
  PD_EPR_UNSUPPORTED,                   // source is not EPR capable
  PD_EPR_ENTERING,                      // entering EPR mode
  PD_EPR_ACTIVE,                        // EPR mode active
  PD_EPR_FAILED                         // entering EPR mode failed
} pd_epr_t;

// ===================================================================================
// Functions
// ===================================================================================
uint8_t  PD_connect(void);                      // Initialize PD and connect
void     PD_update(void);                       // Non-blocking protocol handler
uint8_t  PD_negotiate(void);                    // Process until idle or timeout
uint8_t  PD_setVoltage(uint16_t voltage);       // Set specified voltage (in millivolts)
uint8_t  PD_setPDO(uint8_t pdonum, uint16_t voltage);  // Set specified PDO and voltage

uint8_t  PD_getPDONum(void);                    // Get number of PDO positions
uint8_t  PD_getFixedNum(void);                  // Get number of fixed power PDOs
uint8_t  PD_getPPSNum(void);                    // Get number of PPS PDOs
uint8_t  PD_getAVSNum(void);                    // Get number of AVS PDOs

uint8_t  PD_getPDOType(uint8_t pdonum);         // Get type of PDO
uint16_t PD_getPDOVoltage(uint8_t pdonum);      // Get voltage of specified fixed power PDO
uint16_t PD_getPDOMinVoltage(uint8_t pdonum);   // Get minimum voltage of specified PDO
uint16_t PD_getPDOMaxVoltage(uint8_t pdonum);   // Get maximum voltage of specified PDO
uint16_t PD_getPDOMaxCurrent(uint8_t pdonum);   // Get max current of specified PDO
uint16_t PD_getPDOMaxPower(uint8_t pdonum);     // Get max power (W) of specified PDO
uint16_t PD_getPDOLimitVoltage(uint8_t pdonum); // Get max usable voltage (voltage limit)
uint8_t  PD_isPDOSelectable(uint8_t pdonum);    // Check if PDO can be requested
uint16_t PD_getVoltageStep(uint8_t pdonum);     // Get voltage step of PDO in mV

uint8_t  PD_getPDO(void);                       // Get active PDO
uint16_t PD_getVoltage(void);                   // Get active voltage
uint16_t PD_getCurrent(void);                   // Get active max current
uint8_t  PD_isConnected(void);                  // Check if explicit contract exists
uint8_t  PD_getCapsVersion(void);               // Get capabilities version counter
uint8_t  PD_getResult(void);                    // Get result of last request
uint8_t  PD_getRevision(void);                  // Get PD revision (2 or 3)
uint8_t  PD_isEPRCapable(void);                 // Check if source is EPR capable
uint8_t  PD_isEPR(void);                        // Check if EPR mode is active
uint8_t  PD_getEPRState(void);                  // Get EPR state
uint8_t  PD_getEPRFail(void);                   // Get EPR enter failure reason

#ifdef __cplusplus
}
#endif
