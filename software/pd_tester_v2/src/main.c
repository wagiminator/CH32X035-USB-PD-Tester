// ===================================================================================
// Project:   USB PD Tester for CH32X035
// Version:   v2.0 (USB PD 3.1: EPR and AVS)
// Year:      2024, 2026
// Author:    Stefan Wagner
// Github:    https://github.com/wagiminator
// EasyEDA:   https://easyeda.com/wagiminator
// License:   http://creativecommons.org/licenses/by-sa/3.0/
// ===================================================================================
//
// Description:
// ------------
// The USB PD Tester allows users to retrieve and test the capabilities of a connected
// USB Power Delivery Adapter. It supports fixed, variable and battery PDOs, SPR PPS
// (PD 3.0) as well as EPR mode with 28V/36V/48V fixed PDOs and AVS (PD 3.1).
//
// Display:
// --------
// Each line shows one PDO:   *nnT# <description>      <
//                            |  || |                  +-- cursor
//                            |  || +-- voltage/current or range/current or power
//                            |  |+-- '#': locked (above PD_VOLTAGE_LIMIT / needs EPR)
//                            |  +-- type: F=fixed V=variable B=battery P=PPS A=AVS
//                            +-- '*': active PDO, nn: object position (8..13 = EPR)
// The bottom line shows the PD revision, the EPR state and the voltage limit, or
// the result of the last action.
//
// Operating Instructions:
// -----------------------
// - UP/DOWN: select PDO (list scrolls if there are more than 7 PDOs)
// - SLCT on fixed/variable/battery PDO: request it
// - SLCT on PPS/AVS PDO: open adjust screen, set voltage with UP/DOWN (hold for fast
//   steps), apply with SLCT
//
// References:
// -----------
// - WCH Nanjing Qinheng Microelectronics: http://wch.cn
// - USB Power Delivery Specification Rev. 3.1
//
// Compilation Instructions:
// -------------------------
// - Make sure GCC toolchain (gcc-riscv64-unknown-elf, newlib) and Python3 with chprog
//   are installed. In addition, Linux requires access rights to the USB bootloader.
// - Press the BOOT button on the MCU board and keep it pressed while connecting it
//   via USB to your PC.
// - Run 'make flash'.


// ===================================================================================
// Libraries, Definitions and Macros
// ===================================================================================
#include <config.h>             // user configurations
#include <ssd1306_txt.h>        // OLED text functions
#include <usbpd_sink.h>         // USB PD sink functions

#define LIST_ROWS     7         // number of PDO lines on the screen
#define STATUS_ROW    7         // screen line for status messages
#define LINE_LEN      21        // characters per line
#define MSG_TIME      2500      // display time of status messages in ms
#define KEY_DEBOUNCE  20        // key debounce time in ms
#define KEY_DELAY     400       // time until key repeat starts in ms
#define KEY_REPEAT    60        // key repeat interval in ms
#define KEY_FAST      2000      // time until fast steps (x10) in ms

enum {KEY_NONE = 0, KEY_UP, KEY_DOWN, KEY_SLCT};

// Global variables
static char     line[LINE_LEN + 1];              // line buffer
static uint8_t  items[USBPD_EPR_PDO_MAX];        // PDO positions shown in the list
static uint8_t  itemNum;                         // number of list entries
static uint8_t  cursor;                          // selected list entry
static uint8_t  top;                             // first visible list entry
static uint8_t  msgActive;                       // status message is shown
static uint32_t msgTime;                         // time of status message

// ===================================================================================
// Time and Keys
// ===================================================================================

// Milliseconds since start (SysTick based)
static uint32_t millis(void) {
  static uint32_t ms, tick;
  uint32_t d = (STK->CNTL - tick) / DLY_MS_TIME;
  tick += d * DLY_MS_TIME;
  ms   += d;
  return ms;
}

