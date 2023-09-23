// notes
// Board: Arduino Leonardo
// COM port: set to COM222

// Uncomment this line to activate an advanced mouse mode
// Need to install AdvMouse library (copy /library/AdvMouse to the Arduino libraries)
#define ADVANCE_MODE

#include "usb_rename.h"
USBRename dummy = USBRename("Evoluent VMD", "mon.im");

#include <SPI.h>
#include <avr/pgmspace.h>

// PJRC encoder library
#include <Encoder.h>

#ifdef ADVANCE_MODE
  // install manually from PMW3360_Arduino/library/AdvMouse
  #include <AdvMouse.h>
  #define MOUSE_BEGIN       AdvMouse.begin()
  #define MOUSE_PRESS(x)    AdvMouse.press_(x)
  #define MOUSE_RELEASE(x)  AdvMouse.release_(x)
#else
  #include <Mouse.h>
  #define MOUSE_BEGIN       Mouse.begin()
  #define MOUSE_PRESS(x)    Mouse.press(x)
  #define MOUSE_RELEASE(x)  Mouse.release(x)
#endif

// Configurations
// The default CPI value should be in between 100 -- 12000
#define CPI       800
#define DEBOUNCE  30   //unit = ms.

// Registers
#define Product_ID  0x00
#define Revision_ID 0x01
#define Motion  0x02
#define Delta_X_L 0x03
#define Delta_X_H 0x04
#define Delta_Y_L 0x05
#define Delta_Y_H 0x06
#define SQUAL 0x07
#define Raw_Data_Sum  0x08
#define Maximum_Raw_data  0x09
#define Minimum_Raw_data  0x0A
#define Shutter_Lower 0x0B
#define Shutter_Upper 0x0C
#define Control 0x0D
#define Config1 0x0F
#define Config2 0x10
#define Angle_Tune  0x11
#define Frame_Capture 0x12
#define SROM_Enable 0x13
#define Run_Downshift 0x14
#define Rest1_Rate_Lower  0x15
#define Rest1_Rate_Upper  0x16
#define Rest1_Downshift 0x17
#define Rest2_Rate_Lower  0x18
#define Rest2_Rate_Upper  0x19
#define Rest2_Downshift 0x1A
#define Rest3_Rate_Lower  0x1B
#define Rest3_Rate_Upper  0x1C
#define Observation 0x24
#define Data_Out_Lower  0x25
#define Data_Out_Upper  0x26
#define Raw_Data_Dump 0x29
#define SROM_ID 0x2A
#define Min_SQ_Run  0x2B
#define Raw_Data_Threshold  0x2C
#define Config5 0x2F
#define Power_Up_Reset  0x3A
#define Shutdown  0x3B
#define Inverse_Product_ID  0x3F
#define LiftCutoff_Tune3  0x41
#define Angle_Snap  0x42
#define LiftCutoff_Tune1  0x4A
#define Motion_Burst  0x50
#define LiftCutoff_Tune_Timeout 0x58
#define LiftCutoff_Tune_Min_Length  0x5A
#define SROM_Load_Burst 0x62
#define Lift_Config 0x63
#define Raw_Data_Burst  0x64
#define LiftCutoff_Tune2  0x65

const int ncs = A1;  // This is the SPI "slave select" pin that the sensor is hooked up to

unsigned long dpi_led_timeout = 5000;

typedef struct {
  int pin;
  char key;
  bool state;
  uint8_t debounce;
} button_t;

#define DPI_BTN 0x40

button_t buttons[] = {
  {10, MOUSE_LEFT},
  {6,  MOUSE_RIGHT},
  {4,  MOUSE_MIDDLE},
  // scrollwheel click: I don't like it, disable it
  //{12,  MOUSE_MIDDLE},
  // back/forward swapped between VMC and VMD
  {A0, MOUSE_BACK},
  {2,  MOUSE_FORWARD},
  {8,  DPI_BTN},
};

Encoder scrollwheel(5, 9);

int dpi_steps[] = {
  600,
  800,
  1200,
  1600,
};
int dpi_pins[] = {0, 1, 11, 3};
int current_dpi = 0;

