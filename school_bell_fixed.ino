/*
  Automatic School Bell - "Mission Raftaar" - Tech Fair 2026-27   (FIXED VERSION)
  Board : Arduino Uno / Nano (ATmega328P)
  Libs  : LiquidCrystal_I2C, I2C_RTC, LedControl

  Wiring (as documented): SET=D2  UP=D3  DOWN=D4  RELAY=D5 (active LOW)  LED=D6  BUZZER=D7
                          MAX7219 DIN=D8 CS=D9 CLK=D10

  EEPROM layout (AT24C32, 4096 bytes)
    0..1  magic (0xA5,0x5A)     2..3 timetable count     4 default index
    5..   [name x10][bellCount x1][ (hour,min,type) x bellCount ] ... repeated
  NOTE: layout changed (magic header added). On first boot the old data is wiped/formatted.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <I2C_RTC.h>
#include <LedControl.h>
#include <avr/pgmspace.h>
#include <string.h>
#include <stdlib.h>

/* ================= HARDWARE PINS ================= */
#define BTN_SET     2
#define BTN_UP      3      // was 4 (swapped with DOWN vs. documented wiring)
#define BTN_DOWN    4      // was 3
#define RELAY_PIN   5
#define LED_PIN     6
#define BUZZER_PIN  7

#define MATRIX_DIN  10
#define MATRIX_CS   9
#define MATRIX_CLK  8

/* ================= I2C & EEPROM CONFIG ================= */
#define RTC_ADDR          0x68
#define EEPROM_ADDR       0x57
#define EEPROM_SIZE       4096U
#define MAX_BELLS         20
#define TT_NAME_LEN       10
#define MAX_TIMETABLES    50

#define EEPROM_MAGIC_ADDR 0
#define EEPROM_MAGIC_0    0xA5
#define EEPROM_MAGIC_1    0x5A
#define EEPROM_TTCOUNT_L  2
#define EEPROM_TTCOUNT_H  3
#define EEPROM_DEFAULT_TT 4
#define EEPROM_TT_START   5

#define MENU_TIMEOUT_MS   90000UL

/* ================= OBJECTS ================= */
LedControl matrix = LedControl(MATRIX_DIN, MATRIX_CLK, MATRIX_CS, 1);
LiquidCrystal_I2C lcd(0x27, 16, 2);
static DS3231 rtc;

/* ================= TYPES ================= */
struct ButtonState {
  bool raw;
  bool stable;
  unsigned long changeT;
  unsigned long nextRep;
};

enum UiState : uint8_t {
  UI_DASHBOARD,
  UI_MENU,
  UI_SET_DATETIME,
  UI_SET_BELL,
  UI_MANAGE_TIMETABLE,
  UI_AUTO_BELLS,
  UI_EXAM_TIMER,
  UI_TRIGGER_BELL,
  UI_SELFTEST,
  UI_CLEAR_EEPROM
};

enum BellType : uint8_t {
  BELL_PERIOD = 0,
  BELL_PRAYER = 1,
  BELL_RECESS = 2,
  BELL_END    = 3
};

struct ClockTime {
  uint8_t  h, m, s, d, mo;
  uint16_t y;
};

/* ================= GLOBAL STATE ================= */
ButtonState btnSet  = {true, true, 0, 0};
ButtonState btnUp   = {true, true, 0, 0};
ButtonState btnDown = {true, true, 0, 0};
bool evSet = false, evUp = false, evDown = false;   // one-shot button events (set once per loop)
unsigned long lastActivity = 0;
bool keyLedOn = false;
unsigned long keyLedOffAt = 0;

UiState uiState = UI_DASHBOARD;
bool screenChanged = true;

ClockTime nowT = {0, 0, 0, 0, 0, 0};
bool clockOk = false;

uint8_t bellCount = 0;
uint8_t bellHour[MAX_BELLS];
uint8_t bellMin[MAX_BELLS];
uint8_t bellType[MAX_BELLS];

bool bellActive = false;
bool manualRinging = false;
unsigned long bellStartMs = 0;
uint16_t bellDurMs = 3000;
int lastBellMinute = -1;

bool examRunning = false;
bool examScheduled = false;
uint32_t examStartSec = 0;
uint32_t examDurationSec = 0;
uint8_t examStartH = 0;
uint8_t examStartM = 0;
uint8_t examDurH = 3;
uint8_t examDurM = 0;

char lcdShadow[2][16];
bool lcdBlinkState = false;
uint8_t mxShadow[8];

