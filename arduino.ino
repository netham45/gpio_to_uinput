#include <Wire.h>

#define I2C_ADDR 0x42  // slave address
bool was_polled = false;

static inline void putU16_LE(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

// Bitmask mapping (uint16):
// bits 0..11 => D2..D13
// bits 12..15 => 0
static inline uint16_t readDigitalMask_D2_to_D13() {
  uint16_t m = 0;

  // D2..D7 are PORTD bits 2..7 -> mask bits 0..5
  uint8_t d = PIND;
  for (uint8_t pin = 2; pin <= 7; pin++) {
    if (d & (1 << pin)) m |= (1 << (pin - 2));
  }

  // D8..D13 are PORTB bits 0..5 -> mask bits 6..11
  uint8_t b = PINB;
  for (uint8_t bit = 0; bit <= 5; bit++) {
    if (b & (1 << bit)) m |= (1 << (6 + bit));
  }

  return m; // upper bits remain 0
}

void onI2CRequest() {
  uint16_t a0 = (uint16_t)analogRead(A0);
  uint16_t a1 = (uint16_t)analogRead(A1);
  uint16_t a2 = (uint16_t)analogRead(A2);
  uint16_t a3 = (uint16_t)analogRead(A3);
  uint16_t a6 = (uint16_t)analogRead(A6);  // Nano analog-only pin

  uint16_t dmask = readDigitalMask_D2_to_D13();

  // 6 x uint16 = 12 bytes
  uint8_t out[12];
  putU16_LE(&out[0],  a0);
  putU16_LE(&out[2],  a1);
  putU16_LE(&out[4],  a2);
  putU16_LE(&out[6],  a3);
  putU16_LE(&out[8],  a6);
  putU16_LE(&out[10], dmask);

  was_polled = true;
  Wire.write(out, sizeof(out)); // master should request 12 bytes
}

void setup() {
  // Enable internal pull-ups on ALL digital lines D2..D13
  for (uint8_t pin = 2; pin <= 13; pin++) {
    pinMode(pin, INPUT_PULLUP);
  }

  Wire.begin(I2C_ADDR);
  Wire.onRequest(onI2CRequest);
}

void loop() {
 // Nothing
}

