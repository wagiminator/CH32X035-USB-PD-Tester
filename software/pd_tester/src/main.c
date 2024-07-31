// ===================================================================================
// Project:   USB PD Tester for CH32X035
// Version:   v1.1
// Year:      2024
// Author:    Stefan Wagner
// Github:    https://github.com/wagiminator
// EasyEDA:   https://easyeda.com/wagiminator
// License:   http://creativecommons.org/licenses/by-sa/3.0/
// ===================================================================================
//
// Description:
// ------------
// The USB PD Tester allows users to retrieve and test the capabilities of a connected
// USB Power Delivery Adapter.
//
// References:
// -----------
// - WCH Nanjing Qinheng Microelectronics: http://wch.cn
//
// Compilation Instructions:
// -------------------------
// - Make sure GCC toolchain (gcc-riscv64-unknown-elf, newlib) and Python3 with PyUSB
//   is installed. In addition, Linux requires access rights to the USB bootloader.
// - Press the BOOT button on the MCU board and keep it pressed while connecting it
//   via USB to your PC.
// - Run 'make flash'.


// ===================================================================================
// Libraries, Definitions and Macros
// ===================================================================================
#include <config.h>             // user configurations
#include <ssd1306_txt.h>        // OLED text functions
#include <usbpd_sink.h>         // USB PD sink functions

// Global variables
uint8_t  select  = 1;           // selected PDO
uint8_t  active  = 1;           // active PDO
uint16_t voltage = 5000;        // selected voltage

// ===================================================================================
// Functions
// ===================================================================================

// Set selected PDO marker
void setSelect(uint8_t pdo) {
  OLED_cursor(120, select - 1); OLED_write(' ');
  if(pdo > PD_getPDONum()) select = 1;
  else if(pdo < 1)         select = PD_getPDONum();
  else                     select = pdo;
  OLED_cursor(120, select - 1); OLED_write('<');
}

// Set active PDO marker
void setActive(uint8_t pdo) {
  OLED_cursor( 0, active - 1); OLED_write(' ');
  active = pdo;
  OLED_cursor( 0, active - 1); OLED_write('*');
}

// Set selected voltage
void setVoltage(uint16_t v) {
  if     (v <= PD_getPDOMinVoltage(select)) voltage = PD_getPDOMinVoltage(select);
  else if(v >= PD_getPDOMaxVoltage(select)) voltage = PD_getPDOMaxVoltage(select);
  else                                      voltage = v;
  OLED_cursor(30, 6); OLED_printf(">%6dmV <", voltage);
}

// Print source capabilities
void printSourceCap(void) {
  uint8_t i;
  OLED_clear();
  for(i = 1; i <= PD_getPDONum(); i++) {
    if(i <= PD_getFixedNum())
      OLED_printf(" (%d)%6dmV %5dmA ", i, PD_getPDOVoltage(i), PD_getPDOMaxCurrent(i));
    else
      OLED_printf(" [%d]%6dmV-%5dmV ", i, PD_getPDOMinVoltage(i), PD_getPDOMaxVoltage(i));
  }
  setSelect(select);
  setActive(active);
}

// Print selected programmable power PDO infos
void printPPS(void) {
  OLED_clear();
  OLED_printf("Select voltage of [%d]\n", select);
  OLED_printf("min voltage:%7dmV", PD_getPDOMinVoltage(select));
  OLED_printf("max voltage:%7dmV", PD_getPDOMaxVoltage(select));
  OLED_printf("max current:%7dmA", PD_getPDOMaxCurrent(select));
  setVoltage(voltage);
}

// ===================================================================================
// Main Function
// ===================================================================================
int main(void) {
  // Variables
  uint8_t i;
  uint8_t keydelay;

  // Setup OLED and USB-PD
  OLED_init();
  OLED_clear();
  OLED_printf("Connecting...");
  if(!PD_connect()) {
    OLED_printf("FAILED");
    while(1);
  }

  // Print source capabilities
  printSourceCap();

  // Setup button pins
  PIN_input_PU(PIN_KEY_UP);
  PIN_input_PU(PIN_KEY_DOWN);
  PIN_input_PU(PIN_KEY_SLCT);
  
  // Loop
  while(1) {

    if(!PIN_read(PIN_KEY_UP)) {
      setSelect(select - 1);
      while(!PIN_read(PIN_KEY_UP));
    }

    if(!PIN_read(PIN_KEY_DOWN)) {
      setSelect(select + 1);
      while(!PIN_read(PIN_KEY_DOWN));
    }

    if(!PIN_read(PIN_KEY_SLCT)) {
      if(select <= PD_getFixedNum()) {
        if(PD_setPDO(select, PD_getPDOVoltage(select)))
          setActive(select);
      }
      else {
        printPPS();
        while(!PIN_read(PIN_KEY_SLCT));
        DLY_ms(10);
        while(PIN_read(PIN_KEY_SLCT)) {

          if(!PIN_read(PIN_KEY_UP)) {
            setVoltage(voltage + 20);
            i = keydelay;
            while((i--) && (!PIN_read(PIN_KEY_UP))) DLY_ms(10);
            keydelay = 2;
          }

          else if(!PIN_read(PIN_KEY_DOWN)) {
            setVoltage(voltage - 20);
            i = keydelay;
            while((i--) && (!PIN_read(PIN_KEY_DOWN))) DLY_ms(10);
            keydelay = 2;
          }

          else keydelay = 50;
        }

        if(PD_setPDO(select, voltage))
          active = select;
        printSourceCap();
      }
      while(!PIN_read(PIN_KEY_SLCT));
    }

    DLY_ms(10);
  }
}
