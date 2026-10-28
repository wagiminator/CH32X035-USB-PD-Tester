// ===================================================================================
// User Configurations
// ===================================================================================

#pragma once

// Pin definitions
#define PIN_KEY_UP          PA0       // pin connected to UP button (active low)
#define PIN_KEY_DOWN        PA1       // pin connected to DOWN button (active low)
#define PIN_KEY_SLCT        PA4       // pin connected to SELECT button (active low)
#define PIN_SCL             PA5       // I2C SCL connected to OLED
#define PIN_SDA             PA6       // I2C SDA connected to OLED
#define PIN_LED             PB1       // pin connected to LED (active low)

// If using PC18 and/or PC19 for OLED, disable SWJ
#define DISABLE_SWJ         0         // 0: normal SWJ pins, 1: OLED pins

// MCU supply voltage
#define USB_VDD             0         // 0: 3.3V, 1: 5V

// -----------------------------------------------------------------------------------
// USB Power Delivery Settings
// -----------------------------------------------------------------------------------

// Upper voltage limit in millivolts (5000..48000).
// PDOs above this limit are shown but locked ('#') and cannot be selected,
// PPS/AVS ranges are capped at this value.
// !!! The original hardware is NOT rated for EPR voltages: C3 (330nF) is a 25V part
// !!! and the 78L05 regulator is rated for 30V max. Keep 20000 unless you modified
// !!! the input stage (e.g. 100V capacitor and a regulator rated for >= 55V).
#define PD_VOLTAGE_LIMIT    20000     // max output voltage in mV

// EPR (Extended Power Range, USB PD 3.1, 28V/36V/48V fixed and 15..48V AVS)
// The tester enters EPR mode automatically if the source supports it. This only
// changes the voltage if you select an EPR PDO, so it is safe to read the EPR
// capabilities even with PD_VOLTAGE_LIMIT = 20000. Requires a 5A EPR cable.
#define PD_EPR_ENABLE       1         // 1: enter EPR mode if supported, 0: SPR only
#define PD_EPR_SINK_PDP     140       // sink operational PDP (W) sent on EPR entry (1..240)

// Voltage step sizes for the programmable supplies
#define PD_PPS_STEP         20        // PPS step in mV (multiple of 20)
#define PD_AVS_STEP         100       // AVS step in mV (multiple of 100)

// Hardware safety check
#if PD_VOLTAGE_LIMIT > 20000
  #warning "PD_VOLTAGE_LIMIT > 20V: make sure the input stage of your board is rated for it!"
#endif
