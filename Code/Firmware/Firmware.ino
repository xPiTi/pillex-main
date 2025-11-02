/*
 * Pill Dispenser Firmware
 * ------------------------------------------------------------
 * Responsibilities:
 *  - I2C expander (TCA9555) control for 4 pill modules
 *  - EEPROM read/write
 *  - Pill drop state machine
 *  - OLED display (Adafruit_SSD1305)
 *  - Screen state machine / rendering
 *  - Button input handling (debounce + long-press)
 *  - Serial command parser (non-blocking)
 */

#include <avr/pgmspace.h>
#include <SPI.h>
#include <Wire.h>
#include "TCA9555.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1305.h>

// ===================== Display Definitions =====================
#define OLED_CS    10
#define OLED_DC     4
#define OLED_RESET  9
Adafruit_SSD1305 display(128, 32, &SPI, OLED_DC, OLED_RESET, OLED_CS, 7000000UL);

// ===================== Button Definitions =====================
#define BTN_UP    8
#define BTN_DOWN  6
#define BTN_BACK  5
#define BTN_OK    7

// Button list / metadata
const byte  buttonCount = 4;
const byte  buttonPins[]  = { BTN_UP, BTN_DOWN, BTN_BACK, BTN_OK };
const char* buttonNames[] = { "UP",   "DOWN",  "BACK",  "OK" };

// Debounce + long-press timing
const unsigned long debounceDelay  = 50;     // ms
const unsigned long longPressTime  = 1000;   // ms (1 second)

// Button state tracking
bool           buttonState[buttonCount];      // Current stable state (active-low)
bool           lastReading[buttonCount];      // Last raw reading
unsigned long  lastDebounceTime[buttonCount];
unsigned long  pressStartTime[buttonCount];
bool           longPressReported[buttonCount];

// ===================== Cartridge / Modules =====================
#define CART_IO_INT 2
#define CART_SNS    A0

// TCA9555 I/O Expander @ 0x20
TCA9555 TCA(0x20);

// Per-module pin mapping on TCA9555 (0..15)
typedef struct {
  uint8_t motA;  // Motor A
  uint8_t motB;  // Motor B
  uint8_t vibr;  // Vibrator
  uint8_t sns;   // Sensor (active-low: LOW=pill present)
} ModulePins;

//   | MOD | MOT_A | MOT_B | VIBR | SNS |
//   |-----|-------|-------|------|-----|
//   | M1  | 0     | 1     | 2    | 3   |
//   | M2  | 4     | 5     | 6    | 7   |
//   | M3  | 8     | 9     | 10   | 11  |
//   | M4  | 12    | 13    | 14   | 15  |
ModulePins modules[4] = {
  { 0,  1,  2,  3 },  // MOD1
  { 4,  5,  6,  7 },  // MOD2
  { 8,  9, 10, 11 },  // MOD3
  {12, 13, 14, 15 }   // MOD4
};

// ===================== Pill Drop State Machine =====================
enum PillDropState {
  IDLE,
  CLOSE_SLIDER,
  CHECK_SENSOR,
  VIBRATE_RETRY,
  OPEN_SLIDER,
  FINISH
};

PillDropState dropState   = IDLE;   // current state
bool           busy        = false; // true while a drop is in progress
unsigned long  stateStart  = 0;     // millis() when current state began
int            retryCount  = 0;     // VIBRATE_RETRY attempts
uint8_t        currentModule = 0;   // [0..3]
bool           sns_init    = false; // was sensor initially active (pill present)?
int            pillsDropped[4] = {0, 0, 0, 0};
int            pillsToDrop[4]  = {0, 0, 0, 0};
unsigned long  pillDispDelayTime;   // (kept for parity; not used in current logic)

// ===================== EEPROM =====================
#define EEPROM_I2C_ADDRESS 0x50

// ===================== Time / Screen State =====================
unsigned long timeToUpdateScreen;   // throttles display refresh

uint8_t time_HH = 0;
uint8_t time_MM = 0;
uint8_t time_SS = 0;
unsigned long timeTimer;            // next tick at 1 Hz

enum ScreenState {
  LOADING,
  TIME,
  MESSAGE,
  ERROR,
  TAKE_PILL,
  SUCCESS,
  PILL_STUCK
};

