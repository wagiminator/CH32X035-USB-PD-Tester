// ===================================================================================
// USB PD Constant and Structure Defines (incl. PD 3.1 EPR/AVS)               * v2.0 *
// ===================================================================================
//
// All bit fields are LSB first (GCC RISC-V little endian), matching the PD wire format.
//
// 2023 by Stefan Wagner:   https://github.com/wagiminator
// 2026 PD 3.1 extensions (EPR, AVS, extended messages)

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Get values from PDO representation
#define POWER_DECODE_25MV(value)    ((uint16_t)(((value) * 25)))    // From 25mV  multiples to mV
#define POWER_DECODE_50MV(value)    ((uint16_t)(((value) * 50)))    // From 50mV  multiples to mV
#define POWER_DECODE_100MV(value)   ((uint16_t)(((value) * 100)))   // From 100mV multiples to mV
#define POWER_DECODE_10MA(value)    ((uint16_t)(((value) * 10)))    // From 10mA  multiples to mA
#define POWER_DECODE_50MA(value)    ((uint16_t)(((value) * 50)))    // From 50mA  multiples to mA

// PD PHY Channel
#define USBPD_CCNONE                0x00u
#define USBPD_CC1                   0x01u
#define USBPD_CC2                   0x02u

// USB PD Revision (message header field)
#define USBPD_SPECIFICATION_REV1    0x00u  // Revision 1.0
#define USBPD_SPECIFICATION_REV2    0x01u  // Revision 2.0
#define USBPD_SPECIFICATION_REV3    0x02u  // Revision 3.x (3.0, 3.1, 3.2)

// USB PD Revision aliases
#define USBPD_REVISION_10           0x00u  // Revision 1.0
#define USBPD_REVISION_20           0x01u  // Revision 2.0
#define USBPD_REVISION_30           0x02u  // Revision 3.x

// PDO types (bits 31..30)
#define USBPD_PDO_TYPE_FIXED        0x00u
#define USBPD_PDO_TYPE_BATTERY      0x01u
#define USBPD_PDO_TYPE_VARIABLE     0x02u
#define USBPD_PDO_TYPE_APDO         0x03u

// APDO sub types (bits 29..28)
#define USBPD_APDO_SPR_PPS          0x00u  // SPR Programmable Power Supply (PD 3.0)
#define USBPD_APDO_EPR_AVS          0x01u  // EPR Adjustable Voltage Supply  (PD 3.1)
#define USBPD_APDO_SPR_AVS          0x02u  // SPR Adjustable Voltage Supply  (PD 3.2)

// Number of PDO positions
#define USBPD_SPR_PDO_MAX           7u     // positions 1..7 (SPR)
#define USBPD_EPR_PDO_MAX           13u    // positions 1..13 (SPR + EPR)
#define USBPD_EPR_FIRST_POS         8u     // first EPR position

// Max data bytes of one chunk of an extended message
#define USBPD_MAX_CHUNK_SIZE        26u

// ===================================================================================
// Power Data Objects (PDO)
// ===================================================================================
typedef struct {
  uint32_t MaxCurrentIn10mAunits      : 10u;
  uint32_t VoltageIn50mVunits         : 10u;
  uint32_t PeakCurrent                : 2u;
  uint32_t Reserved_22bit             : 1u;
  uint32_t EPRModeCapable             : 1u;
  uint32_t UnchunkedExtendedMessage   : 1u;
  uint32_t DualRoleData               : 1u;
  uint32_t USBCommunicationsCapable   : 1u;
  uint32_t UnconstrainedPower         : 1u;
  uint32_t USBSuspendSupported        : 1u;
  uint32_t DualRolePower              : 1u;
  uint32_t FixedSupply                : 2u; // 00b
} USBPD_SourceFixedSupplyPDO_t;

typedef struct {
  uint32_t MaxCurrentIn10mAunits      : 10u;
  uint32_t MinVoltageIn50mVunits      : 10u;
  uint32_t MaxVoltageIn50mVunits      : 10u;
  uint32_t VariableSupply             : 2u; // 10b
} USBPD_SourceVariableSupplyPDO_t;

typedef struct {
  uint32_t MaxPowerIn250mWunits       : 10u;
  uint32_t MinVoltageIn50mVunits      : 10u;
  uint32_t MaxVoltageIn50mVunits      : 10u;
  uint32_t Battery                    : 2u; // 01b
} USBPD_SourceBatterySupplyPDO_t;

