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

// MCU supply voltage
#define USB_VDD             0         // 0: 3.3V, 1: 5V