ScreenState   screenState = LOADING;
unsigned long screenTimeout;        // for SUCCESS auto-return
uint8_t       error_code = 0x0F;    // default error code

#define MAX_MESSAGE_LEN 64
char globalMessage[MAX_MESSAGE_LEN];

// ===================== Serial Communication =====================
const byte SERIAL_BUFFER_SIZE = 64;
char serialBuffer[SERIAL_BUFFER_SIZE];
byte serialBufferIndex = 0;

unsigned long heartbeatTime;          // periodic host heartbeat echo time
unsigned long heartbeatRaspberryTime; // last time host pinged (via `time` cmd)

// ================================================================
//                         FORWARD DECLARATIONS
// ================================================================
void updateScreen(ScreenState newState);
void readSerialNonBlocking();
void processCommand(char* commandLine);

void handleSensorRead(int argc, char* argv[]);
void handlePillDrop(int argc, char* argv[]);
void handlePillDropAll(int argc, char* argv[]);
void startPillDrop(uint8_t module);
void setState(PillDropState newState);
void updatePillDrop();

void handleSetTime(int argc, char* argv[]);
void handleEEPROM(int argc, char* argv[]);
void handleChangeScreenWindow(int argc, char* argv[]);
void handleDisplayMsg(int argc, char* argv[]);
void handleDisplayErr(int argc, char* argv[]);
void handleI2CScan(int argc, char* argv[]);
void handleButtonActions();

void writeEEPROM(unsigned int addr, byte data);
byte readEEPROM(unsigned int addr);

void screen_show_loading();
void screen_show_time();
void screen_show_message();
void screen_show_pill_stuck();
void screen_show_error();
void screen_show_take_pill();
void screen_show_success();

// ================================================================
//                              SETUP
// ================================================================
void setup(){
  // --- Buttons ---
  for (byte i = 0; i < buttonCount; i++) {
    pinMode(buttonPins[i], INPUT);        // external pull-ups assumed
    buttonState[i]       = HIGH;          // active-low: HIGH = not pressed
    lastReading[i]       = HIGH;
    lastDebounceTime[i]  = 0;
    pressStartTime[i]    = 0;
    longPressReported[i] = false;
  }

  // --- Core buses & serial ---
  Wire.begin();
  Serial.begin(115200);
  while (! Serial) delay(100);
  Serial.println(F("$init"));

  // --- Display ---
  Serial.println(F("Starting Display..."));
  if ( ! display.begin(0x3C) ) {
     Serial.println(F("Unable to initialize OLED"));
     while (1) yield();
  }

  // --- I/O Expander ---
  Serial.println(F("Starting IO Expander..."));
  TCA.begin();
  for (int i = 0; i < 4; i++) {
    TCA.pinMode1(modules[i].motA, OUTPUT);
    TCA.pinMode1(modules[i].motB, OUTPUT);
    TCA.pinMode1(modules[i].vibr, OUTPUT);
    TCA.pinMode1(modules[i].sns,  INPUT);
  }
  TCA.setPolarity16(0x0000);  // no input inversion
  TCA.write16(0x0000);        // all outputs LOW
  Serial.println(F("TCA9555 Ready."));

  // --- Hello splash ---
  display.clearDisplay();
  display.setTextSize(3);
  display.setTextWrap(false);
  display.setTextColor(WHITE);
  display.setCursor(13,0);
  display.println(F("Hello!"));
  display.display();
  delay(1000);

  heartbeatRaspberryTime = millis();
  heartbeatTime          = millis();
}