#define lengthof(x) (sizeof(x) / sizeof(*(x)))

byte initComplete = 0;
bool inBurst = false;   // in busrt mode
bool reportSQ = false;  // report surface quality
int16_t dx, dy;

unsigned long lastTS;
unsigned long lastButtonCheck = 0;

//Be sure to add the SROM file into this sketch via "Sketch->Add File"
extern const unsigned short firmware_length;
extern const unsigned char firmware_data[];

void setup() {
  Serial.begin(9600);

  pinMode(ncs, OUTPUT);

  for(int i = 0; i < lengthof(buttons); i++) {
    pinMode(buttons[i].pin, INPUT_PULLUP);
  }
  for(int i = 0; i < lengthof(dpi_pins); i++) {
    pinMode(dpi_pins[i], INPUT);
    digitalWrite(dpi_pins[i], LOW);
  }

  // no pull on encoders
  pinMode(5, INPUT);
  pinMode(9, INPUT);

  SPI.begin();
  SPI.setDataMode(SPI_MODE3);
  SPI.setBitOrder(MSBFIRST);
  //SPI.setClockDivider(4);
  Serial.println("ON");

  performStartup();

  // cheeky way to get us to DPI #2
  advance_cpi();

  dx = dy = 0;

  delay(1000);

  //dispRegisters();
  initComplete = 9;

  lastTS = micros();

  MOUSE_BEGIN;
}

void adns_com_begin() {
  digitalWrite(ncs, LOW);
}

void adns_com_end() {
  digitalWrite(ncs, HIGH);
}

byte adns_read_reg(byte reg_addr) {
  adns_com_begin();

  // send adress of the register, with MSBit = 0 to indicate it's a read
  SPI.transfer(reg_addr & 0x7f );
  delayMicroseconds(35); // tSRAD
  // read data
  byte data = SPI.transfer(0);

  delayMicroseconds(1); // tSCLK-NCS for read operation is 120ns
  adns_com_end();
  delayMicroseconds(19); //  tSRW/tSRR (=20us) minus tSCLK-NCS

  return data;
}

void adns_write_reg(byte reg_addr, byte data) {
  adns_com_begin();

  //send adress of the register, with MSBit = 1 to indicate it's a write
  SPI.transfer(reg_addr | 0x80 );
  //sent data
  SPI.transfer(data);

  delayMicroseconds(20); // tSCLK-NCS for write operation
  adns_com_end();
  delayMicroseconds(100); // tSWW/tSWR (=120us) minus tSCLK-NCS. Could be shortened, but is looks like a safe lower bound
}

void adns_upload_firmware() {
  // send the firmware to the chip, cf p.18 of the datasheet
  Serial.println("Uploading firmware...");

  //Write 0 to Rest_En bit of Config2 register to disable Rest mode.
  adns_write_reg(Config2, 0x00);

  // write 0x1d in SROM_enable reg for initializing
  adns_write_reg(SROM_Enable, 0x1d);

  // wait for more than one frame period
  delay(10); // assume that the frame rate is as low as 100fps... even if it should never be that low

  // write 0x18 to SROM_enable to start SROM download
  adns_write_reg(SROM_Enable, 0x18);

  // write the SROM file (=firmware data)
  adns_com_begin();
  SPI.transfer(SROM_Load_Burst | 0x80); // write burst destination adress
  delayMicroseconds(15);

  // send all bytes of the firmware
  unsigned char c;
  for (int i = 0; i < firmware_length; i++) {
    c = (unsigned char)pgm_read_byte(firmware_data + i);
    SPI.transfer(c);
    delayMicroseconds(15);
  }

  //Read the SROM_ID register to verify the ID before any other register reads or writes.
  adns_read_reg(SROM_ID);

  //Write 0x00 (rest disable) to Config2 register for wired mouse or 0x20 for wireless mouse design.
  adns_write_reg(Config2, 0x00);

  adns_com_end();
}

