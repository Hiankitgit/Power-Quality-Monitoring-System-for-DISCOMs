// ============================================================
//  POWER QUALITY MONITOR — Arduino Uno Sampler
//  DCRUST BTech Project | Restored Full Debug Prints
// ============================================================

#include <SoftwareSerial.h>

#define ADC_PIN      A0
#define SS_TX_PIN     3
#define SS_RX_PIN     4
#define LED_PIN      13

#define NUM_SAMPLES         192
#define SAMPLE_INTERVAL_US  104

#define ACTUAL_MAINS_V  230.0f

float voltageScale = 1.0f;
int   dcBias       = 512;

SoftwareSerial stmSerial(SS_RX_PIN, SS_TX_PIN);

bool calibrate() {
  long biasSum = 0;
  for (int i = 0; i < 500; i++) {
    biasSum += analogRead(ADC_PIN);
    delayMicroseconds(50);
  }
  dcBias = (int)(biasSum / 500L);

  long calSumSq   = 0L;
  int calSamples  = NUM_SAMPLES * 2;
  for (int i = 0; i < calSamples; i++) {
    unsigned long slotStart = micros();
    int unbiased = analogRead(ADC_PIN) - dcBias;
    calSumSq += (long)unbiased * unbiased;
    while ((micros() - slotStart) < (unsigned long)SAMPLE_INTERVAL_US);
  }
  float rawRMS = sqrt((float)calSumSq / (float)calSamples);
  
  if (rawRMS < 80.0f) {  
    return false; 
  }

  voltageScale = ACTUAL_MAINS_V / rawRMS;
  Serial.println("[CAL] Calibration successful.");
  Serial.print("[CAL] bias=");  Serial.print(dcBias);
  Serial.print("  rawRMS=");    Serial.print(rawRMS, 2);
  Serial.print("  scale=");     Serial.println(voltageScale, 4);
  return true;
}

void setup() {
  Serial.begin(115200);
  stmSerial.begin(19200);

  ADCSRA &= ~(bit(ADPS2) | bit(ADPS1) | bit(ADPS0));
  ADCSRA |=   bit(ADPS2) | bit(ADPS0);

  Serial.println("[INIT] Waiting for stable mains power...");
  while (!calibrate()) {
    delay(1000); 
  }
  Serial.println("[INIT] Sampling started.");
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  {
    int prev = analogRead(ADC_PIN) - dcBias;
    unsigned long syncStart = millis();
    while (true) {
      int curr = analogRead(ADC_PIN) - dcBias;
      if (prev < -3 && curr >= -3) break;
      prev = curr;
      if (millis() - syncStart > 100UL) break;
    }
  }

  long sumSq        = 0L;
  int  maxPeak      = 0;
  int  zcCount      = 0;
  long noiseEnergy  = 0L;
  int  lastUnbiased = 0;
  int  lastSign     = 0;

  for (int i = 0; i < NUM_SAMPLES; i++) {
    unsigned long slotStart = micros();

    int rawVal   = analogRead(ADC_PIN);
    int unbiased = rawVal - dcBias;

    sumSq += (long)unbiased * unbiased;

    int absVal = abs(unbiased);
    if (absVal > maxPeak) maxPeak = absVal;

    if (i > 0) noiseEnergy += (long)abs(unbiased - lastUnbiased);
    lastUnbiased = unbiased;

    int currentSign = 0;
    if      (unbiased >  3) currentSign =  1;
    else if (unbiased < -3) currentSign = -1;
    if (lastSign != 0 && currentSign != 0 && currentSign != lastSign) zcCount++;
    if (currentSign != 0) lastSign = currentSign;

    while ((micros() - slotStart) < (unsigned long)SAMPLE_INTERVAL_US);
  }

  float rmsADC      = sqrt((float)sumSq / (float)NUM_SAMPLES);
  float realRMS     = rmsADC * voltageScale;
  float realPeak    = (float)maxPeak * voltageScale;
  float crestFactor = (rmsADC > 1.0f) ? ((float)maxPeak / rmsADC) : 0.0f;

  if (realRMS < 80.0f || realRMS > 350.0f) {
    delay(1000); 
    while(!calibrate()) { delay(1000); }
    return;
  }

  // --- RESTORED LOCAL SERIAL MONITOR PRINTS ---
  Serial.print("RMS=");         Serial.print(realRMS, 1);
  Serial.print("V Peak=");      Serial.print(realPeak, 1);
  Serial.print("V ZC=");        Serial.print(zcCount);
  Serial.print(" Noise=");      Serial.print(noiseEnergy);
  Serial.print(" CF=");         Serial.print(crestFactor, 3);
  Serial.print(" RAW_ADC_RMS=");Serial.print(rmsADC, 2);
  Serial.print(" BIAS=");       Serial.println(dcBias);

  int rmsInt  = (int)(realRMS     * 10.0f + 0.5f);
  int peakInt = (int)(realPeak    * 10.0f + 0.5f);
  int cfInt   = (int)(crestFactor * 100.0f + 0.5f);

  char txBuf[40];
  snprintf(txBuf, sizeof(txBuf), "F:%d,%d,%d,%ld,%d\n",
           rmsInt, peakInt, zcCount, noiseEnergy, cfInt);
  stmSerial.print(txBuf);

  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
}