// ================================================================
//                               LOOP
// ================================================================
void loop() {
  unsigned long currentTime = millis();

  // Non-blocking I/O and state machines
  readSerialNonBlocking();
  handleButtonActions();
  updatePillDrop();

  // --- 1 Hz internal clock ---
  if (timeTimer < currentTime){
    timeTimer = currentTime + 1000;
    time_SS++;
    if (time_SS >= 60){ time_SS = 0; time_MM++; }
    if (time_MM >= 60){ time_MM = 0; time_HH++; }
    if (time_HH >= 24){ time_HH = 0; }
  }

  // --- Heartbeat: print time & check host liveness ---
  if (heartbeatTime <= currentTime){
    heartbeatTime += 10000; // every 10s
    Serial.print(F("$time "));
    Serial.println(currentTime);

    // If host hasn't pinged in 60s, show error 0x0F
    if (heartbeatRaspberryTime + 60000 < millis()){
      updateScreen(ERROR);
      error_code = 0x0F;
    }
  }

  // --- Auto-start pill drop if queued ---
  if (dropState == IDLE && screenState != ERROR && screenState != PILL_STUCK){
    for (int i = 0; i < 4; i++){
      if (pillsToDrop[i] > 0){
        startPillDrop(i);
        break;
      }
    }
  }

  // --- Screen refresh (low FPS by default) ---
  if (timeToUpdateScreen < currentTime){
    timeToUpdateScreen = currentTime + 250;

    switch (screenState) {
      case LOADING:    screen_show_loading(); timeToUpdateScreen = currentTime + 16; break; // higher FPS
      case TIME:       screen_show_time();        break;
      case MESSAGE:    screen_show_message();     break;
      case TAKE_PILL:  screen_show_take_pill();   break;
      case SUCCESS:    screen_show_success();     break;
      case PILL_STUCK: screen_show_pill_stuck();  break;
      case ERROR:      screen_show_error();       break;
    }

    // Auto return to TIME after SUCCESS timeout
    if (screenState == SUCCESS && screenTimeout < currentTime){
      updateScreen(TIME);
    }
  }
}

// ================================================================
//                          SCREEN CONTROL
// ================================================================
void updateScreen(ScreenState newState){
  screenState = newState;

  Serial.print(F("$screen "));
  switch (screenState) {
    case LOADING:    Serial.println(F("LOADING"));    break;
    case TIME:       Serial.println(F("TIME"));       break;
    case MESSAGE:    Serial.println(F("MESSAGE"));    break;
    case ERROR:      Serial.println(F("ERROR"));      break;
    case TAKE_PILL:  Serial.println(F("TAKE_PILL"));  break;
    case SUCCESS:    Serial.println(F("SUCCESS"));    break;
    case PILL_STUCK: Serial.println(F("PILL_STUCK")); break;
  }
}

// ================================================================
//                        SERIAL COMMANDS
// ================================================================
// Non-blocking line reader; dispatches a command when CR/LF seen
void readSerialNonBlocking() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {               // end of command
      if (serialBufferIndex > 0) {
        serialBuffer[serialBufferIndex] = '\0';  // terminate
        processCommand(serialBuffer);
        serialBufferIndex = 0;                    // reset
      }
    } else if (serialBufferIndex < SERIAL_BUFFER_SIZE - 1) {
      serialBuffer[serialBufferIndex++] = c;      // append
    }
  }
}

// Tokenize and route to the appropriate handler
void processCommand(char* commandLine) {
  char* argv[24]; // max 24 tokens
  int argc = 0;

  // Split by spaces
  char* token = strtok(commandLine, " ");
  while (token != NULL && argc < 24) {
    argv[argc++] = token;
    token = strtok(NULL, " ");
  }
  if (argc == 0) return; // empty line

  // Dispatch (compare from PROGMEM to save RAM)
  if      (strcmp_P(argv[0], PSTR("time"))   == 0) { handleSetTime(argc, argv); } // also heartbeat
  else if (strcmp_P(argv[0], PSTR("mem"))    == 0) { handleEEPROM(argc, argv); }
  else if (strcmp_P(argv[0], PSTR("msg"))    == 0) { handleDisplayMsg(argc, argv); }
  else if (strcmp_P(argv[0], PSTR("err"))    == 0) { handleDisplayErr(argc, argv); }
  else if (strcmp_P(argv[0], PSTR("screen")) == 0) { handleChangeScreenWindow(argc, argv); }
  else if (strcmp_P(argv[0], PSTR("i2c"))    == 0) { handleI2CScan(argc, argv); }
  else if (strcmp_P(argv[0], PSTR("drp"))    == 0) { handlePillDrop(argc, argv); }
  else if (strcmp_P(argv[0], PSTR("drpall")) == 0) { handlePillDropAll(argc, argv); }
  else if (strcmp_P(argv[0], PSTR("mod"))    == 0) { /* reserved: get module info */ }
  else if (strcmp_P(argv[0], PSTR("sns"))    == 0) { handleSensorRead(argc, argv); }
  else { Serial.println(F("Unknown command")); }
}