void setCPI(int cpi)
{
  int cpival = constrain((cpi/100)-1, 0, 0x77); // limits to 0--119


  adns_com_begin();
  adns_write_reg(Config1, cpival);
  adns_com_end();

  Serial.print("Got ");
  Serial.println(cpi);
  Serial.print("Set cpi to ");
  Serial.println((cpival + 1)*100);
}

void advance_cpi(void) {
  // first click: show LEDs
  // second click: change
  if(dpi_led_timeout) {
    current_dpi++;
    current_dpi %= 4;
    setCPI(dpi_steps[current_dpi]);
  }

  dpi_led_timeout = 5000;

  // all off
  for(int i = 0; i < lengthof(dpi_pins); i++) {
    pinMode(dpi_pins[i], INPUT);
  }
  // on goes on
  for(int i = 0; i <= current_dpi ; i++) {
    pinMode(dpi_pins[i], OUTPUT);
  }
}

void performStartup(void) {
  // hard reset
  adns_com_end(); // ensure that the serial port is reset
  adns_com_begin(); // ensure that the serial port is reset
  adns_com_end(); // ensure that the serial port is reset

  adns_write_reg(Shutdown, 0xb6); // Shutdown first
  delay(300);

  adns_com_begin(); // drop and raise ncs to reset spi port
  delayMicroseconds(40);
  adns_com_end();
  delayMicroseconds(40);

  adns_write_reg(Power_Up_Reset, 0x5a); // force reset
  delay(50); // wait for it to reboot

  // read registers 0x02 to 0x06 (and discard the data)
  adns_read_reg(Motion);
  adns_read_reg(Delta_X_L);
  adns_read_reg(Delta_X_H);
  adns_read_reg(Delta_Y_L);
  adns_read_reg(Delta_Y_H);
  // upload the firmware
  adns_upload_firmware();
  delay(10);

  setCPI(CPI);
  Serial.println("Optical Chip Initialized");
}

// Button state checkup routine
void check_button_state()
{
  // runs only after initialization
  if(initComplete != 9)
    return;

  unsigned long elapsed = micros() - lastButtonCheck;

  // Update at a period of 1/8 of the DEBOUNCE time
  if(elapsed < (DEBOUNCE * 1000UL / 8))
    return;

  lastButtonCheck = micros();

  // Fast Debounce (works with 0 latency most of the time)
  for(int i = 0; i < lengthof(buttons); i++) {
    int btn_state = !digitalRead(buttons[i].pin);
    if(!buttons[i].debounce && btn_state != buttons[i].state) {
      buttons[i].state = btn_state;
      buttons[i].debounce = DEBOUNCE;

      if((buttons[i].key == DPI_BTN) && btn_state) {
        advance_cpi();
      } else {
        if(btn_state) {
          MOUSE_PRESS(buttons[i].key);
        } else {
          MOUSE_RELEASE(buttons[i].key);
        }
      }
    }
  }
}

// device signature
void dispRegisters(void) {
  int oreg[7] = {
    0x00, 0x3F, 0x2A, 0x02
  };
  char* oregname[] = {
    "Product_ID", "Inverse_Product_ID", "SROM_Version", "Motion"
  };
  byte regres;

  digitalWrite(ncs, LOW);

  int rctr = 0;
  for (rctr = 0; rctr < 4; rctr++) {
    SPI.transfer(oreg[rctr]);
    delay(1);
    Serial.println("---");
    Serial.println(oregname[rctr]);
    Serial.println(oreg[rctr], HEX);
    regres = SPI.transfer(0);
    Serial.println(regres, BIN);
    Serial.println(regres, HEX);
    delay(1);
  }
  digitalWrite(ncs, HIGH);
}