typedef struct {
  uint32_t MaxCurrentIn50mAincrements : 7u;
  uint32_t Reserved_7bit              : 1u; // shall be set to zero
  uint32_t MinVoltageIn100mVincrements: 8u;
  uint32_t Reserved_16bit             : 1u; // shall be set to zero
  uint32_t MaxVoltageIn100mVincrements: 8u;
  uint32_t Reserved_25_26bit          : 2u; // shall be set to zero
  uint32_t PPSpowerLimited            : 1u;
  uint32_t SPRprogrammablePowerSupply : 2u; // 00b
  uint32_t AugmentedPowerDataObject   : 2u; // 11b
} USBPD_SourcePPSSupplyPDO_t;

typedef struct {
  uint32_t PDPIn1Wincrements          : 8u;
  uint32_t MinVoltageIn100mVincrements: 8u;
  uint32_t Reserved_16bit             : 1u; // shall be set to zero
  uint32_t MaxVoltageIn100mVincrements: 9u;
  uint32_t PeakCurrent                : 2u;
  uint32_t EPRadjustableVoltageSupply : 2u; // 01b
  uint32_t AugmentedPowerDataObject   : 2u; // 11b
} USBPD_SourceEPRSupplyPDO_t;

typedef union {
  uint32_t                        d32;
  USBPD_SourceFixedSupplyPDO_t    SourceFixedPDO;
  USBPD_SourceVariableSupplyPDO_t SourceVariablePDO;
  USBPD_SourceBatterySupplyPDO_t  SourceBatteryPDO;
  USBPD_SourcePPSSupplyPDO_t      SourcePPSPDO;
  USBPD_SourceEPRSupplyPDO_t      SourceEPRPDO;
} USBPD_PDO_t;

// Sink fixed supply PDO (used in Sink_Capabilities)
typedef struct {
  uint32_t OperationalCurrentIn10mA   : 10u;
  uint32_t VoltageIn50mVunits         : 10u;
  uint32_t Reserved_20_22bit          : 3u;
  uint32_t FastRoleSwapCurrent        : 2u;
  uint32_t DualRoleData               : 1u;
  uint32_t USBCommunicationsCapable   : 1u;
  uint32_t UnconstrainedPower         : 1u;
  uint32_t HigherCapability           : 1u;
  uint32_t DualRolePower              : 1u;
  uint32_t FixedSupply                : 2u; // 00b
} USBPD_SinkFixedSupplyPDO_t;

typedef union {
  uint32_t                   d32;
  USBPD_SinkFixedSupplyPDO_t SinkFixedPDO;
} USBPD_SinkPDO_t;

// ===================================================================================
// Message Types
// ===================================================================================
typedef enum {
  USBPD_CONTROL_MSG_GOODCRC               = 0x01u,
  USBPD_CONTROL_MSG_GOTOMIN               = 0x02u,
  USBPD_CONTROL_MSG_ACCEPT                = 0x03u,
  USBPD_CONTROL_MSG_REJECT                = 0x04u,
  USBPD_CONTROL_MSG_PING                  = 0x05u,
  USBPD_CONTROL_MSG_PS_RDY                = 0x06u,
  USBPD_CONTROL_MSG_GET_SRC_CAP           = 0x07u,
  USBPD_CONTROL_MSG_GET_SNK_CAP           = 0x08u,
  USBPD_CONTROL_MSG_DR_SWAP               = 0x09u,
  USBPD_CONTROL_MSG_PR_SWAP               = 0x0Au,
  USBPD_CONTROL_MSG_VCONN_SWAP            = 0x0Bu,
  USBPD_CONTROL_MSG_WAIT                  = 0x0Cu,
  USBPD_CONTROL_MSG_SOFT_RESET            = 0x0Du,
  USBPD_CONTROL_MSG_DATA_RESET            = 0x0Eu,
  USBPD_CONTROL_MSG_DATA_RESET_COMPLETE   = 0x0Fu,
  USBPD_CONTROL_MSG_NOT_SUPPORTED         = 0x10u,
  USBPD_CONTROL_MSG_GET_SRC_CAPEXT        = 0x11u,
  USBPD_CONTROL_MSG_GET_STATUS            = 0x12u,
  USBPD_CONTROL_MSG_FR_SWAP               = 0x13u,
  USBPD_CONTROL_MSG_GET_PPS_STATUS        = 0x14u,
  USBPD_CONTROL_MSG_GET_COUNTRY_CODES     = 0x15u,
  USBPD_CONTROL_MSG_GET_SNK_CAPEXT        = 0x16u,
  USBPD_CONTROL_MSG_GET_SRC_INFO          = 0x17u,
  USBPD_CONTROL_MSG_GET_REVISION          = 0x18u,
} USBPD_ControlMessage_t;