// ================================================================
//                       COMMAND HANDLERS
// ================================================================
void handleSensorRead(int argc, char* argv[]){
  (void)argc; (void)argv; // unused
  Serial.print(F("$States: "));
  for (int i = 0; i < 4; i++) {
    bool s = TCA.read1(modules[i].sns);
    Serial.print(s==LOW ? F("FULL ") : F("EMPTY "));
  }
  Serial.println();
}

void handlePillDrop(int argc, char* argv[]){
  if (argc < 1) {
    Serial.println(F("Usage: drp <MODULE>"));
    return;
  }
  uint8_t module = atoi(argv[1]);
  if (module <= 0 || module > 4) {
    Serial.println(F("Module ID not found!"));
    return;
  }
  pillsToDrop[module-1] = 1; // queue a single pill for this module
}

void handlePillDropAll(int argc, char* argv[]){
  if (argc < 4) {
    Serial.println(F("Usage: drpall <num> <num> <num> <num>"));
    return;
  }
  pillsToDrop[0] = atoi(argv[1]);
  pillsToDrop[1] = atoi(argv[2]);
  pillsToDrop[2] = atoi(argv[3]);
  pillsToDrop[3] = atoi(argv[4]);
}

void startPillDrop(uint8_t module) {
  updateScreen(LOADING);
  currentModule = module;
  busy = true;
  retryCount = 0;
  setState(CLOSE_SLIDER);
  stateStart = millis();
  Serial.print(F("Dropping pill from module "));
  Serial.println(module+1);
}

// Transition state and set outputs for entry actions
void setState(PillDropState newState) {
  dropState = newState;
  stateStart = millis();

  ModulePins &m = modules[currentModule];

  switch (newState) {
    case CLOSE_SLIDER:
      TCA.write1(m.motA, HIGH);
      TCA.write1(m.motB, LOW);
      break;

    case VIBRATE_RETRY:
      TCA.write1(m.vibr, HIGH);
      break;

    case OPEN_SLIDER:
      TCA.write1(m.motA, LOW);
      TCA.write1(m.motB, HIGH);
      break;

    default:
      break;
  }
}

// State machine step (call frequently from loop)
void updatePillDrop() {
  if (!busy) return;
  unsigned long t = millis();
  ModulePins &m = modules[currentModule];

  switch (dropState) {
    case CLOSE_SLIDER:
      // Close slider for 250ms, then release and check sensor
      if (t - stateStart >= 250) {
        TCA.write1(m.motA, LOW);
        TCA.write1(m.motB, LOW);
        setState(CHECK_SENSOR);
      }
      break;

    case CHECK_SENSOR:
      // Sensor LOW means pill present
      if (TCA.read1(m.sns) == LOW) {
        sns_init = true;
        setState(OPEN_SLIDER);
      } else {
        sns_init = false;
        if (retryCount < 5) {
          setState(VIBRATE_RETRY);
        } else {
          setState(OPEN_SLIDER);
        }
      }
      break;

    case VIBRATE_RETRY:
      // 0..400ms: vibrator ON
      // 400..500ms: vibrator OFF
      // 500..600ms: nudge slider closed briefly
      // >=700ms: release & re-check sensor
      if (t - stateStart >= 400 && t - stateStart < 500) {
        TCA.write1(m.vibr, LOW);
      }
      if (t - stateStart >= 500 && t - stateStart < 600) {
        TCA.write1(m.motA, HIGH);
        TCA.write1(m.motB, LOW);
      }
      if (t - stateStart >= 700) {
        TCA.write1(m.motA, LOW);
        TCA.write1(m.motB, LOW);
        retryCount++;
        setState(CHECK_SENSOR);
      }
      break;

    case OPEN_SLIDER:
      // Open slider for 250ms then release, go to FINISH
      if (t - stateStart >= 250) {
        TCA.write1(m.motA, LOW);
        TCA.write1(m.motB, LOW);
        setState(FINISH);
      }
      break;

    case FINISH:
      // If sensor was initially present and now is HIGH, pill dropped
      if (sns_init && TCA.read1(m.sns) == HIGH) {
        pillsDropped[currentModule]++;
        pillsToDrop[currentModule]--;
        if ((pillsToDrop[0]+pillsToDrop[1]+pillsToDrop[2]+pillsToDrop[3]) <= 0) {
          Serial.println(F("$drp ok"));
          updateScreen(SUCCESS);
        }
        screenTimeout = millis() + 5000; // success screen duration
      } else {
        Serial.println(F("$drp fail"));
        updateScreen(PILL_STUCK);
      }
      busy = false;
      dropState = IDLE;
      break;

    case IDLE:
    default:
      break;
  }
}

