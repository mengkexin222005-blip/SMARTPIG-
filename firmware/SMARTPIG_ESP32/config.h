#pragma once
// ===================== SMARTPIG configuration =====================
// Wi-Fi and Firebase login are in secrets.h (keep that file private).
// Pig age, number of pigs, feeding times and the safety settings come from
// the website (Feeding page). The values here are the hardware and the
// fixed parts of the AI feeding rule.

#define DEVICE_ID        "smartpig-01"

// ---- Pins (your wiring) ----
#define PIN_RS485_RX   5     // ESP32 RX  <-  TTL converter TXD
#define PIN_RS485_TX   4     // ESP32 TX  ->  TTL converter RXD
#define PIN_RPWM       6     // BTS7960 forward PWM
#define PIN_LPWM       7     // BTS7960 reverse PWM (R_EN and L_EN tied to 5 V)
#define PIN_I2C_SDA    8     // LCD + DS3231
#define PIN_I2C_SCL    9
#define PIN_TRAY_DT    10    // tray HX711
#define PIN_TRAY_SCK   11
#define PIN_WATER      12    // P45 float switch (other wire to GND)
#define PIN_HOPPER_DT  18    // hopper HX711 (4 cells via junction box)
#define PIN_HOPPER_SCK 17    // hopper HX711 SCK (not GPIO 19: that is the USB D- pin)

#define LCD_ADDR       0x27  // try 0x3F if the LCD stays blank
#define LCD_COLS       20    // 20x4 LCD. Use 16 and 2 for a 16x2 screen
#define LCD_ROWS       4

// Load cell calibration (counts per GRAM). Re-calibrate any time from the Serial Monitor.
#define DEFAULT_HOPPER_CAL_PER_G  15.9629f
#define DEFAULT_TRAY_CAL_PER_G    420.0f   // placeholder: calibrate the tray ("cal tray <grams>")
#define SMS_ADDR       1     // SMS2113 Modbus address

// Float switch: with INPUT_PULLUP, does a CLOSED switch (pin = LOW) mean water is OK?
#define WATER_OK_WHEN_CLOSED  true

// ---- Feeding ----
#define MAX_PIGS             3        // the website allows 1 to 3 pigs
#define MEAL_WINDOW_MIN      30       // still feed if the ESP32 was off at meal time (up to 30 min late)
#define DEFAULT_AM_RATIO     0.50f    // the website splits the day 50 / 50 (AM / PM)

// 5-day adjustment rule (10 meals = 5 days x 2 meals)
#define HIST_N               10
#define EMPTY_FRAC           0.03f    // leftover <= 3 % of the meal  -> "ate everything"
#define LEFT_FRAC            0.05f    // leftover  > 5 % of the meal  -> "left feed"
#define ADJ_STEP_PCT         5        // default step; the website's "AI step" setting overrides it
#define ADJ_MAX_PCT          20

// Dispensing safety
#define TRAY_MAX_G           3750.0f  // tray capacity (3 pigs x 2500 g / 2 meals)
#define MIN_DISPENSE_G       15.0f    // below this, skip the meal (tray already full enough)
#define RF_TRUST_FRAC        0.15f    // use the formula if the forest differs from it by > 15 % of the meal
#define FEED_PWM_FULL        255
#define FEED_PWM_SLOW        140
#define FEED_LEAD_FRAC       0.04f    // stop early by 4 % (feed still falling)
#define FEED_MAX_MS          240000UL // never run the motor longer than 4 min
#define FEED_NOPROGRESS_MS   15000UL  // no weight change for 15 s = jam / empty hopper
#define FEED_SETTLE_MS       5000UL
#define HOPPER_RESERVE_G     50.0f
#define REFILL_JUMP_G        500.0f   // hopper rise bigger than this = refill

// ---- Feeding result structure ----
struct FeedResult {
  float dispensed;
  float trayAfter;
  const char *status;
  uint32_t durationMs;
};
