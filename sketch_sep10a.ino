// ============================================================
//  POWER QUALITY MONITOR — STM32 Blue Pill Classifier
//  DCRUST BTech Project | Final Tuned Version (2V Sag Drop)
// ============================================================
#include <Arduino.h>

Uart SerialUno(PA10, PA9);      
Uart SerialRS485(PB11, PB10);   

#define RS485_DE_RE PA8

float SAG_V      = 0.0f;
float SWELL_V    = 0.0f;
float CREST_MAX  = 0.0f;
long  NOISE_MED  = 0;
long  NOISE_HIGH = 0;
const int ZC_NORMAL = 2;
const int MAX_PER_CLASS = 5;

// --- Auto-Calibration Variables ---
bool isCalibrated = false;
int  calibCount = 0;
float sumRms = 0;
float sumCf = 0;
long  sumNoise = 0;
const int REQUIRED_CALIB_SAMPLES = 100; 

int tappingCounter = 0; 

unsigned long lastHeartbeatMs = 0;
const unsigned long HEARTBEAT_MS = 10000UL;

struct Features {
  float rms; float peak; int zeroCross; long noiseEnergy; float crestFactor;
};

struct Scores {
  int tap; int loose; int overload; int degrade;
};

static uint8_t xorChecksum(const char* s) {
  uint8_t crc = 0;
  while (*s) crc ^= (uint8_t)(*s++);
  return crc;
}

static bool parsePacket(const char* raw, Features* f) {
  if (!raw || raw[0] != 'F' || raw[1] != ':') return false;
  int rmsInt = 0, peakInt = 0, cfInt = 0, zc = 0;
  long noise = 0;
  int parsed = sscanf(raw + 2, "%d,%d,%d,%ld,%d", &rmsInt, &peakInt, &zc, &noise, &cfInt);
  if (parsed != 5) return false;
  
  f->rms         = rmsInt  / 10.0f;
  f->peak        = peakInt / 10.0f;
  f->zeroCross   = zc;
  f->noiseEnergy = noise;
  f->crestFactor = cfInt   / 100.0f;
  
  if (f->rms < 50.0f || f->rms > 400.0f) return false;
  return true;
}

static void transmitRS485(const char* packet) {
  digitalWrite(RS485_DE_RE, HIGH);
  delay(1);
  SerialRS485.print(packet);
  SerialRS485.flush();
  delay(2);
  digitalWrite(RS485_DE_RE, LOW);
}

static Scores computeScores(const Features& f) {
  Scores s = {0, 0, 0, 0};

  if (f.noiseEnergy > NOISE_MED || f.rms < SAG_V) {
    if (f.noiseEnergy > NOISE_MED) {
      tappingCounter += 3; 
    } else {
      if (tappingCounter > 0) tappingCounter--; 
    }
    
    if (tappingCounter > 15) tappingCounter = 15; 

    if (tappingCounter >= 4) { 
        s.tap = 5; 
    } else if (f.rms < SAG_V) {
        s.overload = 5; 
    }
    return s;
  } else {
    tappingCounter = 0; 
  }

  if (f.noiseEnergy > NOISE_HIGH && f.zeroCross != ZC_NORMAL) {
    s.loose = 5; 
  } else if (f.crestFactor >= CREST_MAX) {
    s.degrade = 5; 
  }

  return s;
}

