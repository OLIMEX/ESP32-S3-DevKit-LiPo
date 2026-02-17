/*
   ============================================================
   ESP32-S3-DevKit-Lipo
   Battery Voltage + External Power Presence Monitor
   ============================================================

   GPIO6  -> Battery measurement
   GPIO5  -> External power sense
   GPIO38 -> Onboard LED (ACTIVE LOW)

   LED ON  = External power present
   LED OFF = Running on battery
*/

//////////////////////////////////////////////////////////////
// ---------------- PIN DEFINITIONS ------------------------
//////////////////////////////////////////////////////////////

#define LED_PIN        38
#define BAT_ADC_PIN    6    // GPIO6 (Battery)
#define PWR_ADC_PIN    5    // GPIO5 (External power)

//////////////////////////////////////////////////////////////
// ------------- ADC CONFIGURATION -------------------------
//////////////////////////////////////////////////////////////

#define ADC_RESOLUTION 4095.0
#define ADC_REF        3.3

// Correct ratios from schematic
#define BAT_DIVIDER_RATIO  4.1333
#define PWR_DIVIDER_RATIO  5.6808

// 5V through divider gives ~880mV at ADC
// Anything above 400mV means external power present
#define PWR_PRESENT_THRESHOLD 400

//////////////////////////////////////////////////////////////
// ------------------- LED FUNCTIONS -----------------------
//////////////////////////////////////////////////////////////

void ledInit()
{
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // LED OFF (active LOW)
}

void ledOn()
{
  digitalWrite(LED_PIN, LOW);
}

void ledOff()
{
  digitalWrite(LED_PIN, HIGH);
}

//////////////////////////////////////////////////////////////
// ------------------- VOLTAGE READ ------------------------
//////////////////////////////////////////////////////////////

float readVoltage(uint8_t pin, float dividerRatio)
{
  uint32_t adcRaw = analogRead(pin);

  float adcVoltage = (adcRaw / ADC_RESOLUTION) * ADC_REF;
  float realVoltage = adcVoltage * dividerRatio;

  return realVoltage;
}

uint16_t readRawMilliVolts(uint8_t pin)
{
  uint32_t adcRaw = analogRead(pin);
  float adcVoltage = (adcRaw / ADC_RESOLUTION) * ADC_REF;
  return (uint16_t)(adcVoltage * 1000.0);
}

//////////////////////////////////////////////////////////////
// ---------------------- SETUP ----------------------------
//////////////////////////////////////////////////////////////

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Power Monitor Started");
  Serial.println("--------------------------------");

  analogReadResolution(12);

  ledInit();
}

//////////////////////////////////////////////////////////////
// ----------------------- LOOP ----------------------------
//////////////////////////////////////////////////////////////

void loop()
{
  float batteryVoltage = readVoltage(BAT_ADC_PIN, BAT_DIVIDER_RATIO);
  uint16_t pwrRawMv = readRawMilliVolts(PWR_ADC_PIN);

  bool externalPowerPresent = (pwrRawMv > PWR_PRESENT_THRESHOLD);

  Serial.print("Battery Voltage: ");
  Serial.print(batteryVoltage, 3);
  Serial.println(" V");

  if (externalPowerPresent)
  {
    Serial.println("External Power: PRESENT");
    ledOn();
  }
  else
  {
    Serial.println("External Power: NOT PRESENT");
    ledOff();
  }

  Serial.println("--------------------------------");
  delay(2000);
}