// Get debounced key event; UP/DOWN repeat while held, *fast is set after KEY_FAST ms
static uint8_t KEY_get(uint8_t* fast) {
  static uint8_t  keyRaw, keyLast;
  static uint32_t keyRawTime, keyTime, keyNext;
  uint32_t now = millis();
  uint8_t  key = KEY_NONE;

  if     (!PIN_read(PIN_KEY_UP))   key = KEY_UP;
  else if(!PIN_read(PIN_KEY_DOWN)) key = KEY_DOWN;
  else if(!PIN_read(PIN_KEY_SLCT)) key = KEY_SLCT;
  *fast = 0;

  if(key != keyRaw) {                            // debounce
    keyRaw     = key;
    keyRawTime = now;
  }
  if((now - keyRawTime) < KEY_DEBOUNCE) return KEY_NONE;

  if(key != keyLast) {                           // new key pressed (or released)
    keyLast = key;
    keyTime = now;
    keyNext = now + KEY_DELAY;
    return key;
  }
  if(((key == KEY_UP) || (key == KEY_DOWN)) && ((int32_t)(now - keyNext) >= 0)) {
    keyNext = now + KEY_REPEAT;
    *fast   = ((now - keyTime) > KEY_FAST);
    return key;
  }
  return KEY_NONE;
}

// ===================================================================================
// Text Formatting
// ===================================================================================

// Format milli-unit value as decimal with dec (0..2) decimals and unit; returns length
static uint8_t fmtValue(char* s, uint32_t milli, uint8_t dec, char unit) {
  char     tmp[10];
  uint8_t  n = 0, i = 0;
  uint32_t ip = milli / 1000;
  uint32_t fp = milli % 1000;
  do { tmp[n++] = '0' + ip % 10; ip /= 10; } while(ip);
  while(n) s[i++] = tmp[--n];
  if(dec == 1) {
    s[i++] = '.';
    s[i++] = '0' + fp / 100;
  }
  else if(dec == 2) {
    fp /= 10;
    s[i++] = '.';
    s[i++] = '0' + fp / 10;
    s[i++] = '0' + fp % 10;
  }
  if(unit) s[i++] = unit;
  s[i] = 0;
  return i;
}

// Format voltage range "min-maxV"; returns length
static uint8_t fmtRange(char* s, uint16_t vmin, uint16_t vmax) {
  uint8_t n = fmtValue(s, vmin, 1, 0);
  s[n++] = '-';
  return n + fmtValue(s + n, vmax, 1, 'V');
}

// Clear line buffer
static void LINE_clear(void) {
  uint8_t i;
  for(i = 0; i < LINE_LEN; i++) line[i] = ' ';
  line[LINE_LEN] = 0;
}

// Put string into line buffer at column
static void LINE_put(uint8_t col, const char* s) {
  while(*s && (col < LINE_LEN)) line[col++] = *s++;
}

// Put string into line buffer, right aligned so that it ends at column end
static void LINE_putRight(uint8_t end, const char* s, uint8_t len) {
  LINE_put(end + 1 - len, s);
}

// Print line buffer on screen line y and keep the PD protocol alive
static void LINE_show(uint8_t y) {
  OLED_cursor(0, y);
  OLED_print(line);
  PD_update();
}

// ===================================================================================
// Status Line
// ===================================================================================

// Show status message (or default status if msg == 0)
static void STATUS_show(const char* msg) {
  char    s[12];
  uint8_t n;
  LINE_clear();
  if(msg) {
    LINE_put(0, msg);
    msgActive = 1;
    msgTime   = millis();
  }
  else if(!PD_isConnected()) {
    LINE_put(0, "No USB PD source");
    msgActive = 0;
  }
  else {
    if(PD_getRevision() >= 3) LINE_put(0, PD_isEPRCapable() ? "PD3.1" : "PD3.0");
    else                      LINE_put(0, "PD2.0");
    switch(PD_getEPRState()) {
      case PD_EPR_ACTIVE:   LINE_put(6, "EPR");  break;
      case PD_EPR_ENTERING: LINE_put(6, "EPR?"); break;
      case PD_EPR_FAILED:   LINE_put(6, "EPR!"); break;
      default:              LINE_put(6, "SPR");  break;
    }
    n = fmtValue(s, PD_VOLTAGE_LIMIT, 1, 'V');
    LINE_putRight(LINE_LEN - 1, s, n);
    LINE_putRight(LINE_LEN - 1 - n, "Lim", 3);
    msgActive = 0;
  }
  LINE_show(STATUS_ROW);
}

