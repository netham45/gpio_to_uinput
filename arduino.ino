#include <Wire.h>
#include <string.h>

// -------- I2C address (change if you want) --------
#define I2C_ADDR 0x42

// -------- Battery measurement config --------
// If using a resistor divider: VBAT -> R_TOP -> A7 -> R_BOTTOM -> GND
// divider_gain = (R_TOP + R_BOTTOM) / R_BOTTOM
static const float VREF = 5.0f;            // Nano default ADC reference (Vcc). If you use INTERNAL or external ref, change this.
static const float R_TOP = 100000.0f;      // ohms
static const float R_BOTTOM = 100000.0f;   // ohms
static const float DIV_GAIN = 1; // No Divider

// For 1S Li-ion defaults (tune to your chemistry)
static const float V_EMPTY = 3.00f;  // volts
static const float V_FULL  = 4.20f;  // volts

// Fake capacity values (tune)
static const uint16_t FULL_MAH = 5000;

// -------- Raw packet helpers --------
static inline void putU16_LE(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline uint16_t readDigitalMask_D2_to_D13() {
  uint16_t m = 0;

  uint8_t d = PIND; // D2..D7 => PORTD bits 2..7
  for (uint8_t pin = 2; pin <= 7; pin++) {
    if (d & (1 << pin)) m |= (1 << (pin - 2));
  }

  uint8_t b = PINB; // D8..D13 => PORTB bits 0..5
  for (uint8_t bit = 0; bit <= 5; bit++) {
    if (b & (1 << bit)) m |= (1 << (6 + bit));
  }

  return m;
}

// -------- Cached readings (so ISR stays fast) --------
static volatile uint16_t adcA0, adcA1, adcA2, adcA3, adcA6, adcA7;
static volatile uint16_t dmask_cache;
static volatile uint8_t raw_packet[12];

// Battery computed/cache
static volatile uint16_t vbat_mV = 3700;
static volatile uint8_t soc_pct = 50;

// -------- SBS/SMBus emulation state --------
static volatile bool sbs_pending = false;
static volatile uint8_t sbs_cmd = 0xFF;
static volatile uint16_t battery_mode = 0;

static const char MANUF[] = "Arduino";
static const char MODEL[] = "SBS-EMU";
static const char CHEM[]  = "LION";

// -------- Atomic reads --------
static inline uint16_t atomicReadU16(volatile uint16_t *vp) {
  uint16_t r;
  uint8_t sreg = SREG; cli();
  r = *vp;
  SREG = sreg;
  return r;
}
static inline uint8_t atomicReadU8(volatile uint8_t *vp) {
  uint8_t r;
  uint8_t sreg = SREG; cli();
  r = *vp;
  SREG = sreg;
  return r;
}

// -------- Battery math --------
static inline float adc_to_v(float adc) {
  // 10-bit ADC: 0..1023
  return (adc * (VREF / 1023.0f));
}

static void refresh_cache() {
  // Raw ADCs
  adcA0 = (uint16_t)analogRead(A0);
  adcA1 = (uint16_t)analogRead(A1);
  adcA2 = (uint16_t)analogRead(A2);
  adcA3 = (uint16_t)analogRead(A3);
  adcA6 = (uint16_t)analogRead(A6);
  adcA7 = (uint16_t)analogRead(A7);

  dmask_cache = readDigitalMask_D2_to_D13();

  // Update raw 12-byte packet (unchanged format)
  uint8_t out[12];
  putU16_LE(&out[0],  adcA0);
  putU16_LE(&out[2],  adcA1);
  putU16_LE(&out[4],  adcA2);
  putU16_LE(&out[6],  adcA3);
  putU16_LE(&out[8],  adcA6);
  putU16_LE(&out[10], dmask_cache);

  uint8_t sreg = SREG; cli();
  memcpy((void*)raw_packet, out, sizeof(out));
  SREG = sreg;

  // Battery voltage from A7:
  // v_a7 = adc_to_v(adcA7)
  // vbat = v_a7 * DIV_GAIN
  float v_a7 = adc_to_v((float)adcA7);
  float vbat = v_a7 * DIV_GAIN;

  // Simple linear SOC estimate (good enough to get a driver working)
  float soc_f = (vbat - V_EMPTY) * 100.0f / (V_FULL - V_EMPTY);
  if (soc_f < 0.0f) soc_f = 0.0f;
  if (soc_f > 100.0f) soc_f = 100.0f;

  // Light smoothing so the kernel doesn't see jittery % (EMA)
  static float soc_ema = 50.0f;
  soc_ema = soc_ema * 0.90f + soc_f * 0.10f;

  uint16_t mv = (uint16_t)(vbat * 1000.0f + 0.5f);
  uint8_t pct = (uint8_t)(soc_ema + 0.5f);

  sreg = SREG; cli();
  vbat_mV = mv;
  soc_pct = pct;
  SREG = sreg;
}

// -------- SMBus write/read helpers --------
static void writeWordLE(uint16_t v) {
  uint8_t out[2];
  putU16_LE(out, v);
  Wire.write(out, 2);
}

static void writeBlockString(const char *s) {
  uint8_t len = (uint8_t)strlen(s);
  if (len > 32) len = 32;
  Wire.write(len);
  Wire.write((const uint8_t*)s, len);
}

// -------- I2C callbacks --------
void onI2CReceive(int n) {
  if (n <= 0) return;
  uint8_t cmd = (uint8_t)Wire.read();
  n--;

  // Common SBS read pattern: write(cmd) then repeated-start read()
  if (n == 0) {
    sbs_cmd = cmd;
    sbs_pending = true;
    return;
  }

  // SMBus write-word: cmd + 2 bytes (LSB, MSB)
  if (n >= 2) {
    uint8_t lo = (uint8_t)Wire.read();
    uint8_t hi = (uint8_t)Wire.read();
    uint16_t val = (uint16_t)(lo | ((uint16_t)hi << 8));
    if (cmd == 0x03) battery_mode = val; // BatteryMode
  }

  while (Wire.available()) (void)Wire.read();
  sbs_pending = false;
  sbs_cmd = 0xFF;
}

void onI2CRequest() {
  // If we got an SBS command first, answer SBS register reads
  if (sbs_pending) {
    uint8_t cmd = sbs_cmd;
    sbs_pending = false;
    sbs_cmd = 0xFF;

    const uint16_t mv   = atomicReadU16(&vbat_mV);
    const uint8_t  soc  = atomicReadU8(&soc_pct);

    switch (cmd) {
      case 0x08: writeWordLE(2982); break;                 // Temperature (0.1K): 25C ~ 298.2K
      case 0x09: writeWordLE(mv); break;                   // Voltage (mV)
      case 0x0A: writeWordLE((uint16_t)0); break;          // Current (mA) stub
      case 0x0D: writeWordLE((uint16_t)soc); break;        // RelativeStateOfCharge (%)
      case 0x0F: writeWordLE((uint16_t)((uint32_t)FULL_MAH * soc / 100)); break; // RemainingCapacity
      case 0x10: writeWordLE(FULL_MAH); break;             // FullChargeCapacity
      case 0x16: {                                         // BatteryStatus (very minimal)
        uint16_t st = 0x0080;                              // INITIALIZED
        if (soc >= 99) st |= 0x0020;                       // FULL_CHARGED
        else st |= 0x0040;                                 // DISCHARGING
        writeWordLE(st);
        break;
      }
      case 0x18: writeWordLE(FULL_MAH); break;             // DesignCapacity
      case 0x19: writeWordLE(3700); break;                 // DesignVoltage (mV)
      case 0x1A: writeWordLE((uint16_t)(2u << 4)); break;  // SpecInfo (SBS 1.1, no PEC)
      case 0x20: writeBlockString(MANUF); break;           // ManufacturerName
      case 0x21: writeBlockString(MODEL); break;           // DeviceName
      case 0x22: writeBlockString(CHEM);  break;           // DeviceChemistry
      case 0x03: writeWordLE(atomicReadU16(&battery_mode)); break; // BatteryMode
      default:   writeWordLE(0xFFFF); break;               // Unknown
    }
    return;
  }

  // Otherwise: legacy behavior — raw 12-byte packet
  uint8_t out[12];
  uint8_t sreg = SREG; cli();
  memcpy(out, (const void*)raw_packet, sizeof(out));
  SREG = sreg;

  Wire.write(out, sizeof(out));
}

void setup() {
  for (uint8_t pin = 2; pin <= 13; pin++) pinMode(pin, INPUT_PULLUP);

  Wire.begin(I2C_ADDR);

  // Disable Arduino internal pullups on SDA/SCL (Pi supplies 3.3V pullups)
  digitalWrite(SDA, LOW);
  digitalWrite(SCL, LOW);

  Wire.onReceive(onI2CReceive);
  Wire.onRequest(onI2CRequest);

  refresh_cache();
}

void loop() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last >= 50) {  // ~20Hz refresh
    last = now;
    refresh_cache();
  }
}