void handleSetTime(int argc, char* argv[]) {
  if (argc < 4) {
    Serial.println(F("Usage: set_time HH MM SS"));
    return;
  }
  time_HH = atoi(argv[1]);
  time_MM = atoi(argv[2]);
  time_SS = atoi(argv[3]);
  heartbeatRaspberryTime = millis();
  if (error_code == 0x0F) {
    updateScreen(TIME);
    error_code = 0x00;
  }
  Serial.print(F("Time set to: ")); Serial.print(time_HH); Serial.print(F(":"));
  Serial.print(time_MM); Serial.print(F(":")); Serial.println(time_SS);
}

// EEPROM helpers + CLI
void handleEEPROM(int argc, char* argv[]) {
  if(argc < 2) {
    // Dump entire EEPROM in 16-byte lines
    uint8_t addr = 0;
    for(uint8_t row = 0; row < 16; row++) {
      Serial.print(F("$MEM 0x"));
      if(row == 0) Serial.print("0");
      Serial.print(row * 16, HEX);
      Serial.print(F(": "));
      for(uint8_t col = 0; col < 16; col++) {
        byte val = readEEPROM(addr++);
        if(val < 16) Serial.print(F("0"));
        Serial.print(val, HEX);
        Serial.print(F(" "));
      }
      Serial.println();
    }
    return;
  }

  if(strcmp_P(argv[1], PSTR("-r")) == 0) { // read
    if(argc < 3) {
      Serial.println(F("Usage: mem <-r addr length|-w addr val1 val2 ...>"));
      return;
    }
    uint8_t addr = strtoul(argv[2], nullptr, 16);
    uint8_t length = 1;
    if(argc >= 4) { length = strtoul(argv[3], nullptr, 16); }

    uint8_t endAddr = addr + length;
    uint8_t lineStart = addr;

    while(addr < endAddr) {
      Serial.print(F("$MEM 0x"));
      if(lineStart < 16) Serial.print("0");
      Serial.print(lineStart, HEX);
      Serial.print(F(": "));

      for(uint8_t i = 0; i < 16 && addr < endAddr; i++, addr++) {
        byte val = readEEPROM(addr);
        if(val < 16) Serial.print(F("0"));
        Serial.print(val, HEX);
        Serial.print(F(" "));
      }
      Serial.println();
      lineStart = addr;
    }
    return;
  }

  if(strcmp_P(argv[1], PSTR("-w")) == 0) { // write
    if(argc < 4) {
      Serial.println(F("Usage: mem <-r addr length|-w addr val1 val2 ...>"));
      return;
    }
    uint8_t addr = strtoul(argv[2], nullptr, 16);
    int bytesToWrite = argc - 3;
    int written = 0;

    while(written < bytesToWrite) {
      Serial.print(F("$MEM 0x"));
      if(addr < 16) Serial.print("0");
      Serial.print(addr, HEX);
      Serial.print(F(": "));

      for(uint8_t i = 0; i < 16 && written < bytesToWrite; i++, written++, addr++) {
        uint8_t val = strtoul(argv[3 + written], nullptr, 16);
        writeEEPROM(addr, val);
        if(val < 16) Serial.print(F("0"));
        Serial.print(val, HEX);
        Serial.print(F(" "));
      }
      Serial.println();
    }
    return;
  }

  Serial.println(F("Usage: mem <-r addr length|-w addr val1 val2 ...>"));
}