// Show result of last request
static void STATUS_result(void) {
  char    s[LINE_LEN + 1];
  uint8_t n;
  switch(PD_getResult()) {
    case PD_RES_OK:
      s[0] = 'O'; s[1] = 'K'; s[2] = ' ';
      n = 3 + fmtValue(s + 3, PD_getVoltage(), 2, 'V');
      s[n++] = ' ';
      fmtValue(s + n, PD_getCurrent(), 2, 'A');
      STATUS_show(s);
      break;
    case PD_RES_REJECT:  STATUS_show("Rejected by source");  break;
    case PD_RES_WAIT:    STATUS_show("Source busy (Wait)");  break;
    case PD_RES_TIMEOUT: STATUS_show("No answer (timeout)"); break;
    case PD_RES_RESET:   STATUS_show("Source reset");        break;
    default:             STATUS_show("Not selectable");      break;
  }
}

// Show reason of failed EPR entry
static void STATUS_eprFail(void) {
  switch(PD_getEPRFail()) {
    case USBPD_EPR_FAIL_CABLE:  STATUS_show("EPR fail: cable");  break;
    case USBPD_EPR_FAIL_VCONN:  STATUS_show("EPR fail: VCONN");  break;
    case USBPD_EPR_FAIL_SOURCE: STATUS_show("EPR fail: source"); break;
    case USBPD_EPR_FAIL_RDO:
    case USBPD_EPR_FAIL_PDO:    STATUS_show("EPR fail: flags");  break;
    default:                    STATUS_show("EPR fail");         break;
  }
}

// ===================================================================================
// PDO List
// ===================================================================================

// Build list of non-empty PDO positions (keep cursor on the same PDO if possible)
static void LIST_build(void) {
  uint8_t i, old = itemNum ? items[cursor] : 0;
  itemNum = 0;
  cursor  = 0;
  for(i = 1; i <= PD_getPDONum(); i++) {
    if(PD_getPDOType(i) == PD_PDO_EMPTY) continue;
    if(i == old) cursor = itemNum;
    items[itemNum++] = i;
  }
  if(cursor < top) top = cursor;
  if(cursor >= top + LIST_ROWS) top = cursor - LIST_ROWS + 1;
  if(top + LIST_ROWS > itemNum) top = (itemNum > LIST_ROWS) ? itemNum - LIST_ROWS : 0;
}

// Build text line for list entry idx
static void LIST_buildLine(uint8_t idx) {
  char    a[12], b[8];
  uint8_t la, lb, pos, type;

  LINE_clear();
  if(idx >= itemNum) return;
  pos  = items[idx];
  type = PD_getPDOType(pos);

  if(PD_isConnected() && (pos == PD_getPDO())) line[0] = '*';
  if(pos >= 10) line[1] = '1';
  line[2] = '0' + pos % 10;
  line[3] = "-FVBPA?"[type];
  if(!PD_isPDOSelectable(pos)) line[4] = '#';
  if(idx == cursor) line[LINE_LEN - 1] = '<';

  switch(type) {
    case PD_PDO_FIXED:
      la = fmtValue(a, PD_getPDOVoltage(pos), 2, 'V');
      lb = fmtValue(b, PD_getPDOMaxCurrent(pos), 2, 'A');
      LINE_putRight(11, a, la);
      LINE_putRight(19, b, lb);
      return;
    case PD_PDO_VARIABLE:
    case PD_PDO_PPS:
      fmtRange(a, PD_getPDOMinVoltage(pos), PD_getPDOMaxVoltage(pos));
      lb = fmtValue(b, PD_getPDOMaxCurrent(pos), 2, 'A');
      break;
    case PD_PDO_BATTERY:
    case PD_PDO_AVS:
      fmtRange(a, PD_getPDOMinVoltage(pos), PD_getPDOMaxVoltage(pos));
      lb = fmtValue(b, (uint32_t)PD_getPDOMaxPower(pos) * 1000, 0, 'W');
      break;
    default:
      LINE_put(5, "unknown APDO");
      return;
  }
  LINE_put(5, a);
  LINE_putRight(19, b, lb);
}