/* ================= 8x8 MATRIX FONT IN FLASH ================= */
const byte font8x8[][8] PROGMEM = {
  {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, // 0
  {0x18,0x38,0x18,0x18,0x18,0x18,0x3C,0x00}, // 1
  {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, // 2
  {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // 3
  {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, // 4
  {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // 5
  {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, // 6
  {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00}, // 7
  {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // 8
  {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, // 9
  {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // 10 = C
  {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, // 11 = R
  {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // 12 = P
  {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}  // 13 = E
};

/* month names live in flash. MUST be file-scope/static: a non-static local PROGMEM array
   is placed on the stack and memcpy_P() then reads garbage from flash. */
static const char MONTHS[] PROGMEM = "JANFEBMARAPRMAYJUNJULAUGSEPOCTNOVDEC";

/* ================= MATRIX HELPERS (shadow buffered) ================= */
/* colBits: bit n = column n  (same convention as the old setLed(row,col,line&(1<<col)) code) */
uint8_t revBits(uint8_t b) {
  uint8_t r = 0;
  for (uint8_t i = 0; i < 8; i++) { r = (r << 1) | (b & 1); b >>= 1; }
  return r;
}

void mxRow(uint8_t row, uint8_t colBits) {
  uint8_t v = revBits(colBits);          // LedControl: column 0 == MSB
  if (mxShadow[row] != v) {
    mxShadow[row] = v;
    matrix.setRow(0, row, v);
  }
}

void mxClear() {
  bool any = false;
  for (uint8_t i = 0; i < 8; i++) if (mxShadow[i]) any = true;
  if (!any) return;
  memset(mxShadow, 0, sizeof(mxShadow));
  matrix.clearDisplay(0);
}

/* ================= LCD HELPERS (only redraw when text changed) ================= */
void lcdClear() {
  lcd.clear();
  memset(lcdShadow, 0, sizeof(lcdShadow));
}

void lcdBlink(bool on) {
  if (on == lcdBlinkState) return;
  lcdBlinkState = on;
  if (on) lcd.blink(); else lcd.noBlink();
}

void lcdPrintRow(uint8_t row, const char *txt) {
  char line[17];
  uint8_t len = strlen(txt);
  for (uint8_t i = 0; i < 16; i++) line[i] = (i < len) ? txt[i] : ' ';
  line[16] = 0;
  if (memcmp(line, lcdShadow[row], 16) == 0) return;
  memcpy(lcdShadow[row], line, 16);
  lcd.setCursor(0, row);
  lcd.print(line);
}

void lcdPrintRow_P(uint8_t row, const __FlashStringHelper *txt) {
  char buf[17];
  strncpy_P(buf, (const char *)txt, 16);
  buf[16] = 0;
  lcdPrintRow(row, buf);
}

void showToast_P(const __FlashStringHelper *line0, const __FlashStringHelper *line1) {
  lcdBlink(false);
  lcdPrintRow_P(0, line0);
  lcdPrintRow_P(1, line1);
  delay(1200);
}

/* ================= RTC: single burst read (no torn h/m/s reads) ================= */
static inline uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }

bool readClock() {
  Wire.beginTransmission((uint8_t)RTC_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)7) != 7) return false;

  uint8_t r[7];
  for (uint8_t i = 0; i < 7; i++) r[i] = Wire.read();

  uint8_t hr = r[2];
  uint8_t h;
  if (hr & 0x40) {                       // 12h mode (should not happen, we force 24h)
    h = bcd2dec(hr & 0x1F) % 12;
    if (hr & 0x20) h += 12;
  } else {
    h = bcd2dec(hr & 0x3F);
  }
  uint8_t s  = bcd2dec(r[0] & 0x7F);
  uint8_t mi = bcd2dec(r[1] & 0x7F);
  uint8_t d  = bcd2dec(r[4] & 0x3F);
  uint8_t mo = bcd2dec(r[5] & 0x1F);
  uint16_t y = 2000 + bcd2dec(r[6]);

  if (s > 59 || mi > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12) return false;

  nowT.h = h; nowT.m = mi; nowT.s = s; nowT.d = d; nowT.mo = mo; nowT.y = y;
  return true;
}

bool rtcPresent() {
  Wire.beginTransmission((uint8_t)RTC_ADDR);
  return Wire.endTransmission() == 0;
}

uint32_t secOfDay() {
  return (uint32_t)nowT.h * 3600UL + (uint32_t)nowT.m * 60UL + (uint32_t)nowT.s;
}

uint8_t daysInMonth(uint8_t mo, uint16_t y) {
  static const uint8_t dm[] PROGMEM = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (mo < 1 || mo > 12) return 31;
  if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) return 29;
  return pgm_read_byte(&dm[mo - 1]);
}

/* ================= BUTTONS: debounced, edge-triggered; UP/DOWN auto-repeat, SET never repeats ================= */
bool scanOne(uint8_t pin, ButtonState &b, bool repeat) {
  bool raw = digitalRead(pin);
  unsigned long ms = millis();
  if (raw != b.raw) { b.raw = raw; b.changeT = ms; }
  if (raw != b.stable && (ms - b.changeT) >= 30) {
    b.stable = raw;
    if (!raw) { b.nextRep = ms + 500; return true; }
    return false;
  }
  if (repeat && !b.stable && (long)(ms - b.nextRep) >= 0) {
    b.nextRep = ms + 150;
    return true;
  }
  return false;
}

void scanButtons() {
  evSet  = scanOne(BTN_SET,  btnSet,  false);
  evUp   = scanOne(BTN_UP,   btnUp,   true);
  evDown = scanOne(BTN_DOWN, btnDown, true);

  unsigned long ms = millis();
  if (evSet || evUp || evDown) {
    lastActivity = ms;
    if (digitalRead(RELAY_PIN) == HIGH) {          // don't disturb LED while bell is ringing
      digitalWrite(LED_PIN, HIGH);
      keyLedOn = true;
      keyLedOffAt = ms + 25;
    }
  }
  if (keyLedOn && (long)(ms - keyLedOffAt) >= 0) {
    keyLedOn = false;
    if (digitalRead(RELAY_PIN) == HIGH) digitalWrite(LED_PIN, LOW);
  }
}

bool bothHeld(uint8_t p1, uint8_t p2, unsigned int holdMs) {
  static unsigned long t0 = 0;
  static bool armed = false;
  if (digitalRead(p1) == LOW && digitalRead(p2) == LOW) {
    if (!armed) { armed = true; t0 = millis(); }
    else if (millis() - t0 >= holdMs) { armed = false; return true; }
  } else {
    armed = false;
  }
  return false;
}

bool cancelRequested() { return bothHeld(BTN_UP, BTN_DOWN, 800); }   // hold UP+DOWN = cancel/back

/* ================= AT24C32 I2C EEPROM ================= */
bool eepromPresent() {
  Wire.beginTransmission((uint8_t)EEPROM_ADDR);
  return Wire.endTransmission() == 0;
}

bool eepromRead(uint16_t addr, uint8_t &out) {
  if (addr >= EEPROM_SIZE) return false;          // never wrap into the header
  Wire.beginTransmission((uint8_t)EEPROM_ADDR);
  Wire.write((uint8_t)(addr >> 8));
  Wire.write((uint8_t)(addr & 0xFF));
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)EEPROM_ADDR, (uint8_t)1) != 1) return false;
  out = Wire.read();
  return true;
}

uint8_t eepromReadByte(uint16_t addr) {
  uint8_t v;
  return eepromRead(addr, v) ? v : 0xFF;
}

/* byte write + ACK polling (tWR can be up to 10 ms on AT24C32 clones; fixed 6 ms was too short) */
bool eepromWriteByte(uint16_t addr, uint8_t data) {
  if (addr >= EEPROM_SIZE) return false;
  uint8_t cur;
  if (eepromRead(addr, cur) && cur == data) return true;   // skip unchanged (only if read really succeeded)

  Wire.beginTransmission((uint8_t)EEPROM_ADDR);
  Wire.write((uint8_t)(addr >> 8));
  Wire.write((uint8_t)(addr & 0xFF));
  Wire.write(data);
  if (Wire.endTransmission() != 0) return false;

  unsigned long t0 = millis();
  for (;;) {                                   // ACK polling
    Wire.beginTransmission((uint8_t)EEPROM_ADDR);
    if (Wire.endTransmission() == 0) return true;
    if (millis() - t0 > 25) return false;
    delayMicroseconds(300);
  }
}

/* ================= TIMETABLE STORAGE ENGINE ================= */
uint16_t getTimetableCount() {
  uint16_t v = eepromReadByte(EEPROM_TTCOUNT_L);
  v |= (uint16_t)eepromReadByte(EEPROM_TTCOUNT_H) << 8;
  if (v == 0xFFFF || v > MAX_TIMETABLES) return 0;
  return v;
}

bool setTimetableCount(uint16_t c) {
  bool a = eepromWriteByte(EEPROM_TTCOUNT_L, c & 0xFF);
  bool b = eepromWriteByte(EEPROM_TTCOUNT_H, (c >> 8) & 0xFF);
  return a && b;
}

uint8_t getDefaultTimetable() {
  uint8_t d = eepromReadByte(EEPROM_DEFAULT_TT);
  return (d == 0xFF) ? 0 : d;
}

bool setDefaultTimetable(uint8_t index) {
  return eepromWriteByte(EEPROM_DEFAULT_TT, index);
}

bool eepromFormatted() {
  return eepromReadByte(EEPROM_MAGIC_ADDR) == EEPROM_MAGIC_0 &&
         eepromReadByte(EEPROM_MAGIC_ADDR + 1) == EEPROM_MAGIC_1;
}

bool eepromFormat() {
  bool ok = true;
  ok &= setTimetableCount(0);
  ok &= eepromWriteByte(EEPROM_DEFAULT_TT, 0);
  ok &= eepromWriteByte(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_0);       // magic last
  ok &= eepromWriteByte(EEPROM_MAGIC_ADDR + 1, EEPROM_MAGIC_1);
  return ok;
}

uint16_t getTimetableAddress(uint16_t index) {
  uint16_t total = getTimetableCount();
  if (index > total) index = total;
  uint16_t addr = EEPROM_TT_START;
  for (uint16_t i = 0; i < index; i++) {
    uint8_t cnt = eepromReadByte(addr + TT_NAME_LEN);
    if (cnt > MAX_BELLS) cnt = 0;
    addr += TT_NAME_LEN + 1 + (uint16_t)cnt * 3;
    if (addr >= EEPROM_SIZE) return EEPROM_SIZE;
  }
  return addr;
}

void readTimetableName(uint16_t index, char *out) {      // out must hold TT_NAME_LEN+1
  out[0] = 0;
  if (index >= getTimetableCount()) return;
  uint16_t addr = getTimetableAddress(index);
  if (addr + TT_NAME_LEN + 1 > EEPROM_SIZE) return;
  uint8_t n = 0;
  for (uint8_t i = 0; i < TT_NAME_LEN; i++) {
    uint8_t c = eepromReadByte(addr + i);
    if (c == 0) break;
    out[n++] = (c >= 32 && c < 127) ? (char)c : '?';
  }
  out[n] = 0;                                            // always terminated
}

uint8_t getTimetableBellCount(uint16_t index) {
  uint16_t addr = getTimetableAddress(index) + TT_NAME_LEN;
  uint8_t cnt;
  if (!eepromRead(addr, cnt) || cnt > MAX_BELLS) return 0;
  return cnt;
}

void readTimetableBell(uint16_t ttIndex, uint8_t bellIndex, uint8_t &h, uint8_t &m, uint8_t &t) {
  uint16_t addr = getTimetableAddress(ttIndex) + TT_NAME_LEN;
  uint8_t cnt = eepromReadByte(addr++);
  if (cnt > MAX_BELLS || bellIndex >= cnt) return;
  addr += (uint16_t)bellIndex * 3;
  h = eepromReadByte(addr++);
  m = eepromReadByte(addr++);
  t = eepromReadByte(addr++);
}

bool loadTimetable(uint16_t index) {
  if (index >= getTimetableCount()) return false;
  uint16_t addr = getTimetableAddress(index) + TT_NAME_LEN;
  uint8_t cnt;
  if (!eepromRead(addr++, cnt) || cnt > MAX_BELLS) return false;
  if (addr + (uint16_t)cnt * 3 > EEPROM_SIZE) return false;

  for (uint8_t i = 0; i < cnt; i++) {
    uint8_t h, m, t;
    if (!eepromRead(addr++, h) || !eepromRead(addr++, m) || !eepromRead(addr++, t) ||
        h > 23 || m > 59 || t > BELL_END) {
      bellCount = 0;                                    // corrupt entry -> never run garbage
      return false;
    }
    bellHour[i] = h; bellMin[i] = m; bellType[i] = t;
  }
  bellCount = cnt;
  return true;
}

void restoreActiveTimetable() {
  uint16_t n = getTimetableCount();
  if (n == 0) { bellCount = 0; return; }
  uint8_t d = getDefaultTimetable();
  if (d >= n) d = 0;
  if (!loadTimetable(d)) bellCount = 0;
}

bool deleteTimetable(uint16_t index) {
  uint16_t total = getTimetableCount();
  if (total == 0 || index >= total) return false;

  uint16_t addrDel  = getTimetableAddress(index);
  uint16_t addrNext = getTimetableAddress(index + 1);
  uint16_t addrEnd  = getTimetableAddress(total);
  if (addrEnd > EEPROM_SIZE || addrNext > addrEnd) return false;

  bool ok = true;
  for (uint16_t a = addrNext; a < addrEnd && ok; a++) {
    ok = eepromWriteByte(addrDel++, eepromReadByte(a));
  }
  if (!ok) return false;

  if (!setTimetableCount(total - 1)) return false;
  uint8_t def = getDefaultTimetable();
  if (def == index) setDefaultTimetable(0);
  else if (def > index) setDefaultTimetable(def - 1);

  restoreActiveTimetable();
  return true;
}

void getMonthName(uint8_t m, char *out) {
  if (m < 1 || m > 12) {
    strcpy_P(out, PSTR("TBL"));
    return;
  }
  memcpy_P(out, MONTHS + (m - 1) * 3, 3);
  out[3] = 0;
}

/* Single pass over EEPROM: finds which suffixes (none, _2, _3 ...) of this month are already used,
   then returns the lowest free one -> NOV, NOV_2, NOV_3 ... (also correct after deletions). */
void makeAutoTimetableName(char *out) {                  // out must hold TT_NAME_LEN+1
  char base[4];
  getMonthName(nowT.mo, base);

  uint32_t used = 0;
  uint16_t n = getTimetableCount();
  uint16_t addr = EEPROM_TT_START;
  for (uint16_t i = 0; i < n && addr < EEPROM_SIZE; i++) {
    char nm[TT_NAME_LEN + 1];
    for (uint8_t j = 0; j < TT_NAME_LEN; j++) nm[j] = (char)eepromReadByte(addr + j);
    nm[TT_NAME_LEN] = 0;
    uint8_t cnt = eepromReadByte(addr + TT_NAME_LEN);
    if (cnt > MAX_BELLS) cnt = 0;
    addr += TT_NAME_LEN + 1 + (uint16_t)cnt * 3;

    if (strncmp(nm, base, 3) == 0) {
      if (nm[3] == 0) used |= 1UL;
      else if (nm[3] == '_') {
        int k = atoi(nm + 4);
        if (k >= 2 && k <= 31) used |= (1UL << (k - 1));
      }
    }
  }
  uint8_t k = 1;
  while (k <= 31 && (used & (1UL << (k - 1)))) k++;
  if (k == 1) strcpy(out, base);
  else snprintf_P(out, TT_NAME_LEN + 1, PSTR("%s_%u"), base, k);
}

/* returns 0 = OK, 1 = memory/table limit, 2 = EEPROM I/O error. New table becomes the default (active) one. */
uint8_t saveCurrentTimetable(const char *name) {
  if (bellCount == 0 || !eepromPresent()) return 2;
  uint16_t ttCount = getTimetableCount();
  if (ttCount >= MAX_TIMETABLES) return 1;

  uint16_t addr = getTimetableAddress(ttCount);
  uint16_t need = TT_NAME_LEN + 1 + (uint16_t)bellCount * 3;
  if (addr + need > EEPROM_SIZE) return 1;

  uint8_t len = strlen(name);
  bool ok = true;
  for (uint8_t i = 0; i < TT_NAME_LEN; i++) {
    ok &= eepromWriteByte(addr++, (i < len) ? (uint8_t)name[i] : 0);
  }
  ok &= eepromWriteByte(addr++, bellCount);
  for (uint8_t i = 0; i < bellCount; i++) {
    ok &= eepromWriteByte(addr++, bellHour[i]);
    ok &= eepromWriteByte(addr++, bellMin[i]);
    ok &= eepromWriteByte(addr++, bellType[i]);
  }
  if (!ok) return 2;

  if (!setTimetableCount(ttCount + 1)) return 2;       // count written LAST (commit)
  if (!setDefaultTimetable((uint8_t)ttCount)) return 2;
  return 0;
}

void showSaveResult(uint8_t r) {
  if (r == 0)      showToast_P(F("TIMETABLE"), F("SAVED!"));
  else if (r == 1) showToast_P(F("MEMORY FULL!"), F("DELETE A TABLE"));
  else             showToast_P(F("EEPROM ERROR"), F("NOT SAVED"));
}

/* ================= BELL OUTPUT ENGINE (non-blocking) ================= */
void outputsOffIfIdle() {
  if (!bellActive && !manualRinging) {
    digitalWrite(RELAY_PIN, HIGH);
    digitalWrite(LED_PIN, LOW);
    noTone(BUZZER_PIN);
  }
}

void startBell(uint16_t durMs, uint16_t freq) {
  bellDurMs = durMs;
  bellStartMs = millis();
  bellActive = true;
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, HIGH);
  tone(BUZZER_PIN, freq);
}

void handleBell() {
  if (bellActive && (millis() - bellStartMs >= bellDurMs)) {
    bellActive = false;
    outputsOffIfIdle();
  }
}

bool bellsSuppressed() {
  return examRunning || uiState == UI_SET_BELL || uiState == UI_AUTO_BELLS;
}

/* fires on the first loop pass of the bell's minute (the old "seconds == 0" window could be missed) */
void checkScheduledBells() {
  int cm = nowT.h * 60 + nowT.m;
  if (cm == lastBellMinute) return;
  lastBellMinute = cm;
  if (bellsSuppressed()) return;
  for (uint8_t i = 0; i < bellCount; i++) {
    if (bellHour[i] == nowT.h && bellMin[i] == nowT.m) {
      startBell(3000, 1000);
      break;
    }
  }
}

void getBellStatus(int &currentBell, int &nextBell, long &countdownSec) {
  currentBell = -1; nextBell = -1; countdownSec = -1;
  if (bellCount == 0) return;
  uint32_t nowSec = secOfDay();
  for (int i = 0; i < bellCount; i++) {
    uint32_t bs = ((uint32_t)bellHour[i] * 60UL + bellMin[i]) * 60UL;
    if (nowSec >= bs) currentBell = i;
    else {
      nextBell = i;
      countdownSec = (long)(bs - nowSec);
      return;
    }
  }
}

char getBellSymbolChar(int index) {
  if (index < 0 || index >= bellCount) return '-';
  if (bellType[index] == BELL_PRAYER) return 'P';
  if (bellType[index] == BELL_RECESS) return 'R';
  if (bellType[index] == BELL_END)    return 'E';
  return 'C';
}

const char *bellTypeName(uint8_t t) {
  return (t == BELL_PRAYER) ? "PRAYER" : (t == BELL_RECESS) ? "RECESS" : (t == BELL_END) ? "END" : "PERIOD";
}

/* ================= MATRIX DRAWING ================= */
void drawChar8x8(uint8_t index) {
  if (index > 13) return;
  for (uint8_t row = 0; row < 8; row++) {
    mxRow(row, pgm_read_byte(&font8x8[index][7 - row]));
  }
}

void drawTwoDigitScroll(uint8_t d1, uint8_t d2) {
  static uint8_t offset = 0;
  static unsigned long t = 0;
  if (millis() - t > 130) { t = millis(); offset = (offset + 1) % 9; }
  for (uint8_t row = 0; row < 8; row++) {
    byte a = pgm_read_byte(&font8x8[d1][7 - row]);
    byte b = pgm_read_byte(&font8x8[d2][7 - row]);
    uint16_t line16 = ((uint16_t)a << 8) | b;
    mxRow(row, (byte)((line16 >> (8 - offset)) & 0xFF));
  }
}

void showCountdownOnMatrix(long sec) {
  if (sec < 0) { mxClear(); return; }
  if (sec > 60) sec = 60;
  int cols = (int)((sec * 8L + 59L) / 60L);              // 1..8 (rounds up so 1 s still shows 1 column)
  if (cols > 8) cols = 8;
  uint8_t mask = (uint8_t)(0xFF00 >> cols);               // lit columns on the right side
  for (uint8_t r = 0; r < 8; r++) mxRow(r, mask);
}

void updateCountdownIndicators(long countdown) {
  static unsigned long t = 0;
  static bool blink = false;
  if (countdown <= 10) {
    if (millis() - t > 200) { t = millis(); blink = !blink; }
    if (blink) showCountdownOnMatrix(countdown);
    else mxClear();
    return;
  }
  showCountdownOnMatrix(countdown);
}

void updateMatrix(int cb, int nb, long cd) {
  if (examRunning) return;                                  // exam screen draws its own pattern
  if (uiState == UI_TRIGGER_BELL || uiState == UI_SELFTEST) return;
  if (uiState == UI_SET_BELL || uiState == UI_AUTO_BELLS || bellCount == 0) { mxClear(); return; }

  if (nb >= 0 && cd >= 1 && cd <= 60) { updateCountdownIndicators(cd); return; }  // last minute before ANY bell
  if (cb < 0)                          { mxClear(); return; }
  if (nb < 0 && !bellActive)           { mxClear(); return; }                      // day finished

  uint8_t t = bellType[cb];
  if (t == BELL_PRAYER)      drawChar8x8(12);
  else if (t == BELL_RECESS) drawChar8x8(11);
  else if (t == BELL_END) {
    if (bellActive) drawChar8x8(13); else mxClear();
  } else {
    int periodNumber = 0;
    for (int i = 0; i <= cb; i++) if (bellType[i] == BELL_PERIOD) periodNumber++;
    if (periodNumber < 10) drawChar8x8(periodNumber);
    else drawTwoDigitScroll(periodNumber / 10, periodNumber % 10);
  }
}

/* ================= NAVIGATION HELPERS ================= */
void cancelEditing() {                       // discard unsaved edits, put the saved active table back
  restoreActiveTimetable();
  lcdBlink(false);
  uiState = UI_MENU;
  screenChanged = true;
}

void leaveToDashboard() {                    // used by inactivity time-out
  if (uiState == UI_SET_BELL || uiState == UI_AUTO_BELLS) restoreActiveTimetable();
  if (uiState == UI_SELFTEST) { outputsOffIfIdle(); mxClear(); }
  lcdBlink(false);
  uiState = UI_DASHBOARD;
  screenChanged = true;
}

void checkMenuTimeout() {
  bool timed = false;
  switch (uiState) {
    case UI_MENU: case UI_SET_DATETIME: case UI_SET_BELL: case UI_MANAGE_TIMETABLE:
    case UI_AUTO_BELLS: case UI_SELFTEST: case UI_CLEAR_EEPROM:
      timed = true; break;
    case UI_EXAM_TIMER:
      timed = !examScheduled && !examRunning; break;
    default: break;
  }
  if (timed && (millis() - lastActivity > MENU_TIMEOUT_MS)) leaveToDashboard();
}

/* ================= SCREEN 1: DASHBOARD ================= */
void screenDashboard() {
  if (screenChanged) {
    lcdClear();
    lcdBlink(false);
    screenChanged = false;
  }

  int curBell, nextBell;
  long countdown;
  getBellStatus(curBell, nextBell, countdown);

  char statusTag[4] = "--";
  if (curBell >= 0) {
    uint8_t t = bellType[curBell];
    if (t == BELL_PRAYER) strcpy_P(statusTag, PSTR("PR"));
    else if (t == BELL_RECESS) strcpy_P(statusTag, PSTR("RC"));
    else if (t == BELL_END) strcpy_P(statusTag, PSTR("EN"));
    else {
      int pNum = 0;
      for (int i = 0; i <= curBell; i++) if (bellType[i] == BELL_PERIOD) pNum++;
      snprintf_P(statusTag, sizeof(statusTag), PSTR("P%d"), pNum);
    }
  }

  char buf[17];
  if (clockOk) {
    snprintf_P(buf, sizeof(buf), PSTR("%02d/%02d %02d:%02d:%02d%2s"),
               nowT.d, nowT.mo, nowT.h, nowT.m, nowT.s, statusTag);
    lcdPrintRow(0, buf);
  } else {
    lcdPrintRow_P(0, F("RTC READ ERROR"));
  }

  if (nextBell >= 0 && countdown >= 0) {
    int mm = (countdown % 3600) / 60;
    int ss = countdown % 60;
    if (countdown >= 3600) {
      int hh = countdown / 3600;
      snprintf_P(buf, sizeof(buf), PSTR("N:%02d:%02d%c -%02dh%02dm"),
                 bellHour[nextBell], bellMin[nextBell], getBellSymbolChar(nextBell), hh, mm);
    } else {
      snprintf_P(buf, sizeof(buf), PSTR("N:%02d:%02d%c -%02d:%02d"),
                 bellHour[nextBell], bellMin[nextBell], getBellSymbolChar(nextBell), mm, ss);
    }
    lcdPrintRow(1, buf);
  } else if (bellCount == 0) {
    lcdPrintRow_P(1, F("NO BELLS SET"));
  } else {
    lcdPrintRow_P(1, F("DAY COMPLETED"));
  }

  if (evSet) {
    uiState = UI_MENU;
    screenChanged = true;
  }
}

/* ================= SCREEN 2: MAIN MENU ================= */
void screenMenu() {
  static uint8_t sel = 0;
  const uint8_t ITEM_COUNT = 9;

  if (screenChanged) {
    sel = 0;
    lcdClear();
    lcdBlink(false);
    screenChanged = false;
  }

  if (evDown) sel = (sel + 1) % ITEM_COUNT;
  if (evUp)   sel = (sel + ITEM_COUNT - 1) % ITEM_COUNT;

  char buf[17];
  snprintf_P(buf, sizeof(buf), PSTR("MENU (%d/%d)"), sel + 1, ITEM_COUNT);
  lcdPrintRow(0, buf);

  switch (sel) {
    case 0: lcdPrintRow_P(1, F(">1.Set Date/Time")); break;
    case 1: lcdPrintRow_P(1, F(">2.Set Bells"));     break;
    case 2: lcdPrintRow_P(1, F(">3.Timetables"));    break;
    case 3: lcdPrintRow_P(1, F(">4.Auto Generate")); break;
    case 4: lcdPrintRow_P(1, F(">5.Exam Timer"));    break;
    case 5: lcdPrintRow_P(1, F(">6.Manual Bell"));   break;
    case 6: lcdPrintRow_P(1, F(">7.Self Test"));     break;
    case 7: lcdPrintRow_P(1, F(">8.Clear EEPROM"));  break;
    case 8: lcdPrintRow_P(1, F(">9.Exit Menu"));     break;
  }

  if (evSet) {
    screenChanged = true;
    switch (sel) {
      case 0: uiState = UI_SET_DATETIME;     break;
      case 1: uiState = UI_SET_BELL;         break;
      case 2: uiState = UI_MANAGE_TIMETABLE; break;
      case 3: uiState = UI_AUTO_BELLS;       break;
      case 4: uiState = UI_EXAM_TIMER;       break;
      case 5: uiState = UI_TRIGGER_BELL;     break;
      case 6: uiState = UI_SELFTEST;         break;
      case 7: uiState = UI_CLEAR_EEPROM;     break;
      case 8: uiState = UI_DASHBOARD;        break;
    }
  }
}

/* ================= SCREEN 3: SET DATE & TIME ================= */
void screenSetDateTime() {
  static uint8_t step, d, mo, h, mi;
  static uint16_t y;

  if (screenChanged) {
    d  = nowT.d;  if (d < 1) d = 1;
    mo = nowT.mo; if (mo < 1 || mo > 12) mo = 1;
    y  = nowT.y;  if (y < 2024 || y > 2099) y = 2026;
    h  = nowT.h;
    mi = nowT.m;
    step = 0;
    lcdClear();
    lcdBlink(true);
    screenChanged = false;
  }

  if (cancelRequested()) { lcdBlink(false); uiState = UI_MENU; screenChanged = true; return; }

  char buf[17];
  snprintf_P(buf, sizeof(buf), PSTR("DATE: %02d/%02d/%04d"), d, mo, y);
  lcdPrintRow(0, buf);
  snprintf_P(buf, sizeof(buf), PSTR("TIME: %02d:%02d  OK"), h, mi);
  lcdPrintRow(1, buf);

  const uint8_t cx[] = {6, 9, 12, 6, 9};
  const uint8_t cy[] = {0, 0, 0,  1, 1};
  lcd.setCursor(cx[step], cy[step]);

  uint8_t dim = daysInMonth(mo, y);

  if (evUp) {
    if (step == 0) d = (d >= dim) ? 1 : d + 1;
    else if (step == 1) mo = (mo % 12) + 1;
    else if (step == 2 && y < 2099) y++;
    else if (step == 3) h = (h + 1) % 24;
    else if (step == 4) mi = (mi + 1) % 60;
  }
  if (evDown) {
    if (step == 0) d = (d <= 1) ? dim : d - 1;
    else if (step == 1) mo = (mo == 1) ? 12 : mo - 1;
    else if (step == 2 && y > 2024) y--;
    else if (step == 3) h = (h + 23) % 24;
    else if (step == 4) mi = (mi + 59) % 60;
  }

  dim = daysInMonth(mo, y);                 // keep the day valid for the chosen month/year
  if (d > dim) d = dim;

  if (evSet) {
    step++;
    if (step > 4) {
      rtc.setDate(d, mo, y);
      rtc.setTime(h, mi, 0);
      if (readClock()) lastBellMinute = nowT.h * 60 + nowT.m;
      showToast_P(F("DATE & TIME"), F("SAVED!"));
      uiState = UI_MENU;
      screenChanged = true;
    }
  }
}

/* ================= SCREEN 4: MANUAL SET BELLS ================= */
void screenSetBell() {
  static uint8_t stage, idx, hh, mm, step, typeSel, newCount;
  static char ttName[TT_NAME_LEN + 1];

  if (screenChanged) {
    stage = 0; idx = 0; hh = 8; mm = 0; step = 0;
    typeSel = BELL_PERIOD;
    newCount = (bellCount > 0) ? bellCount : 1;   // do NOT touch live bellCount while choosing
    lcdClear();
    lcdBlink(false);
    screenChanged = false;
  }

  if (cancelRequested()) { cancelEditing(); return; }

  char buf[17];

  if (stage == 0) {
    lcdPrintRow_P(0, F("NO. OF BELLS:"));
    snprintf_P(buf, sizeof(buf), PSTR("> %02d  (SET=NEXT)"), newCount);
    lcdPrintRow(1, buf);

    if (evUp && newCount < MAX_BELLS) newCount++;
    if (evDown && newCount > 1) newCount--;

    if (evSet) {
      stage = 1; idx = 0; hh = 8; mm = 0; step = 0;
      lcdClear();
      lcdBlink(true);
    }
    return;
  }

  if (stage == 2) {
    snprintf_P(buf, sizeof(buf), PSTR("SAVE %s?"), ttName);
    lcdPrintRow(0, buf);
    lcdPrintRow_P(1, F("SET=YES   UP=NO"));

    if (evSet) {
      uint8_t r = saveCurrentTimetable(ttName);
      showSaveResult(r);
      if (r != 0) restoreActiveTimetable();
      uiState = UI_MENU;
      screenChanged = true;
    } else if (evUp) {
      restoreActiveTimetable();
      uiState = UI_MENU;
      screenChanged = true;
    }
    return;
  }

  snprintf_P(buf, sizeof(buf), PSTR("BELL %02d OF %02d"), idx + 1, newCount);
  lcdPrintRow(0, buf);
  snprintf_P(buf, sizeof(buf), PSTR("%02d:%02d  %s"), hh, mm, bellTypeName(typeSel));
  lcdPrintRow(1, buf);

  lcdBlink(true);
  if (step == 0) lcd.setCursor(0, 1);
  else if (step == 1) lcd.setCursor(3, 1);
  else lcd.setCursor(7, 1);

  if (evUp) {
    if (step == 0) hh = (hh + 1) % 24;
    else if (step == 1) mm = (mm + 1) % 60;
    else if (step == 2 && typeSel < BELL_END) typeSel++;
  }
  if (evDown) {
    if (step == 0) hh = (hh + 23) % 24;
    else if (step == 1) mm = (mm + 59) % 60;
    else if (step == 2 && typeSel > 0) typeSel--;
  }

  if (evSet) {
    if (step < 2) { step++; return; }

    if (idx > 0) {
      int prev = bellHour[idx - 1] * 60 + bellMin[idx - 1];
      int curr = hh * 60 + mm;
      if (curr <= prev) {
        showToast_P(F("INVALID TIME!"), F("MUST INCREASE"));
        step = 0;
        return;
      }
    }

    bellHour[idx] = hh;
    bellMin[idx]  = mm;
    bellType[idx] = typeSel;
    idx++;
    step = 0;

    if (idx == newCount) {
      bellCount = newCount;                     // commit only when the whole table is entered
      lcdBlink(false);
      makeAutoTimetableName(ttName);
      stage = 2;
      lcdClear();
    }
  }
}

/* ================= SCREEN 5: MANAGE TIMETABLES ================= */
#define TL_LIST    0
#define TL_INFO    1
#define TL_ACTION  2
#define TL_CONFIRM 3

static uint8_t  tlLayer, tlInfoIdx, tlBTotal, tlDef, tlBh, tlBm, tlBt;
static uint16_t tlSel, tlTotal;
static char     tlName[TT_NAME_LEN + 1];
static bool     tlDirty;

static void tlRefresh() {                   // EEPROM is read only when selection/state changed
  tlTotal = getTimetableCount();
  tlName[0] = 0; tlBTotal = 0;
  if (tlTotal > 0) {
    if (tlSel >= tlTotal) tlSel = tlTotal - 1;
    readTimetableName(tlSel, tlName);
    tlBTotal = getTimetableBellCount(tlSel);
    tlDef = getDefaultTimetable();
    if (tlInfoIdx >= tlBTotal) tlInfoIdx = (tlBTotal > 0) ? tlBTotal - 1 : 0;
    if (tlLayer == TL_INFO && tlBTotal > 0) {
      tlBh = 0; tlBm = 0; tlBt = 0;
      readTimetableBell(tlSel, tlInfoIdx, tlBh, tlBm, tlBt);
    }
  }
  tlDirty = false;
}

void screenTimetableList() {
  char buf[17];

  if (screenChanged) {
    tlLayer = TL_LIST; tlSel = 0; tlInfoIdx = 0;
    lcdClear(); lcdBlink(false);
    tlRefresh();
    screenChanged = false;
  }

  if (tlTotal == 0) {
    lcdPrintRow_P(0, F("NO TIMETABLES"));
    lcdPrintRow_P(1, F("SET = BACK"));
    if (evSet) { uiState = UI_MENU; screenChanged = true; }
    return;
  }

  if (tlLayer == TL_LIST) {
    if (evDown && tlSel + 1 < tlTotal) { tlSel++; tlDirty = true; }
    if (evUp) {
      if (tlSel > 0) { tlSel--; tlDirty = true; }
      else { uiState = UI_MENU; screenChanged = true; return; }
    }
    if (evSet) { tlLayer = TL_INFO; tlInfoIdx = 0; tlDirty = true; }
  } else if (tlLayer == TL_INFO) {
    if (evDown && tlInfoIdx + 1 < tlBTotal) { tlInfoIdx++; tlDirty = true; }
    if (evUp) {
      if (tlInfoIdx > 0) { tlInfoIdx--; tlDirty = true; }
      else { tlLayer = TL_LIST; tlDirty = true; }
    }
    if (evSet) tlLayer = TL_ACTION;
  } else if (tlLayer == TL_ACTION) {
    if (evUp) {                                   // make active + default
      setDefaultTimetable((uint8_t)tlSel);
      loadTimetable(tlSel);
      lcdPrintRow_P(0, F("ACTIVE TABLE:"));
      lcdPrintRow(1, tlName);
      delay(1200);
      tlLayer = TL_INFO; tlDirty = true;
    } else if (evDown) {
      tlLayer = TL_CONFIRM;
    } else if (evSet) {
      tlLayer = TL_INFO; tlDirty = true;
    }
  } else {                                        // TL_CONFIRM
    if (evSet) {
      lcdPrintRow_P(0, F("DELETING..."));
      lcdPrintRow_P(1, F("PLEASE WAIT"));
      bool ok = deleteTimetable(tlSel);
      if (ok) showToast_P(F("TIMETABLE"), F("DELETED"));
      else    showToast_P(F("EEPROM ERROR"), F("NOT DELETED"));
      tlLayer = TL_LIST; tlDirty = true;
    } else if (evUp) {
      tlLayer = TL_ACTION;
    }
  }

  if (tlDirty) tlRefresh();
  if (tlTotal == 0) return;

  if (tlLayer == TL_LIST) {
    snprintf_P(buf, sizeof(buf), PSTR("TABLES (%d/%d)"), (int)tlSel + 1, (int)tlTotal);
    lcdPrintRow(0, buf);
    snprintf_P(buf, sizeof(buf), PSTR(">%-10s %s"), tlName, (tlSel == tlDef) ? "[*]" : "   ");
    lcdPrintRow(1, buf);
  } else if (tlLayer == TL_INFO) {
    snprintf_P(buf, sizeof(buf), PSTR("%s %d/%d"), tlName, tlInfoIdx + 1, tlBTotal);
    lcdPrintRow(0, buf);
    if (tlBTotal > 0) {
      snprintf_P(buf, sizeof(buf), PSTR("%02d:%02d %s"), tlBh, tlBm, bellTypeName(tlBt));
      lcdPrintRow(1, buf);
    } else {
      lcdPrintRow_P(1, F("EMPTY"));
    }
  } else if (tlLayer == TL_ACTION) {
    lcdPrintRow_P(0, F("UP=SET AS ACTIVE"));
    lcdPrintRow_P(1, F("DN=DELETE SET=BK"));
  } else {
    snprintf_P(buf, sizeof(buf), PSTR("DEL %s ?"), tlName);
    lcdPrintRow(0, buf);
    lcdPrintRow_P(1, F("SET=YES   UP=NO"));
  }
}

/* ================= SCREEN 6: AUTO BELL GENERATOR ================= */
bool generateAutoTimetable(uint8_t startH, uint8_t startM, uint8_t prayerMin,
                           uint8_t periodMin, uint8_t totalPeriods,
                           uint8_t recessMin, uint8_t recessAfter) {
  bellCount = 0;
  int cur = startH * 60 + startM;

  if (prayerMin > 0) {
    if (bellCount >= MAX_BELLS || cur >= 1440) return false;
    bellHour[bellCount] = cur / 60;
    bellMin[bellCount]  = cur % 60;
    bellType[bellCount] = BELL_PRAYER;
    bellCount++;
    cur += prayerMin;
  }

  for (uint8_t p = 1; p <= totalPeriods; p++) {
    if (bellCount >= MAX_BELLS || cur >= 1440) return false;     // would wrap past midnight -> unsorted table
    bellHour[bellCount] = cur / 60;
    bellMin[bellCount]  = cur % 60;
    bellType[bellCount] = BELL_PERIOD;
    bellCount++;
    cur += periodMin;

    if (recessAfter == p && recessMin > 0) {
      if (bellCount >= MAX_BELLS || cur >= 1440) return false;
      bellHour[bellCount] = cur / 60;
      bellMin[bellCount]  = cur % 60;
      bellType[bellCount] = BELL_RECESS;
      bellCount++;
      cur += recessMin;
    }
  }

  if (bellCount >= MAX_BELLS || cur >= 1440) return false;
  bellHour[bellCount] = cur / 60;
  bellMin[bellCount]  = cur % 60;
  bellType[bellCount] = BELL_END;
  bellCount++;
  return true;
}

void screenAutoGenerateBells() {
  static uint8_t step = 0, mode = 0, viewIdx = 0;
  static uint8_t startH, startM, prayerMin, periodMin, totalPeriods, recessMin, recessAfter;

  if (screenChanged) {
    startH = 8; startM = 0;
    prayerMin = 15; periodMin = 40; totalPeriods = 8;
    recessMin = 25; recessAfter = 4;
    step = 0; mode = 0; viewIdx = 0;
    lcdClear(); lcdBlink(true);
    screenChanged = false;
  }

  if (cancelRequested()) { cancelEditing(); return; }

  char buf[17];

  if (mode == 1) {
    lcdBlink(false);
    snprintf_P(buf, sizeof(buf), PSTR("PREVIEW (%d/%d)"), viewIdx + 1, bellCount);
    lcdPrintRow(0, buf);
    snprintf_P(buf, sizeof(buf), PSTR("B%02d %02d:%02d [%c]"), viewIdx + 1,
               bellHour[viewIdx], bellMin[viewIdx], getBellSymbolChar(viewIdx));
    lcdPrintRow(1, buf);

    if (evDown && viewIdx + 1 < bellCount) viewIdx++;
    if (evUp && viewIdx > 0) viewIdx--;
    if (evSet) mode = 2;
    return;
  }

  if (mode == 2) {
    lcdPrintRow_P(0, F("SAVE AUTO TABLE?"));
    lcdPrintRow_P(1, F("SET=YES   UP=NO"));
    if (evSet) {
      char name[TT_NAME_LEN + 1];
      makeAutoTimetableName(name);
      uint8_t r = saveCurrentTimetable(name);
      showSaveResult(r);
      if (r != 0) restoreActiveTimetable();
      uiState = UI_MENU;
      screenChanged = true;
    } else if (evUp) {
      mode = 1;
    }
    return;
  }

  lcdPrintRow_P(0, F("AUTO GENERATOR"));
  switch (step) {
    case 0: snprintf_P(buf, sizeof(buf), PSTR("START HR : %02d"), startH);       break;
    case 1: snprintf_P(buf, sizeof(buf), PSTR("START MIN: %02d"), startM);       break;
    case 2: snprintf_P(buf, sizeof(buf), PSTR("PRAYER   : %02dm"), prayerMin);   break;
    case 3: snprintf_P(buf, sizeof(buf), PSTR("PERIOD   : %02dm"), periodMin);   break;
    case 4: snprintf_P(buf, sizeof(buf), PSTR("PERIODS  : %02d"), totalPeriods); break;
    case 5: snprintf_P(buf, sizeof(buf), PSTR("RECESS   : %02dm"), recessMin);   break;
    default: snprintf_P(buf, sizeof(buf), PSTR("REC AFTER: P%02d"), recessAfter); break;
  }
  lcdPrintRow(1, buf);
  lcdBlink(true);
  lcd.setCursor(11, 1);

  if (evUp) {
    if (step == 0) startH = (startH + 1) % 24;
    else if (step == 1) startM = (startM + 1) % 60;
    else if (step == 2 && prayerMin < 60) prayerMin++;
    else if (step == 3 && periodMin < 90) periodMin++;
    else if (step == 4 && totalPeriods < 16) totalPeriods++;
    else if (step == 5 && recessMin < 60) recessMin++;
    else if (step == 6 && recessAfter < totalPeriods) recessAfter++;
  }
  if (evDown) {
    if (step == 0) startH = (startH + 23) % 24;
    else if (step == 1) startM = (startM + 59) % 60;
    else if (step == 2 && prayerMin > 0) prayerMin--;
    else if (step == 3 && periodMin > 5) periodMin--;
    else if (step == 4 && totalPeriods > 1) totalPeriods--;
    else if (step == 5 && recessMin > 0) recessMin--;
    else if (step == 6 && recessAfter > 0) recessAfter--;
  }
  if (recessAfter > totalPeriods) recessAfter = totalPeriods;

  if (evSet) {
    if (step < 6) {
      step++;
    } else {
      if (generateAutoTimetable(startH, startM, prayerMin, periodMin, totalPeriods, recessMin, recessAfter)) {
        viewIdx = 0;
        mode = 1;
      } else {
        showToast_P(F("ERROR:"), F("TOO MANY/LATE"));
        restoreActiveTimetable();
        screenChanged = true;
      }
    }
  }
}

/* ================= SCREEN 7: EXAM TIMER ================= */
void updateExamMatrixPattern(uint32_t left, uint32_t total) {
  uint8_t height = (uint8_t)((left * 8UL + total - 1UL) / total);   // rounds up
  if (height > 8) height = 8;
  for (uint8_t r = 0; r < 8; r++) mxRow(r, (r >= 8 - height) ? 0xFF : 0x00);
}

void screenExamTimer() {
  static uint8_t step = 0;

  if (screenChanged) {
    if (!examScheduled && !examRunning) {
      examStartH = nowT.h;
      examStartM = nowT.m;
      examDurH = 3;
      examDurM = 0;
      step = 0;
    }
    lcdClear();
    screenChanged = false;
  }

  char buf[17];

  if (examScheduled && !examRunning) {
    lcdBlink(false);
    uint32_t now = secOfDay();
    uint32_t startSec = (uint32_t)examStartH * 3600UL + (uint32_t)examStartM * 60UL;

    snprintf_P(buf, sizeof(buf), PSTR("WAIT %02d:%02d:%02d"), nowT.h, nowT.m, nowT.s);
    lcdPrintRow(0, buf);
    snprintf_P(buf, sizeof(buf), PSTR("START @ %02d:%02d"), examStartH, examStartM);
    lcdPrintRow(1, buf);

    if (((now + 86400UL - startSec) % 86400UL) < 60UL) {      // robust window instead of "== exact second"
      examScheduled = false;
      examRunning = true;
      examStartSec = now;
      startBell(2500, 1200);
    }

    if (bothHeld(BTN_UP, BTN_SET, 600)) {
      examScheduled = false;
      uiState = UI_MENU;
      screenChanged = true;
    }
    return;
  }

  if (examRunning) {
    lcdBlink(false);
    uint32_t now = secOfDay();
    uint32_t elapsed = (now + 86400UL - examStartSec) % 86400UL;   // survives midnight

    if (elapsed >= examDurationSec) {
      examRunning = false;
      startBell(3500, 1000);
      mxClear();
      showToast_P(F("EXAM FINISHED"), F("TIME UP!"));
      uiState = UI_DASHBOARD;
      screenChanged = true;
      return;
    }

    uint32_t left = examDurationSec - elapsed;
    updateExamMatrixPattern(left, examDurationSec);

    lcdPrintRow_P(0, F("EXAM IN PROGRESS"));
    snprintf_P(buf, sizeof(buf), PSTR("LEFT: %02lu:%02lu:%02lu"), left / 3600UL, (left % 3600UL) / 60UL, left % 60UL);
    lcdPrintRow(1, buf);

    if (bothHeld(BTN_UP, BTN_SET, 600)) {
      examRunning = false;
      mxClear();
      uiState = UI_MENU;
      screenChanged = true;
    }
    return;
  }

  if (cancelRequested()) { lcdBlink(false); uiState = UI_MENU; screenChanged = true; return; }

  snprintf_P(buf, sizeof(buf), PSTR("START : %02d:%02d"), examStartH, examStartM);
  lcdPrintRow(0, buf);
  snprintf_P(buf, sizeof(buf), PSTR("DURATN: %02d:%02d"), examDurH, examDurM);
  lcdPrintRow(1, buf);

  lcdBlink(true);
  if (step == 0) lcd.setCursor(8, 0);
  else if (step == 1) lcd.setCursor(11, 0);
  else if (step == 2) lcd.setCursor(8, 1);
  else lcd.setCursor(11, 1);

  if (evUp) {
    if (step == 0) examStartH = (examStartH + 1) % 24;
    else if (step == 1) examStartM = (examStartM + 1) % 60;
    else if (step == 2) examDurH = (examDurH + 1) % 12;
    else examDurM = (examDurM + 1) % 60;
  }
  if (evDown) {
    if (step == 0) examStartH = (examStartH + 23) % 24;
    else if (step == 1) examStartM = (examStartM + 59) % 60;
    else if (step == 2) examDurH = (examDurH + 11) % 12;
    else examDurM = (examDurM + 59) % 60;
  }

  if (evSet) {
    if (step < 3) {
      step++;
    } else if (examDurH == 0 && examDurM == 0) {
      showToast_P(F("DURATION"), F("CANNOT BE 0"));
      step = 2;
    } else {
      examDurationSec = (uint32_t)examDurH * 3600UL + (uint32_t)examDurM * 60UL;
      examScheduled = true;
      step = 0;
      lcdBlink(false);
    }
  }
}

/* ================= SCREEN 8: MANUAL EMERGENCY BELL ================= */
void screenTriggerBell() {
  static unsigned long lastToggle = 0;
  static bool toneHigh = false;

  if (screenChanged) {
    manualRinging = false;
    lcdClear();
    lcdBlink(false);
    screenChanged = false;
  }

  if (!manualRinging) {
    lcdPrintRow_P(0, F("MANUAL BELL"));
    lcdPrintRow_P(1, F("SET=RING UP=EXIT"));

    if (evUp) {
      uiState = UI_MENU;
      screenChanged = true;
    } else if (evSet) {
      manualRinging = true;
      digitalWrite(RELAY_PIN, LOW);
      digitalWrite(LED_PIN, HIGH);
    }
  } else {
    lcdPrintRow_P(0, F("!! BELL ACTIVE!!"));
    lcdPrintRow_P(1, F("PRESS UP TO STOP"));

    if (millis() - lastToggle > 300) {
      lastToggle = millis();
      toneHigh = !toneHigh;
      if (toneHigh) { tone(BUZZER_PIN, 1300); drawChar8x8(13); }
      else          { tone(BUZZER_PIN, 900);  mxClear(); }
    }

    if (evUp || evSet) {
      manualRinging = false;
      outputsOffIfIdle();
      mxClear();
      uiState = UI_MENU;
      screenChanged = true;
    }
  }
}

/* ================= SCREEN 9: SELF TEST ================= */
void screenSelfTest() {
  static uint8_t step = 0, subStep = 1;
  static unsigned long lastUpdate = 0;
  char buf[17];

  if (screenChanged) {
    step = 0; subStep = 1;
    lastUpdate = millis() - 1500;
    lcdClear();
    lcdBlink(false);
    mxClear();
    outputsOffIfIdle();
    screenChanged = false;
  }

  if (cancelRequested()) {                     // always a way out (stuck button / bad wiring)
    outputsOffIfIdle(); mxClear();
    uiState = UI_MENU; screenChanged = true;
    return;
  }

  if (step == 8) {
    lcdPrintRow_P(0, F("SELF TEST DONE!"));
    lcdPrintRow_P(1, F("SET = EXIT"));
    if (evSet) { uiState = UI_MENU; screenChanged = true; }
    return;
  }

  if (millis() - lastUpdate < 1500) return;

  if (step == 6) {                             // interactive button test (non-blocking)
    mxClear();
    lcdPrintRow_P(0, F("5.BUTTON TEST"));
    if (subStep == 1) {
      lcdPrintRow_P(1, F("PRESS SET (D2)"));
      if (evSet) subStep++;
    } else if (subStep == 2) {
      lcdPrintRow_P(1, F("PRESS UP (D3)"));
      if (evUp) subStep++;
    } else {
      lcdPrintRow_P(1, F("PRESS DOWN (D4)"));
      if (evDown) step = 8;
    }
    return;
  }

  lastUpdate = millis();
  switch (step) {
    case 0:
      lcdPrintRow_P(0, F("1.I2C DEVICES"));
      snprintf_P(buf, sizeof(buf), PSTR("EE:%s RTC:%s"), eepromPresent() ? "OK" : "ERR", rtcPresent() ? "OK" : "ERR");
      lcdPrintRow(1, buf);
      break;
    case 1:
      lcdPrintRow_P(0, F("2.RELAY & LED"));
      lcdPrintRow_P(1, F("TURNING ON..."));
      if (!bellActive) { digitalWrite(RELAY_PIN, LOW); digitalWrite(LED_PIN, HIGH); }
      break;
    case 2:
      outputsOffIfIdle();
      lcdPrintRow_P(1, F("RELAY/LED: OFF"));
      break;
    case 3:
      lcdPrintRow_P(0, F("3.BUZZER TEST"));
      lcdPrintRow_P(1, F("TONE 1000Hz..."));
      tone(BUZZER_PIN, 1000);
      break;
    case 4:
      outputsOffIfIdle();
      lcdPrintRow_P(1, F("BUZZER: OFF"));
      break;
    case 5:
      lcdPrintRow_P(0, F("4.MATRIX 8x8"));
      lcdPrintRow_P(1, F("ALL LEDS ON"));
      for (uint8_t r = 0; r < 8; r++) mxRow(r, 0xFF);
      break;
  }
  step++;
  if (step == 6) lastUpdate = millis();
}

/* ================= SCREEN 10: CLEAR EEPROM ================= */
void screenClearEEPROM() {
  if (screenChanged) {
    lcdClear();
    lcdBlink(false);
    screenChanged = false;
  }

  lcdPrintRow_P(0, F("WIPE ALL DATA?"));
  lcdPrintRow_P(1, F("SET=YES   UP=NO"));

  if (evUp) {
    uiState = UI_MENU;
    screenChanged = true;
  } else if (evSet) {
    lcdPrintRow_P(0, F("WIPING AT24C32.."));
    lcdPrintRow_P(1, F("PLEASE WAIT"));
    bool ok = eepromFormat();
    bellCount = 0;
    mxClear();
    if (ok) showToast_P(F("SYSTEM RESET"), F("EEPROM CLEARED"));
    else    showToast_P(F("EEPROM ERROR"), F("WIPE FAILED"));
    uiState = UI_DASHBOARD;
    screenChanged = true;
  }
}

/* ================= SETUP & MAIN LOOP ================= */
void setup() {
  // Drive outputs to their idle level BEFORE switching to OUTPUT -> no relay/buzzer blip on reset
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(BTN_SET, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  Wire.begin();
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(3000, true);             // a hung I2C device can no longer freeze the loop forever
#endif

  matrix.shutdown(0, false);
  matrix.setIntensity(0, 6);
  matrix.clearDisplay(0);
  memset(mxShadow, 0, sizeof(mxShadow));

  lcd.init();
  lcd.backlight();
  lcdClear();

  lcdPrintRow_P(0, F("Mission Raftaar"));
  lcdPrintRow_P(1, F("TechFair 2026-27"));          // was 17 chars -> last char was cut off
  delay(2000);

  lcdPrintRow_P(0, F("Automatic"));
  lcdPrintRow_P(1, F("School Bell"));
  delay(1500);

  while (!rtc.begin()) {
    lcdPrintRow_P(0, F("DS3231 RTC"));
    lcdPrintRow_P(1, F("NOT FOUND!"));
    delay(500);
  }
  rtc.setHourMode(CLOCK_H24);

  if (!eepromPresent()) {
    showToast_P(F("AT24C32 EEPROM"), F("NOT FOUND!"));
  } else if (!eepromFormatted()) {
    eepromFormat();
    showToast_P(F("EEPROM FORMAT"), F("FIRST BOOT"));
  }

  restoreActiveTimetable();

  if (readClock()) { clockOk = true; lastBellMinute = nowT.h * 60 + nowT.m; }   // no bell on power-up
  lastActivity = millis();

  uiState = UI_DASHBOARD;
  screenChanged = true;
}

void loop() {
  static unsigned long lastLcdRefresh = 0;

  scanButtons();

  clockOk = readClock();                        // exactly ONE consistent I2C read per loop
  if (clockOk) checkScheduledBells();

  int cb, nb;
  long cd;
  getBellStatus(cb, nb, cd);
  updateMatrix(cb, nb, cd);
  handleBell();
  checkMenuTimeout();

  if (millis() - lastLcdRefresh > 3000UL) {     // heal LCD garbage caused by relay EMI
    lastLcdRefresh = millis();
    memset(lcdShadow, 0, sizeof(lcdShadow));
  }

  switch (uiState) {
    case UI_DASHBOARD:        screenDashboard();         break;
    case UI_MENU:             screenMenu();              break;
    case UI_SET_DATETIME:     screenSetDateTime();       break;
    case UI_SET_BELL:         screenSetBell();           break;
    case UI_MANAGE_TIMETABLE: screenTimetableList();     break;
    case UI_AUTO_BELLS:       screenAutoGenerateBells(); break;
    case UI_EXAM_TIMER:       screenExamTimer();         break;
    case UI_TRIGGER_BELL:     screenTriggerBell();       break;
    case UI_SELFTEST:         screenSelfTest();          break;
    case UI_CLEAR_EEPROM:     screenClearEEPROM();       break;
  }

  delay(10);
}