void handleChangeScreenWindow(int argc, char* argv[]){
  if(argc < 1){
    Serial.println(F("Usage: screen <LOADING|TIME|MESSAGE|ERROR|TAKE_PILL|SUCCESS|PILL_STUCK>"));
  }

  if      (strcmp_P(argv[1], PSTR("LOADING"))    == 0){ updateScreen(LOADING); }
  else if (strcmp_P(argv[1], PSTR("TIME"))       == 0){ updateScreen(TIME); }
  else if (strcmp_P(argv[1], PSTR("MESSAGE"))    == 0){ updateScreen(MESSAGE); }
  else if (strcmp_P(argv[1], PSTR("ERROR"))      == 0){ updateScreen(ERROR); }
  else if (strcmp_P(argv[1], PSTR("TAKE_PILL"))  == 0){ updateScreen(TAKE_PILL); }
  else if (strcmp_P(argv[1], PSTR("SUCCESS"))    == 0){ updateScreen(SUCCESS); screenTimeout = millis() + 5000; }
  else if (strcmp_P(argv[1], PSTR("PILL_STUCK")) == 0){ updateScreen(PILL_STUCK); }
  else { Serial.println(F("Allowed states are: <LOADING|TIME|MESSAGE|ERROR|TAKE_PILL|SUCCESS|PILL_STUCK>")); }
}

void handleDisplayMsg(int argc, char* argv[]) {
  if (argc < 2) {
    Serial.println(F("Usage: msg <message>"));
    return;
  }

  // Build message from args 1..N (space separated)
  globalMessage[0] = '\0';
  for (int i = 1; i < argc; i++) {
    strncat(globalMessage, argv[i], MAX_MESSAGE_LEN - strlen(globalMessage) - 1);
    if (i < argc - 1) strncat(globalMessage, " ", MAX_MESSAGE_LEN - strlen(globalMessage) - 1);
  }

  Serial.print(F("Display message: "));
  Serial.println(globalMessage);
  updateScreen(MESSAGE);
}

void handleDisplayErr(int argc, char* argv[]){
  if (argc < 1) {
    Serial.println(F("Usage: err <code>"));
    return;
  }
  error_code = atoi(argv[1]);
  updateScreen(ERROR);
}

void handleI2CScan(int argc, char* argv[]){
  (void)argc; (void)argv; // unused

  Serial.print(F("$I2C devices: ["));
  byte error, address;
  int nDevices = 0;
  for(address = 1; address < 127; address++ ){
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0){
      if(nDevices > 0) Serial.print(F(", "));
      Serial.print(F("0x"));
      if (address<16) Serial.print(F("0"));
      Serial.print(address, HEX);
      nDevices++;
    }
  }
  Serial.print(F("] = "));
  Serial.println(nDevices);
}

// ================================================================
//                         BUTTON HANDLER
// ================================================================
void handleButtonActions(){
  unsigned long currentTime = millis();

  for (byte i = 0; i < buttonCount; i++) {
    bool rawReading = digitalRead(buttonPins[i]);

    // Debounce: detect edge on raw reading, set timer
    if (rawReading != lastReading[i]) {
      lastDebounceTime[i] = currentTime;
      lastReading[i]      = rawReading;
    }

    // If stable beyond debounce delay, treat as a state change
    if ((currentTime - lastDebounceTime[i]) > debounceDelay) {
      if (rawReading != buttonState[i]) {
        buttonState[i] = rawReading;

        if (buttonState[i] == LOW) {
          // Just pressed
          pressStartTime[i]    = currentTime;
          longPressReported[i] = false;

          // Special immediate action: if OK while stuck, retry
          if(buttonNames[i] == "OK" && screenState == PILL_STUCK){
            startPillDrop(currentModule);
          }
        } else {
          // Just released
          unsigned long pressDuration = currentTime - pressStartTime[i];
          if (pressDuration < longPressTime) {
            Serial.print(F("$Button ")); Serial.print(buttonNames[i]); Serial.println(F(" short press"));
          } else if (!longPressReported[i]) {
            Serial.print(F("$Button ")); Serial.print(buttonNames[i]); Serial.println(F(" long press"));
          }
        }
      }
    }

    // Long press while holding
    if (buttonState[i] == LOW && !longPressReported[i]) {
      if ((currentTime - pressStartTime[i]) >= longPressTime) {
        Serial.print(F("$Button ")); Serial.print(buttonNames[i]); Serial.println(F(" long press"));
        longPressReported[i] = true;

        // Long-press actions (kept as-is)
        if(buttonNames[i] == "OK"){
          updateScreen(TIME);
        } else if(buttonNames[i] == "DOWN"){
          pillsToDrop[0] = 1;
        }
      }
    }
  }
}