static void processEvent(const Features& f) {
  // If mains drops completely, force STM32 back to learning mode
  if (f.rms < 80.0f) {
    isCalibrated = false;
    calibCount = 0;
    sumRms = 0;
    sumCf = 0;
    sumNoise = 0;
    transmitRS485("\r\n[RESET] Mains lost. Re-entering calibration mode...\r\n");
    return;
  }

  bool healthy = (f.rms >= SAG_V && f.rms <= SWELL_V && 
                  f.zeroCross == ZC_NORMAL && 
                  f.crestFactor < CREST_MAX && 
                  f.noiseEnergy < NOISE_MED);
  if (healthy) {
      tappingCounter = 0; 
      return; 
  }

  Scores s = computeScores(f);
  int maxScore = max(max(s.tap, s.loose), max(s.overload, s.degrade));

  const char* type = "UNKNOWN";
  if (s.tap >= 3)          type = "TAPPING";
  else if (s.overload >= 3)type = "OVERLOAD";
  else if (s.loose >= 3)   type = "LOOSE_JOINT";
  else if (s.degrade >= 3) type = "DEGRADATION";

  int rms_i  = (int)f.rms;           int rms_d  = abs((int)(f.rms * 10) % 10);
  int pk_i   = (int)f.peak;          int pk_d   = abs((int)(f.peak * 10) % 10);
  int cf_i   = (int)f.crestFactor;   int cf_d   = abs((int)(f.crestFactor * 100) % 100);

  float conf = (float)maxScore / (float)MAX_PER_CLASS;
  if (conf > 1.0f) conf = 1.0f;
  int conf_i = (int)conf; int conf_d = abs((int)(conf * 100) % 100);

  char dsid[64];
  snprintf(dsid, sizeof(dsid), "%d.%d|%d.%d|%d|%ld|%d.%02d",
           rms_i, rms_d, pk_i, pk_d, f.zeroCross, f.noiseEnergy, cf_i, cf_d);

  char body[220];
  snprintf(body, sizeof(body),
           "NODE=N1,TIME=%lu,TYPE=%s,CONF=%d.%02d,TAP=%d,LOOSE=%d,OVR=%d,DEG=%d,DSID=%s",
           millis(), type, conf_i, conf_d, s.tap, s.loose, s.overload, s.degrade, dsid);

  uint8_t crc = xorChecksum(body);
  char full[260];
  snprintf(full, sizeof(full), "%s*%02X\r\n", body, crc);
  transmitRS485(full);
}

void setup() {
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW);
  SerialUno.begin(19200);   
  SerialRS485.begin(115200);
  delay(1500);
  
  while(SerialUno.available()) SerialUno.read();

  isCalibrated = false; // Force calibration on every boot
  transmitRS485("\r\nSYSTEM BOOT: Learning grid baseline for 3 seconds...\r\n");
}

void loop() {
  if (!isCalibrated) {
    if (SerialUno.available()) {
      char buf[64];
      int len = SerialUno.readBytesUntil('\n', buf, sizeof(buf) - 1);
      buf[len] = '\0';
      Features f;
      if (parsePacket(buf, &f)) {
        if (f.rms < 80.0f) return;

        sumRms += f.rms;
        sumCf += f.crestFactor;
        sumNoise += f.noiseEnergy;
        calibCount++;

        if (calibCount >= REQUIRED_CALIB_SAMPLES) {
          float baseRms = sumRms / REQUIRED_CALIB_SAMPLES;
          float baseCf = sumCf / REQUIRED_CALIB_SAMPLES;
          long baseNoise = sumNoise / REQUIRED_CALIB_SAMPLES;

          // TUNED FOR 800W IRON: Exactly 2V drop threshold
          SAG_V      = baseRms - 4.0f;    
          SWELL_V    = baseRms + 15.0f;   
          CREST_MAX  = baseCf + 0.08f;    
          NOISE_MED  = baseNoise + 300L;  
          NOISE_HIGH = baseNoise * 3;     

          char calMsg[150];
          int rms_i = (int)baseRms; int rms_d = abs((int)(baseRms * 10) % 10);
          int sag_i = (int)SAG_V;   int sag_d = abs((int)(SAG_V * 10) % 10);
          
          snprintf(calMsg, sizeof(calMsg), 
            "[CALIBRATED] Base RMS:%d.%dV | Sag Limit:%d.%dV | Base Noise:%ld\r\n", 
            rms_i, rms_d, sag_i, sag_d, baseNoise);
            
          transmitRS485(calMsg);
          isCalibrated = true;
        }
      }
    }
    return; 
  }

  if (millis() - lastHeartbeatMs >= HEARTBEAT_MS) {
    char body[96];
    snprintf(body, sizeof(body), "NODE=N1,TIME=%lu,TYPE=HEARTBEAT,CONF=1.00,DSID=0|0|0|0|0", millis());
    uint8_t crc = xorChecksum(body);
    char full[120];
    snprintf(full, sizeof(full), "%s*%02X\r\n", body, crc);
    transmitRS485(full);
    lastHeartbeatMs = millis();
  }

  if (SerialUno.available()) {
    char buf[64];
    int len = SerialUno.readBytesUntil('\n', buf, sizeof(buf) - 1);
    buf[len] = '\0';
    Features f;
    if (parsePacket(buf, &f)) {
      processEvent(f);
    }
  }
}