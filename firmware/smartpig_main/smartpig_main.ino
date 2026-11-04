#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include "firebase_uploader.h"   // ADDED: Wi-Fi + Firebase (runs in its own task), includes feeder.h

// LCD and RTC pins
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9

// RS485 pins
#define RS485_RX_PIN 5
#define RS485_TX_PIN 4

// Water level pins
// CHANGED: 13, 14 and 15 are now used by the HX711 (H1 CLK) and the motor driver (RPWM, LPWM)
// in the schematic. Move the three float switches to these free pins (other wire to GND).
#define TOP_FLOAT_PIN 38
#define MIDDLE_FLOAT_PIN 39
#define BOTTOM_FLOAT_PIN 40

// Modbus settings
#define SENSOR_ADDRESS 1
#define MODBUS_BAUD 9600

// Change to true only when setting the RTC time
#define SET_RTC_TIME false

LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;
HardwareSerial modbusSerial(1);

float temperature = 0.0;
float humidity = 0.0;

bool sensorOK = false;
bool rtcOK = false;

String waterStatus = "BELOW LOW";

unsigned long lastSensorRead = 0;
unsigned long lastDisplayChange = 0;

int displayPage = 0;

// Modbus CRC16
uint16_t calculateCRC(byte *data, byte length) {
  uint16_t crc = 0xFFFF;

  for (byte i = 0; i < length; i++) {
    crc ^= data[i];

    for (byte j = 0; j < 8; j++) {
      if (crc & 1) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }

  return crc;
}

// Read temperature and humidity from the RS485 sensor
bool readSensor() {
  byte request[8];

  request[0] = SENSOR_ADDRESS;
  request[1] = 0x03;
  request[2] = 0x00;
  request[3] = 0x00;
  request[4] = 0x00;
  request[5] = 0x02;

  uint16_t crc = calculateCRC(request, 6);

  request[6] = crc & 0xFF;
  request[7] = (crc >> 8) & 0xFF;

  // Clear old received data
  while (modbusSerial.available()) {
    modbusSerial.read();
  }

  // Send the request
  modbusSerial.write(request, 8);
  modbusSerial.flush();

  // Receive the response
  byte response[9];
  byte count = 0;

  unsigned long startTime = millis();

  while (millis() - startTime < 500 && count < 9) {
    if (modbusSerial.available()) {
      response[count] = modbusSerial.read();
      count++;
    }
  }

  if (count != 9) {
    Serial.println("Sensor error: Timeout");
    return false;
  }

  // Check the sensor address
  if (response[0] != SENSOR_ADDRESS) {
    Serial.println("Sensor error: Wrong address");
    return false;
  }

  // Check Modbus function and data length
  if (response[1] != 0x03 || response[2] != 0x04) {
    Serial.println("Sensor error: Wrong response");
    return false;
  }

  // Check response CRC
  uint16_t receivedCRC =
      ((uint16_t)response[8] << 8) | response[7];

  uint16_t calculatedCRC = calculateCRC(response, 7);

  if (receivedCRC != calculatedCRC) {
    Serial.println("Sensor error: CRC mismatch");
    return false;
  }

  // Register 0x0000 = humidity x10
  // Register 0x0001 = temperature x10
  uint16_t rawHumidity =
      ((uint16_t)response[3] << 8) | response[4];

  int16_t rawTemperature =
      ((uint16_t)response[5] << 8) | response[6];

  humidity = rawHumidity / 10.0;
  temperature = rawTemperature / 10.0;

  Serial.print("Temperature: ");
  Serial.print(temperature, 1);
  Serial.println(" C");

  Serial.print("Humidity: ");
  Serial.print(humidity, 1);
  Serial.println(" %");

  return true;
}

// Read the three water level switches
void readWaterLevel() {
  bool top = digitalRead(TOP_FLOAT_PIN) == LOW;
  bool middle = digitalRead(MIDDLE_FLOAT_PIN) == LOW;
  bool bottom = digitalRead(BOTTOM_FLOAT_PIN) == LOW;

  if (top) {
    waterStatus = "FULL";
  } else if (middle) {
    waterStatus = "HALF FULL";
  } else if (bottom) {
    waterStatus = "ALMOST LOW";
  } else {
    waterStatus = "BELOW LOW";
  }
}

// Print a full 16-character LCD line
void printLCDLine(byte row, String message) {
  lcd.setCursor(0, row);

  for (byte i = 0; i < 16; i++) {
    if (i < message.length()) {
      lcd.print(message[i]);
    } else {
      lcd.print(' ');
    }
  }
}

// Display water level and time
void displayWaterAndTime() {
  String levelLine = "Lvl: " + waterStatus;
  printLCDLine(0, levelLine);

  if (rtcOK) {
    DateTime now = rtc.now();

    char timeText[17];

    snprintf(timeText, sizeof(timeText),
             "Time: %02d:%02d:%02d",
             now.hour(), now.minute(), now.second());

    printLCDLine(1, String(timeText));
  } else {
    printLCDLine(1, "RTC unavailable");
  }
}

// Display temperature and humidity
void displaySensorData() {
  if (sensorOK) {
    char tempText[17];
    char humText[17];

    snprintf(tempText, sizeof(tempText),
             "Temp: %.1f C", temperature);

    snprintf(humText, sizeof(humText),
             "Hum: %.1f %%", humidity);

    printLCDLine(0, String(tempText));
    printLCDLine(1, String(humText));
  } else {
    printLCDLine(0, "Temp: --.- C");
    printLCDLine(1, "Hum: --.- %");
  }
}

// Display date and water level
void displayDate() {
  if (rtcOK) {
    DateTime now = rtc.now();

    char dateText[17];

    snprintf(dateText, sizeof(dateText),
             "Date: %02d/%02d/%04d",
             now.day(), now.month(), now.year());

    printLCDLine(0, String(dateText));
  } else {
    printLCDLine(0, "RTC unavailable");
  }

  printLCDLine(1, "Lvl: " + waterStatus);
}

// NEW: hopper and tray weights
void displayWeights() {
  float g;
  char line[17];
  if (hopperGrams(g)) snprintf(line, sizeof(line), "Hopper: %.0f g", g);
  else snprintf(line, sizeof(line), "Hopper: --");
  printLCDLine(0, String(line));
  if (trayGrams(g)) snprintf(line, sizeof(line), "Tray: %.0f g", g);
  else snprintf(line, sizeof(line), "Tray: --");
  printLCDLine(1, String(line));
}

// NEW: shows the feeding on the LCD while the motor runs
void displayFeeding() {
  char line[17];
  snprintf(line, sizeof(line), "Target: %.0f g", feederCurrentTarget());
  printLCDLine(0, "FEEDING...");
  printLCDLine(1, String(line));
}

// NEW: reads the RTC once and says whether the time can be trusted
bool readRtc(DateTime &now) {
  if (!rtcOK || rtc.lostPower()) return false;
  now = rtc.now();
  return now.year() >= 2025 && now.year() <= 2099;
}

// NEW: keeps the DS3231 on Philippine time using internet time (NTP).
// - If the clock lost its time (battery removed, power cut), it is set right away.
// - Otherwise it is corrected once a day if it drifted more than 2 seconds.
// Scheduled feeding still runs from the DS3231 when there is no internet.
#define PH_OFFSET_SEC (8UL * 3600UL)   // Philippines = UTC+8
unsigned long lastRtcCheck = 0;
unsigned long lastRtcSync = 0;
bool rtcSyncedOnce = false;

void syncRtcFromInternet() {
  if (!rtcOK) return;
  if (millis() - lastRtcCheck < 60000UL && lastRtcCheck != 0) return;   // check once a minute
  lastRtcCheck = millis();

  uint64_t ms = epochMs();             // 0 until the ESP32 has internet time
  if (ms == 0) return;
  uint32_t phNow = (uint32_t)(ms / 1000ULL) + PH_OFFSET_SEC;

  DateTime rtcTime = rtc.now();
  bool invalid = rtc.lostPower() || rtcTime.year() < 2025 || rtcTime.year() > 2099;
  long drift = (long)rtcTime.unixtime() - (long)phNow;
  bool dailyCheck = !rtcSyncedOnce || millis() - lastRtcSync > 86400000UL;

  if (invalid || (dailyCheck && labs(drift) > 2)) {
    rtc.adjust(DateTime(phNow));
    Serial.printf("[rtc] Clock set from internet time (was off by %ld s).\n", invalid ? 0L : drift);
  }
  if (invalid || dailyCheck) {
    rtcSyncedOnce = true;
    lastRtcSync = millis();
  }
}

// ADDED: hands the latest real readings to the Firebase task
void publishSnapshot() {
  // Raw float states, read right after readWaterLevel()
  bool top = digitalRead(TOP_FLOAT_PIN) == LOW;
  bool middle = digitalRead(MIDDLE_FLOAT_PIN) == LOW;
  bool bottom = digitalRead(BOTTOM_FLOAT_PIN) == LOW;

  // rtcOK only means the chip answered. Also check that the time is believable.
  bool rtcValid = false;
  char dateText[11] = "";
  char timeText[9] = "";

  if (rtcOK && !rtc.lostPower()) {
    DateTime now = rtc.now();
    if (now.year() >= 2025 && now.year() <= 2099) {
      rtcValid = true;
      snprintf(dateText, sizeof(dateText), "%04d-%02d-%02d",
               now.year(), now.month(), now.day());
      snprintf(timeText, sizeof(timeText), "%02d:%02d:%02d",
               now.hour(), now.minute(), now.second());
    }
  }

  uploaderUpdate(sensorOK, temperature, humidity, waterStatus.c_str(),
                 top, middle, bottom, rtcValid, dateText, timeText);
}

void setup() {
  Serial.begin(115200);

  feederBegin();   // NEW: motor off right away, load the saved schedule
  scalesBegin();   // NEW: hopper + tray load cells

  // Initialize I2C for both LCD and RTC
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  lcd.init();
  lcd.backlight();

  printLCDLine(0, "SMARTPIG");
  printLCDLine(1, "Starting...");

  // Configure water switches
  pinMode(TOP_FLOAT_PIN, INPUT_PULLUP);
  pinMode(MIDDLE_FLOAT_PIN, INPUT_PULLUP);
  pinMode(BOTTOM_FLOAT_PIN, INPUT_PULLUP);

  // Initialize RS485 serial
  modbusSerial.begin(
      MODBUS_BAUD,
      SERIAL_8N1,
      RS485_RX_PIN,
      RS485_TX_PIN
  );

  // Initialize RTC
  rtcOK = rtc.begin();

  if (rtcOK) {
    Serial.println("DS3231 detected.");

    if (SET_RTC_TIME) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println("RTC time set from computer.");
    }
  } else {
    Serial.println("RTC not detected.");
  }

  uploaderBegin();   // ADDED: start the Wi-Fi/Firebase task

  Serial.println("SMARTPIG started.");

  delay(1500);
}