typedef enum {
  USBPD_DATA_MSG_SRC_CAP                  = 0x01u,
  USBPD_DATA_MSG_REQUEST                  = 0x02u,
  USBPD_DATA_MSG_BIST                     = 0x03u,
  USBPD_DATA_MSG_SNK_CAP                  = 0x04u,
  USBPD_DATA_MSG_BATTERY_STATUS           = 0x05u,
  USBPD_DATA_MSG_ALERT                    = 0x06u,
  USBPD_DATA_MSG_GET_COUNTRY_INFO         = 0x07u,
  USBPD_DATA_MSG_ENTER_USB                = 0x08u,
  USBPD_DATA_MSG_EPR_REQUEST              = 0x09u,
  USBPD_DATA_MSG_EPR_MODE                 = 0x0Au,
  USBPD_DATA_MSG_SRC_INFO                 = 0x0Bu,
  USBPD_DATA_MSG_REVISION                 = 0x0Cu,
  USBPD_DATA_MSG_VENDOR_DEFINED           = 0x0Fu
} USBPD_DataMessage_t;

#define USBPD_DATA_MSG_EPR_REUEST   USBPD_DATA_MSG_EPR_REQUEST  // legacy alias (typo)

typedef enum {
  USBPD_EXT_MSG_SRC_CAP_EXT               = 0x01u,
  USBPD_EXT_MSG_STATUS                    = 0x02u,
  USBPD_EXT_MSG_GET_BATTERY_CAP           = 0x03u,
  USBPD_EXT_MSG_GET_BATTERY_STATUS        = 0x04u,
  USBPD_EXT_MSG_BATTERY_CAP               = 0x05u,
  USBPD_EXT_MSG_GET_MANUFACTURER_INFO     = 0x06u,
  USBPD_EXT_MSG_MANUFACTURER_INFO         = 0x07u,
  USBPD_EXT_MSG_SECURITY_REQUEST          = 0x08u,
  USBPD_EXT_MSG_SECURITY_RESPONSE         = 0x09u,
  USBPD_EXT_MSG_FW_UPDATE_REQUEST         = 0x0Au,
  USBPD_EXT_MSG_FW_UPDATE_RESPONSE        = 0x0Bu,
  USBPD_EXT_MSG_PPS_STATUS                = 0x0Cu,
  USBPD_EXT_MSG_COUNTRY_INFO              = 0x0Du,
  USBPD_EXT_MSG_COUNTRY_CODES             = 0x0Eu,
  USBPD_EXT_MSG_SNK_CAP_EXT               = 0x0Fu,
  USBPD_EXT_MSG_EXTENDED_CONTROL          = 0x10u,
  USBPD_EXT_MSG_EPR_SRC_CAP               = 0x11u,
  USBPD_EXT_MSG_EPR_SNK_CAP               = 0x12u,
} USBPD_ExtendedMessage_t;

// Extended_Control message data block types (ECDB)
#define USBPD_ECDB_EPR_GET_SRC_CAP  0x01u
#define USBPD_ECDB_EPR_GET_SNK_CAP  0x02u
#define USBPD_ECDB_EPR_KEEPALIVE    0x03u
#define USBPD_ECDB_EPR_KEEPALIVE_ACK 0x04u

// EPR_Mode data object (EPRMDO) actions (bits 31..24)
#define USBPD_EPR_ACTION_ENTER      0x01u
#define USBPD_EPR_ACTION_ENTER_ACK  0x02u
#define USBPD_EPR_ACTION_ENTER_OK   0x03u
#define USBPD_EPR_ACTION_ENTER_FAIL 0x04u
#define USBPD_EPR_ACTION_EXIT       0x05u

// EPR_Mode "Enter Failed" reasons (data field bits 23..16)
#define USBPD_EPR_FAIL_UNKNOWN      0x00u
#define USBPD_EPR_FAIL_CABLE        0x01u  // cable not EPR capable
#define USBPD_EPR_FAIL_VCONN        0x02u  // source failed to become VCONN source
#define USBPD_EPR_FAIL_RDO          0x03u  // EPR capable bit not set in RDO
#define USBPD_EPR_FAIL_SOURCE       0x04u  // source unable to enter EPR mode
#define USBPD_EPR_FAIL_PDO          0x05u  // EPR capable bit not set in PDO

typedef union {
  uint16_t               d16;
  USBPD_DataMessage_t    DataMessage;
  USBPD_ControlMessage_t ControlMessage;
} USBPD_MessageType_t;