// ================================================================
//                         EEPROM I2C HELPERS
// ================================================================
void writeEEPROM(unsigned int addr, byte data) {
  Wire.beginTransmission(EEPROM_I2C_ADDRESS);
  Wire.write((byte)(addr & 0xFF)); // 8-bit memory address
  Wire.write(data);
  Wire.endTransmission();
  delay(5); // EEPROM write cycle time
}

byte readEEPROM(unsigned int addr) {
  Wire.beginTransmission(EEPROM_I2C_ADDRESS);
  Wire.write((byte)(addr & 0xFF)); // 8-bit memory address
  Wire.endTransmission();

  Wire.requestFrom(EEPROM_I2C_ADDRESS, 1);
  if (Wire.available()) return Wire.read();
  return 0xFF; // default if nothing returned
}

// ================================================================
//                         SCREEN RENDERING
// ================================================================
void screen_show_loading(){
  display.clearDisplay();
  display.invertDisplay(false);
  display.setTextSize(1);
  display.setTextWrap(true);
  display.setTextColor(WHITE);
  display.setCursor(8, 12);
  display.println(F("Please wait"));
  for(int i=0; i<5; i++){
    int posX = display.width()-24 + cos(millis()/150.0-i*0.75)*10;
    int posY = display.height()/2 + sin(millis()/150.0-i*0.75)*10;
    display.fillCircle(posX, posY, 2, WHITE);
  }
  display.display();
}

void screen_show_time(){
  display.clearDisplay();
  display.invertDisplay(false);
  display.setTextSize(3);
  display.setTextWrap(true);
  display.setTextColor(WHITE);
  display.setCursor(0, 4);
  if(time_HH < 10) display.print("0");
  display.print(time_HH);
  display.print(":");
  if(time_MM < 10) display.print("0");
  display.print(time_MM);
  display.setTextSize(2);
  display.setCursor(90, 12);
  display.print(":");
  if(time_SS < 10) display.print("0");
  display.print(time_SS);
  display.display();
}

void screen_show_message(){
  display.clearDisplay();
  display.invertDisplay(false);
  display.setTextSize(1);
  display.setTextWrap(true);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.print(globalMessage);
  display.display();
}

void screen_show_pill_stuck(){
  display.clearDisplay();
  display.invertDisplay(false);
  display.setTextWrap(false);
  display.setTextColor(WHITE);

  if(millis()/3000%2==0){
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("Error 0x01"));
    display.setCursor(0, 10);
    display.setTextSize(2);
    display.println(F("Pill stuck"));
  } else {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("Shake device gently\nthen press ok to\nretry!"));
  }
  display.display();
}

void screen_show_error(){
  display.clearDisplay();
  display.invertDisplay(false);
  display.setTextSize(2);
  display.setTextWrap(true);
  display.setTextColor(WHITE);
  display.setCursor(4, 8);
  display.print(F("ERROR "));
  display.print(F("0x"));
  if(error_code < 16) display.print("0");
  display.print(error_code, HEX);
  display.display();
}

void screen_show_take_pill(){
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextWrap(true);
  display.setTextColor(WHITE);
  display.setCursor(3, 2);

  display.print(F("TAKE PILLS"));
  display.setTextSize(1);
  display.setCursor(3, 20);
  display.print(F("Press OK to dispense"));
  display.invertDisplay((millis()/500)%2==0);

  display.display();
}

void screen_show_success(){
  display.clearDisplay();
  display.invertDisplay(false);
  display.setTextWrap(false);
  display.setTextColor(WHITE);

  display.setTextSize(1);
  display.setCursor(0, 6);
  display.print(F("PILLS DISPENSED."));
  display.setCursor(0, 16);
  display.print(F("Have a nice day!"));

  display.setTextSize(3);
  display.setCursor(98, 4);
  display.print(F(":"));
  display.setCursor(110, 4);
  display.print(F(")"));

  display.display();
}