void loop() {
  unsigned long currentMillis = millis();

  // Update water level continuously
  readWaterLevel();

  // NEW: load cells, Serial Monitor calibration commands, feeding
  scalesLoop();
  scalesHandleSerial();

  syncRtcFromInternet();   // NEW: fixes the clock if it lost its time

  DateTime rtcNow;
  bool rtcValid = readRtc(rtcNow);
  feederLoop(rtcNow, rtcValid);

  // While the motor runs, skip the slow sensor read so the stop time stays accurate
  if (feederIsFeeding()) {
    displayFeeding();
    delay(20);
    return;
  }

  // Read sensor every 2 seconds
  if (currentMillis - lastSensorRead >= 2000) {
    lastSensorRead = currentMillis;
    sensorOK = readSensor();
  }

  // Rotate LCD pages every 3 seconds
  if (currentMillis - lastDisplayChange >= 3000) {
    lastDisplayChange = currentMillis;

    displayPage++;

    if (displayPage > 3) {   // CHANGED: 4 pages (weights added)
      displayPage = 0;
    }
  }

  // Show the selected LCD page
  if (displayPage == 0) {
    displayWaterAndTime();
  } else if (displayPage == 1) {
    displaySensorData();
  } else if (displayPage == 2) {
    displayDate();
  } else {
    displayWeights();
  }

  publishSnapshot();   // ADDED: share the latest readings with the network task

  delay(200);
}