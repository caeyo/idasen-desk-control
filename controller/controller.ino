#include <BLEAddress.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <BLEUtils.h>
#include <Preferences.h>

#define MAC_ADDRESS           "de:26:5b:17:6f:24"
#define CONTROL_SERVICE_UUID  "99fa0001-338a-1024-8a49-009c0215f78a"
#define CONTROL_CHAR_UUID     "99fa0002-338a-1024-8a49-009c0215f78a"
#define OUTPUT_SERVICE_UUID   "99fa0020-338a-1024-8a49-009c0215f78a"
#define OUTPUT_CHAR_UUID      "99fa0021-338a-1024-8a49-009c0215f78a"
#define INPUT_SERVICE_UUID    "99fa0030-338a-1024-8a49-009c0215f78a"
#define INPUT_CHAR_UUID       "99fa0031-338a-1024-8a49-009c0215f78a"

#define SAVE  18 // SW3
#define STAND 19 // SW2
#define SIT   21 // SW1
#define DELAY 100

BLERemoteCharacteristic *controlChar = nullptr;
BLERemoteCharacteristic *outputChar = nullptr;
BLERemoteCharacteristic *inputChar = nullptr;

bool isConnected = false;
bool isBonded = false;

// Note that this is stored as little endian
uint32_t currHeight = 0;

Preferences preferences;
uint32_t sitPreset = 0;
uint32_t standPreset = 0;

static void heightAdjustCallback(BLERemoteCharacteristic *characteristic, uint8_t *pData, size_t length, bool isNotify);

class DeskSecurity : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() { return 123456; }
  void onPassKeyNotify(uint32_t pass_key) { }
  bool onConfirmPIN(uint32_t pass_key) { return true; }
  bool onSecurityRequest() { return true; }
  
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl){
    if (cmpl.success) {
      Serial.println("Auth complete");
      isBonded = true;
    } else {
      Serial.printf("Auth failed. Error Code: %d\n", cmpl.fail_reason);
    }
  }
};



void setup() {
  Serial.begin(115200);

  pinMode(SAVE, INPUT_PULLUP);
  pinMode(STAND, INPUT_PULLUP);
  pinMode(SIT, INPUT_PULLUP);

  preferences.begin("desk-config", false);
  sitPreset = preferences.getUInt("sit", 0);
  standPreset = preferences.getUInt("stand", 0);
  Serial.printf("Loaded presets - Sit: %04X, Stand: %04X\n", sitPreset, standPreset);

  BLEDevice::init("Idasen Controller");
  
  BLEDevice::setSecurityCallbacks(new DeskSecurity()); 
  BLESecurity *security = new BLESecurity();
  security->setAuthenticationMode(ESP_LE_AUTH_BOND); 
  security->setCapability(ESP_IO_CAP_NONE);
  security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  BLEClient *client = BLEDevice::createClient();

  BLEAdvertisedDevice* myDesk = nullptr;
  Serial.println("Scanning...");
  while (!myDesk) {
    BLEScanResults *foundDevices = pBLEScan->start(3, false);
    for (int i = 0; i < foundDevices->getCount(); i++) {
      BLEAdvertisedDevice device = foundDevices->getDevice(i);
      if (device.getAddress().toString().equals(MAC_ADDRESS)) {
        Serial.println("Found Desk!");
        myDesk = new BLEAdvertisedDevice(device);
        pBLEScan->stop();
        break;
      }
    }
    pBLEScan->clearResults();
  }

  if (client->connect(myDesk)) {
    Serial.println("Connected to desk");
    isConnected = true;
  } else {
    Serial.println("Connection failed");
    while(1);
  }

  BLERemoteService *dataService = client->getService(OUTPUT_SERVICE_UUID);
  if (dataService)
    outputChar = dataService->getCharacteristic(OUTPUT_CHAR_UUID);
  
  BLERemoteService *controlService = client->getService(CONTROL_SERVICE_UUID);
  if (controlService)
    controlChar = controlService->getCharacteristic(CONTROL_CHAR_UUID);

  BLERemoteService *inputService = client->getService(INPUT_SERVICE_UUID);
  if (inputService)
    inputChar = inputService->getCharacteristic(INPUT_CHAR_UUID);
}



static void heightAdjustCallback(BLERemoteCharacteristic *characteristic, uint8_t *pData, size_t length, bool isNotify) {
  currHeight = (pData[0] << 8) | pData[1];
}

bool buttonPressed(uint8_t buttonId, bool *state, uint32_t *lastTiming) {
  bool retVal = false;
  bool readVal = digitalRead(buttonId);
  if (millis() - *lastTiming >= DELAY) {
    retVal = *state && !readVal;
    if (retVal)
      *lastTiming = millis();
    *state = readVal;
  }
  return retVal;
}

void writeHeight(uint32_t height) {
  uint8_t data[2];
  data[0] = height >> 8;
  data[1] = height;
  inputChar->writeValue(data, 2);
}

void stop() {
  uint8_t data[] = { 0xFE, 0x00 };
  controlChar->writeValue(data, 2);
  data[0] = 0xFF;
  controlChar->writeValue(data, 2);
}

void loop() {
  static bool saveState = HIGH, standState = HIGH, sitState = HIGH;
  static uint32_t lastSave = 0, lastStand = 0, lastSit = 0;
  static bool callbacksRegistered = false;
  static bool shouldSave = false, moving = false;
  static uint32_t target = 0;

  if (isConnected && isBonded && !callbacksRegistered) {
    Serial.println("Bonding verified. Registering callbacks now...");
    outputChar->registerForNotify(heightAdjustCallback);
    callbacksRegistered = true;
    Serial.println("Callbacks registered. Begin main loop");
  }
  if (!callbacksRegistered) {
    Serial.println("Waiting to register callbacks...");
    delay(500);
    return;
  }


  if (buttonPressed(SAVE, &saveState, &lastSave)) {
    shouldSave = true;
  }

  if (buttonPressed(SIT, &sitState, &lastSit)) {
    if (shouldSave) {
      sitPreset = currHeight;
      preferences.putUInt("sit", sitPreset);
      shouldSave = false;
      Serial.printf("Saved sit preset: %04X\n", sitPreset);
    } else {
      Serial.printf("Beginning move to sit preset: %04X\n", sitPreset);
      if (moving) {
        stop();
        delay(750);
      }
      moving = true;
      target = sitPreset;
    }
  }

  if (buttonPressed(STAND, &standState, &lastStand)) {
    if (shouldSave) {
      standPreset = currHeight;
      preferences.putUInt("stand", standPreset);
      shouldSave = false;
      Serial.printf("Saved stand preset: %04X\n", standPreset);
    } else {
      Serial.printf("Beginning move to stand preset: %04X\n", standPreset);
      if (moving) {
        stop();
        delay(750);
      }
      moving = true;
      target = standPreset;
    }
  }

  if (moving) {
    if (currHeight != target)
      writeHeight(target);
    else {
      moving = false;
    }
    delay(100);
  }
}