void loop() {
  byte burstBuffer[12];
  unsigned long elapsed = micros() - lastTS;

  check_button_state();
  // keep it updating fast
  scrollwheel.read();

  if(!inBurst)
  {
    adns_write_reg(Motion_Burst, 0x00); // start burst mode
    lastTS = micros();
    inBurst = true;
  }

  if(elapsed > 870)  // polling interval : more than > 0.5 ms.
  {
    adns_com_begin();
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE3));

    SPI.transfer(Motion_Burst);
    delayMicroseconds(35); // waits for tSRAD

    SPI.transfer(burstBuffer, 12); // read burst buffer
    delayMicroseconds(1); // tSCLK-NCS for read operation is 120ns

    SPI.endTransaction();
    /*
    BYTE[00] = Motion    = if the 7th bit is 1, a motion is detected.
           ==> 7 bit: MOT (1 when motion is detected)
           ==> 3 bit: 0 when chip is on surface / 1 when off surface
           ] = Observation
    BYTE[02] = Delta_X_L = dx (LSB)
    BYTE[03] = Delta_X_H = dx (MSB)
    BYTE[04] = Delta_Y_L = dy (LSB)
    BYTE[05] = Delta_Y_H = dy (MSB)
    BYTE[06] = SQUAL     = Surface Quality register, max 0x80
                         - Number of features on the surface = SQUAL * 8
    BYTE[07] = Raw_Data_Sum   = It reports the upper byte of an 18‐bit counter which sums all 1296 raw data in the current frame;
                               * Avg value = Raw_Data_Sum * 1024 / 1296
    BYTE[08] = Maximum_Raw_Data  = Max raw data value in current frame, max=127
    BYTE[09] = Minimum_Raw_Data  = Min raw data value in current frame, max=127
    BYTE[10] = Shutter_Upper     = Shutter LSB
    BYTE[11] = Shutter_Lower     = Shutter MSB, Shutter = shutter is adjusted to keep the average raw data values within normal operating ranges
    */

    int motion = (burstBuffer[0] & 0x80) > 0;
    int surface = (burstBuffer[0] & 0x08) > 0;   // 0 if on surface / 1 if off surface

    int xl = burstBuffer[2];
    int xh = burstBuffer[3];
    int yl = burstBuffer[4];
    int yh = burstBuffer[5];

    int squal = burstBuffer[6];

    int x = xh<<8 | xl;
    int y = yh<<8 | yl;

    dx += x;
    dy += y;

    adns_com_end();

    // update only if a movement is detected.

#ifdef ADVANCE_MODE
    // wheel is read at 2x clicks, so halve it
    signed char wheel = scrollwheel.read();
    if(AdvMouse.needSendReport() || motion || wheel)
    {
      AdvMouse.move(dx, dy, wheel/2);

      dx = 0;
      dy = 0;
      scrollwheel.write(wheel % 2);
    }
#else
    if(motion)
    {
      signed char mdx = constrain(dx, -127, 127);
      signed char mdy = constrain(dy, -127, 127);

      Mouse.move(mdx, mdy, 0);

      dx = 0;
      dy = 0;
    }
#endif

    if(reportSQ && !surface)  // print surface quality
    {
      Serial.println(squal);
    }

    for(int i = 0; i < lengthof(buttons); i++) {
      if(buttons[i].debounce) {
        buttons[i].debounce--;
      }
    }

    if(dpi_led_timeout) {
      if(!--dpi_led_timeout) {
        for(int i = 0; i < lengthof(dpi_pins); i++) {
          pinMode(dpi_pins[i], INPUT);
        }
      }
    }

    lastTS = micros();
  }

  // command process routine
  if(Serial.available() > 0)
  {
    char c = Serial.read();
    switch(c)
    {
      case 'Q':   // Toggle reporting surface quality
        reportSQ = !reportSQ;
        break;
      case 'I':   // sensor info (signature)
        inBurst = false;
        dispRegisters();
        break;
      case 'C':   // set CPI
        int newCPI = readNumber();
        setCPI(newCPI);
        break;
    }
  }
}

unsigned long readNumber()
{
  String inString = "";
  for (int i = 0; i < 10; i++)
  {
    while (Serial.available() == 0);
    int inChar = Serial.read();
    if (isDigit(inChar))
    {
      inString += (char)inChar;
    }

    if (inChar == '\n')
    {
      int val = inString.toInt();
      return (unsigned long)val;
    }
  }

  // flush remain strings in serial buffer
  while (Serial.available() > 0)
  {
    Serial.read();
  }
  return 0UL;
}