// Draw one list entry if visible
static void LIST_drawEntry(uint8_t idx) {
  if((idx < top) || (idx >= top + LIST_ROWS)) return;
  LIST_buildLine(idx);
  LINE_show(idx - top);
}

// Draw all visible list entries
static void LIST_draw(void) {
  uint8_t r;
  for(r = 0; r < LIST_ROWS; r++) {
    LIST_buildLine(top + r);
    LINE_show(r);
  }
}

// Move cursor (with wrap around and scrolling)
static void LIST_move(int8_t dir) {
  uint8_t old = cursor;
  if(!itemNum) return;
  if(dir > 0) cursor = (cursor + 1 >= itemNum) ? 0 : cursor + 1;
  else        cursor = cursor ? cursor - 1 : itemNum - 1;
  if(cursor < top)                    { top = cursor;                 LIST_draw(); }
  else if(cursor >= top + LIST_ROWS)  { top = cursor - LIST_ROWS + 1; LIST_draw(); }
  else { LIST_drawEntry(old); LIST_drawEntry(cursor); }
  if(msgActive) STATUS_show(0);
}

// Redraw complete main screen
static void SCREEN_main(void) {
  OLED_clear();
  PD_update();
  LIST_draw();
  STATUS_show(0);
}

// ===================================================================================
// Adjust Screen for PPS and AVS
// ===================================================================================

// Show voltage and resulting current of adjust screen
static void ADJUST_showValue(uint8_t pos, uint16_t v) {
  char     s[10];
  uint8_t  n;
  uint32_t i = PD_getPDOMaxCurrent(pos);
  if(PD_getPDOType(pos) == PD_PDO_AVS) {
    i = ((uint32_t)PD_getPDOMaxPower(pos) * 1000000) / v;
    if(i > 5000) i = 5000;
  }
  LINE_clear();
  LINE_put(0, "Imax:");
  n = fmtValue(s, i, 2, 'A');
  LINE_putRight(13, s, n);
  LINE_show(4);

  LINE_clear();
  n = fmtValue(s, v, 2, 'V');
  LINE_put(4, ">");
  LINE_putRight(13, s, n);
  LINE_put(16, "<");
  LINE_show(6);
}

// Show labeled voltage line
static void ADJUST_showLine(uint8_t y, const char* label, uint16_t v) {
  char    s[10];
  uint8_t n = fmtValue(s, v, 2, 'V');
  LINE_clear();
  LINE_put(0, label);
  LINE_putRight(13, s, n);
  LINE_show(y);
}

// Adjust and request voltage of PPS/AVS PDO
static void ADJUST_run(uint8_t pos) {
  uint8_t  key, fast;
  uint16_t vmin  = PD_getPDOMinVoltage(pos);
  uint16_t vmax  = PD_getPDOLimitVoltage(pos);
  uint16_t step  = PD_getVoltageStep(pos);
  uint16_t delta;
  uint16_t v     = (PD_isConnected() && (pos == PD_getPDO())) ? PD_getVoltage() : vmin;
  if(v < vmin) v = vmin;
  if(v > vmax) v = vmax;

  OLED_clear();
  LINE_clear();
  LINE_put(0, "Adjust PDO");
  line[11] = (pos >= 10) ? '1' : ' ';
  line[12] = '0' + pos % 10;
  LINE_put(14, (PD_getPDOType(pos) == PD_PDO_AVS) ? "(AVS)" : "(PPS)");
  LINE_show(0);
  ADJUST_showLine(1, "Min:",  vmin);
  ADJUST_showLine(2, "Max:",  PD_getPDOMaxVoltage(pos));
  ADJUST_showLine(3, "Limit:", vmax);
  ADJUST_showValue(pos, v);
  LINE_clear();
  LINE_put(0, "UP/DN: set SLCT: ok");
  LINE_show(7);

  while(1) {
    PD_update();
    key = KEY_get(&fast);
    if(key == KEY_SLCT) break;
    if((key == KEY_UP) || (key == KEY_DOWN)) {
      delta = fast ? step * 10 : step;
      if(key == KEY_UP) v = (v + delta > vmax) ? vmax : v + delta;
      else              v = (v < vmin + delta) ? vmin : v - delta;
      ADJUST_showValue(pos, v);
    }
    DLY_ms(1);
  }

  SCREEN_main();
  STATUS_show("Requesting...");
  PD_setPDO(pos, v);
  LIST_draw();
  STATUS_result();
}