// ===================================================================================
// Message Header and Extended Message Header
// ===================================================================================
typedef struct {
  uint16_t MessageType               : 5u;
  uint16_t PortDataRole              : 1u; // 0b->UFP, 1b->DFP
  uint16_t SpecificationRevision     : 2u; // 00b->v1.0, 01b->v2.0, 10b->v3.x
  uint16_t PortPowerRole             : 1u; // 0b->sink, 1b->source
  uint16_t MessageID                 : 3u;
  uint16_t NumberOfDataObjects       : 3u;
  uint16_t Extended                  : 1u;
} USBPD_MessageHeader_tt;

typedef union {
  uint16_t               d16;
  USBPD_MessageHeader_tt MessageHeader;
} USBPD_MessageHeader_t;

typedef struct {
  uint16_t DataSize                  : 9u; // total number of data bytes (all chunks)
  uint16_t Reserved_9bit             : 1u;
  uint16_t RequestChunk              : 1u;
  uint16_t ChunkNumber               : 4u;
  uint16_t Chunked                   : 1u;
} USBPD_ExtendedHeader_tt;

typedef union {
  uint16_t                d16;
  USBPD_ExtendedHeader_tt ExtendedHeader;
} USBPD_ExtendedHeader_t;

// ===================================================================================
// Request Data Objects (RDO)
// ===================================================================================
typedef struct {
  uint32_t MaxOperatingCurrent10mAunits : 10u;
  uint32_t OperatingCurrentIn10mAunits  : 10u;
  uint32_t Reserved20_21                : 2u; // 00b
  uint32_t EPRModeCapable               : 1u;
  uint32_t UnchunkedExtendedMessage     : 1u;
  uint32_t NoUSBSuspend                 : 1u;
  uint32_t USBCommunicationsCapable     : 1u;
  uint32_t CapabilityMismatch           : 1u;
  uint32_t GiveBackFlag                 : 1u;
  uint32_t ObjectPosition               : 4u;
} USBPD_SinkFixedVariableRDO_t;

typedef struct {
  uint32_t MaxOperatingPower250mWunits  : 10u;
  uint32_t OperatingPowerIn250mWunits   : 10u;
  uint32_t Reserved20_21                : 2u; // 00b
  uint32_t EPRModeCapable               : 1u;
  uint32_t UnchunkedExtendedMessage     : 1u;
  uint32_t NoUSBSuspend                 : 1u;
  uint32_t USBCommunicationsCapable     : 1u;
  uint32_t CapabilityMismatch           : 1u;
  uint32_t GiveBackFlag                 : 1u;
  uint32_t ObjectPosition               : 4u;
} USBPD_SinkBatteryRDO_t;

// PPS RDO (voltage in 20mV units) and AVS RDO (voltage in 25mV units, 2 LSBs = 0)
typedef struct {
  uint32_t OperatingCurrentIn50mAunits  : 7u;
  uint32_t Reserved7_8                  : 2u; // shall be set to zero
  uint32_t OutputVoltage                : 12u;
  uint32_t Reserved21                   : 1u; // shall be set to zero
  uint32_t EPRModeCapable               : 1u;
  uint32_t UnchunkedExtendedMessage     : 1u;
  uint32_t NoUSBSuspend                 : 1u;
  uint32_t USBCommunicationsCapable     : 1u;
  uint32_t CapabilityMismatch           : 1u;
  uint32_t Reserved27                   : 1u; // shall be set to zero
  uint32_t ObjectPosition               : 4u;
} USBPD_SinkPPSRDO_t;

typedef union {
  uint32_t                     d32;
  USBPD_SinkFixedVariableRDO_t SinkFixedVariableRDO;
  USBPD_SinkBatteryRDO_t       SinkBatteryRDO;
  USBPD_SinkPPSRDO_t           SinkPPSRDO;
  USBPD_SinkPPSRDO_t           SinkAVSRDO;
} USBPD_SINKRDO_t;

// ===================================================================================
// EPR Mode Data Object (EPRMDO) and Revision Message Data Object (RMDO)
// ===================================================================================
typedef struct {
  uint32_t Reserved                     : 16u;
  uint32_t Data                         : 8u;
  uint32_t Action                       : 8u;
} USBPD_EPRModeDO_tt;

typedef union {
  uint32_t           d32;
  USBPD_EPRModeDO_tt EPRMode;
} USBPD_EPRModeDO_t;

typedef struct {
  uint32_t Reserved                     : 16u;
  uint32_t VersionMinor                 : 4u;
  uint32_t VersionMajor                 : 4u;
  uint32_t RevisionMinor                : 4u;
  uint32_t RevisionMajor                : 4u;
} USBPD_RevisionDO_tt;

typedef union {
  uint32_t            d32;
  USBPD_RevisionDO_tt Revision;
} USBPD_RevisionDO_t;

#ifdef __cplusplus
}
#endif