// ===================================================================================
// Main Function
// ===================================================================================
int main(void) {
  uint8_t  key, fast, pos;
  uint8_t  lastCaps = 0, lastPos = 0, lastEPR = 0xFF, lastConn = 0;
  uint16_t lastV = 0;

  // Setup OLED and USB-PD
  #if DISABLE_SWJ
  RCC->APB2PCENR |= RCC_AFIOEN;        // enable AFIO clock
  AFIO->PCFR1 |= AFIO_PCFR1_SWJ_CFG_2; // disable SWJ on pins PC18 and PC19
  #endif

  // Setup button pins
  PIN_input_PU(PIN_KEY_UP);
  PIN_input_PU(PIN_KEY_DOWN);
  PIN_input_PU(PIN_KEY_SLCT);

  OLED_init();
  OLED_clear();
  OLED_print("Connecting...");
  PD_connect();

  // Show capabilities
  LIST_build();
  SCREEN_main();
  if(PD_getEPRState() == PD_EPR_FAILED) STATUS_eprFail();
  lastCaps = PD_getCapsVersion();
  lastEPR  = PD_getEPRState();
  lastConn = PD_isConnected();
  lastPos  = PD_getPDO();
  lastV    = PD_getVoltage();

  // Loop
  while(1) {
    PD_update();

    // Redraw if capabilities, contract or EPR state changed
    if((PD_getCapsVersion() != lastCaps) || (PD_isConnected() != lastConn)) {
      lastCaps = PD_getCapsVersion();
      lastConn = PD_isConnected();
      LIST_build();
      SCREEN_main();
    }
    if((PD_getPDO() != lastPos) || (PD_getVoltage() != lastV)) {
      lastPos = PD_getPDO();
      lastV   = PD_getVoltage();
      LIST_draw();
    }
    if(PD_getEPRState() != lastEPR) {
      lastEPR = PD_getEPRState();
      if(lastEPR == PD_EPR_FAILED) STATUS_eprFail();
      else if(!msgActive) STATUS_show(0);
    }
    if(msgActive && ((millis() - msgTime) > MSG_TIME)) STATUS_show(0);

    // Handle keys
    key = KEY_get(&fast);
    if(key == KEY_UP)   LIST_move(-1);
    if(key == KEY_DOWN) LIST_move(+1);
    if((key == KEY_SLCT) && itemNum) {
      pos = items[cursor];
      if(!PD_isPDOSelectable(pos)) {
        if((pos >= USBPD_EPR_FIRST_POS) || (PD_getPDOType(pos) == PD_PDO_AVS))
             STATUS_show(PD_isEPR() ? "Above voltage limit" : "Needs EPR mode");
        else if(PD_getPDOType(pos) == PD_PDO_UNKNOWN) STATUS_show("Not supported");
        else STATUS_show("Above voltage limit");
      }
      else if(PD_getVoltageStep(pos)) ADJUST_run(pos);
      else {
        STATUS_show("Requesting...");
        PD_setPDO(pos, PD_getPDOMinVoltage(pos));
        STATUS_result();
      }
    }

    DLY_ms(1);
  }
}
