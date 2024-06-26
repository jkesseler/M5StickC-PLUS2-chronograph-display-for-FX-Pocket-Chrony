#include <BLEDevice.h>
#include <OneButton.h>
#include <EEPROM.h>
#include <M5StickCPlus2.h>
#include <OpenFontRender.h>
#include <esp_sleep.h>
#include "gun.h"
#include "pellet.h"
#include "ttfbin.h"
/*
 * For Minimum, Average, Maximum change MIN_AVE_MAX from 0 to 1
 * For ES/SD leave MIN_AVE_MAX at 0
 */
#define MIN_AVE_MAX 0

OpenFontRender render;

#define SHOT_STRING_LENGTH 10
// [0] = shot string length
// [1] = pellet_idx
// SHOT_STRING_LENGTH * shots
#define EEPROM_PER_GUN (SHOT_STRING_LENGTH * sizeof(float) + 2)
#define EEPROM_SIZE (6 + EEPROM_PER_GUN * NUM_GUNS)

#define PIN_INPUT 37
OneButton button(PIN_INPUT, true);

#define UNITS_IMPERIAL 0
#define UNITS_METRIC 1
#define UNITS_MAX 1

#define DISPLAY_FLIP_OFF 0
#define DISPLAY_FLIP_ON 1
#define DISPLAY_FLIP_MAX 1

#define SENSITIVITY_MAX 100

#define STATE_IDLE 0
#define STATE_CONNECTING 1
#define STATE_CONNECTED 2
#define STATE_OFFLINE 3

#define seconds() (millis() / 1000)

#define MAX_SHOT_HISTORY 4

static uint8_t state = STATE_IDLE;

static float chronyVBattery;
static unsigned long chronyVBattLastRead = 0;

// The remote service we wish to connect to.
static BLEUUID deviceUUID("0000180a-0000-1000-8000-00805f9b34fb");

static BLEUUID serviceUUID("00001623-88EC-688C-644B-3FA706C0BB75");

// The characteristic of the remote service we are interested in.
static const char* charUUIDs[] = {
  "00001624-88EC-688C-644B-3FA706C0BB75",
  "00001625-88EC-688C-644B-3FA706C0BB75",
  "00001626-88EC-688C-644B-3FA706C0BB75",
  "00001627-88EC-688C-644B-3FA706C0BB75",
  "00001628-88EC-688C-644B-3FA706C0BB75",
  "00001629-88EC-688C-644B-3FA706C0BB75",
  "0000162A-88EC-688C-644B-3FA706C0BB75"
};

static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;
static BLEClient* pClient;
static BLERemoteService* pRemoteService;

static bool renderMenu = false;
static bool dirty = false;
static bool profile_changed = false;
static bool power_saving = false;
static unsigned long display_on_at = 0;
static uint8_t searching_ctr = 0;

static uint8_t sensitivity = 50;
static uint8_t units = UNITS_IMPERIAL;

static uint8_t display_flip = 0;
static uint8_t power_save_duration = 0;
static uint8_t gun_index = 0;

#define ROLL_MOVING_AVE_LENGTH 32

static float roll_moving_average[ROLL_MOVING_AVE_LENGTH];
static float roll_moving_average_total = 0;
static int roll_moving_average_index = 0;

static bool notified_new_shot = false;
static uint16_t notified_speed;
static uint8_t notified_return;

typedef void (*menuItemCallback_t)(uint8_t);
typedef void (*menuItemGenString_t)(uint8_t, char*);

typedef struct shot_stats {
  float min;
  float max;
  float avg;
  float es;
  float sd;
} shot_stats_t;

typedef struct menuItem {
  const char* menuString;
  menuItemGenString_t menuItemGenString;
  struct menuItem* nextMenuItem;
  struct menuItem* currentSubMenu;
  struct menuItem* subMenu;
  menuItemCallback_t menuItemCallback;
  uint8_t param;
  menuItemGenString_t menuItemGenCurSelString;
} menuItem_t;

static menuItem_t* menu_gun = NULL;
static menuItem_t* menu_pellet = NULL;
static menuItem_t* menuStack[4];
static int menuStackIndex;
static menuItem_t* pCurrentMenuItem;

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
  }

  void onDisconnect(BLEClient* pclient) {
    /* this make me sad, can't get the code to reliably reconnect
        more than 4-5 times so here it is ...
        (On the plus side it seems to work well and I can get
        on with shot strings :) )*/
    ESP.restart();
    while (1) {}
  }
};

void setup() {
  auto cfg = M5.config();
  StickCP2.begin(cfg);

  Serial.printf("Starting Open Display (M5 Stick Plus) V1.1... %d EEPROM bytes\n", EEPROM_SIZE);

  button.attachDoubleClick(doubleClick);
  button.attachLongPressStop(longPressStop);
  button.attachClick(singleClick);

  /*
     * Read the Settings, if any setting is out of range
     * give it a sensible default value (cope with first use)
     */

  EEPROM.begin(EEPROM_SIZE);
  sensitivity = EEPROM.read(0);
  if (sensitivity > SENSITIVITY_MAX) {
    sensitivity = 30;
    EEPROM.write(0, sensitivity);
  }
  units = EEPROM.read(1);
  if (units > UNITS_MAX) {
    units = UNITS_MAX;
    EEPROM.write(1, units);
  }
  gun_index = EEPROM.read(2);
  if (gun_index >= NUM_GUNS) {
    gun_index = 0;
    EEPROM.write(2, gun_index);
  }
  display_flip = EEPROM.read(3);
  if (display_flip > DISPLAY_FLIP_MAX) {
    display_flip = 0;
    EEPROM.write(3, display_flip);
  }
  power_save_duration = EEPROM.read(4);
  if (power_save_duration > 60) {
    power_save_duration = 60;
    EEPROM.write(4, power_save_duration);
  }
  EEPROM.commit();

  BLEDevice::init("");

  build_gun_menu();

  // tft.init();
  StickCP2.Display.setRotation(display_flip ? 1 : 3);

  render.setSerial(Serial);      // Need to print render library message
  render.showFreeTypeVersion();  // print FreeType version
  render.showCredit();           // print FTL credit

  if (render.loadFont(ttfbin, sizeof(ttfbin))) {
    Serial.println("Render initialize error");
    return;
  }

  render.setDrawer(StickCP2.Display);  // Set drawer object

  StickCP2.Display.fillScreen(TFT_BLACK);

  dirty = true;
  Serial.println("Startup Complete");
}

uint8_t get_string_length(uint8_t gidx) {
  return EEPROM.read(6 + (gidx * EEPROM_PER_GUN));
}

void clear_string(uint8_t gidx) {
  EEPROM.write(6 + (gidx * EEPROM_PER_GUN), 0);
  EEPROM.commit();
}

float get_shot(uint8_t gidx, uint8_t sidx) {
  float result;
  EEPROM.get(6 + (gidx * EEPROM_PER_GUN) + sidx * sizeof(float) + 2, result);
  return result;
}

void add_shot(uint8_t gidx, float speed) {
  uint8_t sidx = get_string_length(gidx);

  if (sidx >= my_guns[gun_index].shot_string_length) {
    clear_string(gidx);
    sidx = 0;
  }
  EEPROM.put(6 + (gidx * EEPROM_PER_GUN) + sidx * sizeof(float) + 2, speed);
  EEPROM.write(6 + (gidx * EEPROM_PER_GUN), sidx + 1);
  EEPROM.commit();
}

uint8_t get_pellet_index(uint8_t gidx) {
  return EEPROM.read(6 + (gidx * EEPROM_PER_GUN) + 1);
}

void set_pellet_index(uint8_t gidx, uint8_t pidx) {
  EEPROM.write(6 + (gidx * EEPROM_PER_GUN) + 1, pidx);
  EEPROM.commit();
  get_pellet_index(gidx);
}

void doRenderMenu() {
  char genHeader[256];
  StickCP2.Display.fillScreen(TFT_BLACK);

  const char* pHeadertxt;
  if (pCurrentMenuItem->menuItemGenString == NULL) {
    pHeadertxt = pCurrentMenuItem->menuString;
  } else {
    pHeadertxt = genHeader;
    pCurrentMenuItem->menuItemGenString(pCurrentMenuItem->currentSubMenu->param, genHeader);
  }

  render.setFontSize(30);
  render.cdrawString(pHeadertxt,
                     StickCP2.Display.width() / 2,
                     20,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);

  const char* psubHeadertxt = pCurrentMenuItem->currentSubMenu->menuString;
  if (psubHeadertxt == NULL) {
    psubHeadertxt = genHeader;
    pCurrentMenuItem->currentSubMenu->menuItemGenString(pCurrentMenuItem->currentSubMenu->param, genHeader);
  }

  render.setFontSize(20);
  render.cdrawString(psubHeadertxt,
                     StickCP2.Display.width() / 2,
                     60,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);

  const char* pcurSelHeadertxt;
  if (pCurrentMenuItem->currentSubMenu->menuItemGenCurSelString) {
    pcurSelHeadertxt = genHeader;
    pCurrentMenuItem->currentSubMenu->menuItemGenCurSelString(pCurrentMenuItem->currentSubMenu->param, genHeader);

    render.setFontSize(20);
    render.cdrawString(pcurSelHeadertxt,
                       StickCP2.Display.width() / 2,
                       90,
                       TFT_WHITE,
                       TFT_BLACK,
                       Layout::Horizontal);
  }
}

#define STR_SEARCHING "Searching"

void renderSearching() {
  char temp_str[16];
  int16_t w;
  StickCP2.Display.fillScreen(TFT_BLACK);

  strcpy(temp_str, STR_SEARCHING);
  for (uint8_t i = 0; i < searching_ctr; i++) {
    strcat(temp_str, ".");
  }
  render.setFontSize(36);
  render.cdrawString(temp_str,
                     StickCP2.Display.width() / 2,
                     (StickCP2.Display.height() / 2) - 18,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);

  renderDeviceVBatt();
  searching_ctr++;
  searching_ctr %= 4;
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  /**
     * Called for each advertising BLE server.
     */
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    // We have found a device, let us now see if it contains the service we are looking for.
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(deviceUUID)) {
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      state = STATE_CONNECTING;
    }  // Found our server
  }    // onResult
};     // MyAdvertisedDeviceCallbacks

void do_scan() {
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(1, false);
}

bool writeChar(BLERemoteService* pRemoteService, int idx, uint8_t value) {
  // Obtain a reference to the characteristic in the service of the remote BLE server.
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUIDs[idx]);
  if (pRemoteCharacteristic == nullptr) {
    return false;
  }

  // Read the value of the characteristic.
  if (pRemoteCharacteristic->canWrite()) {
    pRemoteCharacteristic->writeValue(&value, 1);
  }
  return true;
}

bool readChar(BLERemoteService* pRemoteService, int idx, uint8_t* value) {
  // Obtain a reference to the characteristic in the service of the remote BLE server.
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUIDs[idx]);
  if (pRemoteCharacteristic == nullptr) {
    return false;
  }

  // Read the value of the characteristic.
  if (pRemoteCharacteristic->canRead()) {
    std::string v = pRemoteCharacteristic->readValue();
    *value = v[0];
  }
  return true;
}

/*
 4.5V = 100%
 3.6V = 0%

  50-100% Green     > 4.05V
  25-50% Yellow
  0-25% Red       < 3.85V
 */
void renderChronyVBatt() {
  uint8_t vb;
  uint16_t font_color = TFT_YELLOW;
  char temp_str[16];
  if (chronyVBattery >= 4.05) {
    font_color = TFT_GREEN;
  }
  if (chronyVBattery < 3.85) {
    font_color = TFT_RED;
  }
  sprintf(temp_str, "C %.1fV", chronyVBattery);
  render.setFontSize(15);
  render.rdrawString(temp_str,
                     StickCP2.Display.width(),
                     0,
                     font_color,
                     TFT_BLACK,
                     Layout::Horizontal);
}

void renderDeviceVBatt() {
  uint16_t font_color = TFT_YELLOW;
  char temp_str[16];
  float fbat = StickCP2.Power.getBatteryVoltage();
  fbat = fbat / 1000;

  if (fbat >= 4) {
    font_color = TFT_GREEN;
  }
  if (fbat < 3.8) {
    font_color = TFT_RED;
  }

  sprintf(temp_str, "D %.2fV", fbat);
  render.setFontSize(15);
  render.drawString(temp_str,
                    0,
                    0,
                    font_color,
                    TFT_BLACK,
                    Layout::Horizontal);
}

bool readBattery() {
  uint8_t vb;
  if (!readChar(pRemoteService, 3, &vb)) {
    return false;
  }
  chronyVBattery = (vb * 20.0) / 1000;
  return true;
}

static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {
  // Third byte is return in steps of 5%, anything better than 10% we display
  notified_return = ((char*)pData)[2] * 5;

  if ((notified_return >= sensitivity) && !renderMenu) {
    display_on_at = seconds();
    power_saving = false;

    renderMenu = false;

    notified_speed = ((char*)pData)[0];
    notified_speed <<= 8;
    notified_speed |= ((char*)pData)[1];

    notified_new_shot = true;
  }
}

uint8_t profile_bytes[2][5] = { { 0x32, 0x32, 0x32, 0x32, 0x64 }, { 0x17, 0x1E, 0x2D, 0x43, 0x5A } };

void connectToChrony() {
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  // Connect to the remove BLE Server.
  pClient->connect(myDevice);

  // Obtain a reference to the service we are after in the remote BLE server.
  pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }

  if (!writeChar(pRemoteService, 2, profile_bytes[0][my_guns[gun_index].gun_profile])) {
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }
  if (!writeChar(pRemoteService, 4, profile_bytes[1][my_guns[gun_index].gun_profile])) {
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }

  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUIDs[0]);
  if (pRemoteCharacteristic == nullptr) {
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }

  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
  } else {
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }

  readBattery();

  state = STATE_CONNECTED;
  StickCP2.Display.fillScreen(TFT_BLACK);
  power_saving = true;
}

// pitch = 180 * atan (accelerationX/sqrt(accelerationY*accelerationY + accelerationZ*accelerationZ))/M_PI;
// roll = 180 * atan (accelerationY/sqrt(accelerationX*accelerationX + accelerationZ*accelerationZ))/M_PI;
// yaw = 180 * atan (accelerationZ/sqrt(accelerationX*accelerationX + accelerationZ*accelerationZ))/M_PI;

void calculate_roll(bool override) {
  int i;
  float roll;
  auto imu_update = StickCP2.Imu.update();

  if (imu_update) {
    auto data = StickCP2.Imu.getImuData();

    roll = 180 * atan(data.accel.y / sqrt(data.accel.x * data.accel.x + data.accel.z * data.accel.z)) / M_PI;

    int old_average = (int)(roll_moving_average_total / ROLL_MOVING_AVE_LENGTH);
    if (old_average < -45) {
      old_average = -45;
    } else if (old_average > 45) {
      old_average = 45;
    }

    roll_moving_average_total -= roll_moving_average[roll_moving_average_index];
    roll_moving_average[roll_moving_average_index] = roll;
    roll_moving_average_total += roll;
    roll_moving_average_index += 1;
    roll_moving_average_index %= ROLL_MOVING_AVE_LENGTH;

    int new_average = (int)(roll_moving_average_total / ROLL_MOVING_AVE_LENGTH);
    if (new_average < -45) {
      new_average = -45;
    } else if (new_average > 45) {
      new_average = 45;
    }

    if ((old_average != new_average) || override) {
      uint16_t tri_colour = TFT_RED;
      if (new_average > -1 && new_average < 1) {
        tri_colour = TFT_GREEN;
      }
      StickCP2.Display.fillTriangle(0, 68 - old_average, 20, 60 - old_average, 20, 77 - old_average, TFT_BLACK);
      StickCP2.Display.fillTriangle(0, 68 - new_average, 20, 60 - new_average, 20, 77 - new_average, tri_colour);

      StickCP2.Display.fillTriangle(239, 68 + old_average, 220, 60 + old_average, 220, 77 + old_average, TFT_BLACK);
      StickCP2.Display.fillTriangle(239, 68 + new_average, 220, 60 + new_average, 220, 77 + new_average, tri_colour);
    }
  }
}

void handle_new_shot() {
  uint8_t pellet_index = get_pellet_index(gun_index);
  char sbuffer[256];
  shot_stats_t ss;
  uint8_t start_at, end_at, i;
  int idx;
  float energy;
  float fspeed = notified_speed;

  if (units == UNITS_IMPERIAL) {
    fspeed *= 0.0475111859;
    sprintf(sbuffer, "%d FPS", int(fspeed));
  } else {
    fspeed *= 0.014481409;
    sprintf(sbuffer, "%d M/S", int(fspeed));
  }

  StickCP2.Display.fillScreen(TFT_BLACK);
  render.setFontSize(40);
  render.cdrawString(sbuffer,
                     StickCP2.Display.width() / 2,
                     15,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);

  /* Draw the Pellet energy */
  if (units == UNITS_IMPERIAL) {
    energy = (my_pellets[pellet_index].pellet_weight_grains * powf(fspeed, 2)) / 450240;
    sprintf(sbuffer, "%.2f FPE", energy);
  } else {
    energy = 0.5 * (my_pellets[pellet_index].pellet_weight_grams / 1000) * powf(fspeed, 2);
    sprintf(sbuffer, "%.2f J", energy);
  }
  render.setFontSize(30);
  render.cdrawString(sbuffer,
                     StickCP2.Display.width() / 2,
                     57,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);

  add_shot(gun_index, fspeed);

  shotStringStats(&ss);

  render.setFontSize(17);
#if MIN_AVE_MAX
  sprintf(sbuffer, "Mn %d Av %d Mx %d", (int)ss.min, (int)ss.avg, (int)ss.max);
#else
  sprintf(sbuffer, "ES %.2f SD %.2f", ss.es, ss.sd);
#endif
  render.cdrawString(sbuffer,
                     StickCP2.Display.width() / 2,
                     94,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);

  if (get_string_length(gun_index) <= MAX_SHOT_HISTORY) {
    start_at = 0;
    end_at = get_string_length(gun_index);
  } else {
    start_at = get_string_length(gun_index) - MAX_SHOT_HISTORY;
    end_at = get_string_length(gun_index);
  }

  Serial.printf("Length %d Start %d End %d \n", get_string_length(gun_index), start_at, end_at);
  idx = 0;
  for (i = start_at; i < end_at; i++) {
    idx += sprintf(&sbuffer[idx], "%d, ", (int)get_shot(gun_index, i));
  }
  render.cdrawString(sbuffer,
                     StickCP2.Display.width() / 2,
                     117,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);

  render.setFontSize(15);
  /* Draw the return signal strength */
  sprintf(sbuffer, "R %d%%", notified_return);
  render.rdrawString(sbuffer,
                     3 * StickCP2.Display.width() / 4,
                     0,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);

  /* Draw the shot count */
  sprintf(sbuffer, "# %d/%d", get_string_length(gun_index), my_guns[gun_index].shot_string_length);
  render.drawString(sbuffer,
                    StickCP2.Display.width() / 4,
                    0,
                    TFT_WHITE,
                    TFT_BLACK,
                    Layout::Horizontal);

  renderDeviceVBatt();
  renderChronyVBatt();
  notified_new_shot = false;
}

void loop() {
  bool override_angle_change = false;
  unsigned long now = seconds();

  if (!power_saving && !renderMenu && state != STATE_IDLE && (power_save_duration != 0) && (display_on_at + power_save_duration) < now) {
    display_on_at = seconds();
    power_saving = true;
  }

  button.tick();

  if (dirty) {
    if (power_saving) {
      display_on_at = seconds();
      // u8g2.setPowerSave(0);
      power_saving = false;
    }
    if (renderMenu) {
      doRenderMenu();
    }
    dirty = false;
  }

  switch (state) {
    case STATE_IDLE:
      //      if(!dirty) {
      renderSearching();
      do_scan();
      //      }
      break;
    case STATE_OFFLINE:
      break;
    case STATE_CONNECTING:
      connectToChrony();
      break;
    case STATE_CONNECTED:
      if (now - chronyVBattLastRead > 5) {
        readBattery();
        chronyVBattLastRead = now;
      }

      if (notified_new_shot) {
        override_angle_change = true;
        handle_new_shot();
      }
      calculate_roll(override_angle_change);
      break;
  }
}

/*
  Menu system
*/
static void sleepCallback(uint8_t param) {
  // u8g2.setPowerSave(1);
  esp_deep_sleep_start();
}

static menuItem_t menu_sleep[] = {
  { "Zzzz", NULL, menu_sleep, NULL, NULL, sleepCallback, 0, NULL },
};

void menuItemGenStringCurSleep(uint8_t, char* buffer) {
  sprintf(buffer, "[%s]", menu_sleep[0].menuString);
}

static const uint8_t power_save_duration_lut[] = { 0, 2, 5, 10, 20, 40, 60 };

static void powerSaveCallback(uint8_t param) {
  if (power_save_duration != power_save_duration_lut[param]) {
    power_save_duration = power_save_duration_lut[param];
    EEPROM.write(4, power_save_duration);
    EEPROM.commit();
  }
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_power_save[] = {
  { "Off", NULL, &menu_power_save[1], NULL, NULL, powerSaveCallback, 0, NULL },
  { "2s", NULL, &menu_power_save[2], NULL, NULL, powerSaveCallback, 1, NULL },
  { "5s", NULL, &menu_power_save[3], NULL, NULL, powerSaveCallback, 2, NULL },
  { "10s", NULL, &menu_power_save[4], NULL, NULL, powerSaveCallback, 3, NULL },
  { "20s", NULL, &menu_power_save[5], NULL, NULL, powerSaveCallback, 4, NULL },
  { "40s", NULL, &menu_power_save[6], NULL, NULL, powerSaveCallback, 5, NULL },
  { "60s", NULL, &menu_power_save[0], NULL, NULL, powerSaveCallback, 6, NULL },
};

void menuItemGenStringCurPowerSaving(uint8_t, char* buffer) {
  uint8_t idx = 0;
  switch (power_save_duration) {
    case 2:
      idx = 1;
      break;
    case 5:
      idx = 2;
      break;
    case 10:
      idx = 3;
      break;
    case 20:
      idx = 4;
      break;
    case 40:
      idx = 5;
      break;
    case 60:
      idx = 6;
      break;
  }
  sprintf(buffer, "[%s]", menu_power_save[idx].menuString);
}

static void displayFlipCallback(uint8_t param) {
  display_flip = param;
  EEPROM.write(3, display_flip);
  EEPROM.commit();
  StickCP2.Display.setRotation(display_flip ? 1 : 3);
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_display_flip[] = {
  { "Off", NULL, &menu_display_flip[1], NULL, NULL, displayFlipCallback, DISPLAY_FLIP_OFF, NULL },
  { "On", NULL, &menu_display_flip[0], NULL, NULL, displayFlipCallback, DISPLAY_FLIP_ON, NULL }
};

void menuItemGenStringCurDisplayFlip(uint8_t, char* buffer) {
  sprintf(buffer, "[%s]", menu_display_flip[display_flip].menuString);
}

static void unitsCallback(uint8_t param) {
  units = param;
  EEPROM.write(1, units);
  EEPROM.commit();
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_units[] = {
  { "FPS", NULL, &menu_units[1], NULL, NULL, unitsCallback, UNITS_IMPERIAL, NULL },
  { "M/S", NULL, &menu_units[0], NULL, NULL, unitsCallback, UNITS_METRIC, NULL }
};

void menuItemGenStringCurSelUnits(uint8_t, char* buffer) {
  sprintf(buffer, "[%s]", menu_units[units].menuString);
}

static void sensitivityIncCallback(uint8_t param) {
  if (sensitivity <= 95) {
    sensitivity += 5;
    EEPROM.write(0, sensitivity);
    EEPROM.commit();
  }
}
static void sensitivityDecCallback(uint8_t param) {
  if (sensitivity >= 5) {
    sensitivity -= 5;
    EEPROM.write(0, sensitivity);
    EEPROM.commit();
  }
}

static menuItem_t menu_sensitivity[] = {
  { "Increase", NULL, &menu_sensitivity[1], NULL, NULL, sensitivityIncCallback, 0, NULL },
  { "Decrease", NULL, &menu_sensitivity[0], NULL, NULL, sensitivityDecCallback, 0, NULL }
};

void menuItemGenStringSensitivity(uint8_t, char* buffer) {
  sprintf(buffer, "%d %%", sensitivity);
}

void menuItemGenStringCurSelSensitivity(uint8_t, char* buffer) {
  sprintf(buffer, "[%d %%]", sensitivity);
}

static void selectPelletCallback(uint8_t param) {
  set_pellet_index(gun_index, param);
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

void menuItemGenStringPellet(uint8_t i, char* buffer) {
  if (units == UNITS_IMPERIAL) {
    sprintf(buffer, "%s %.3f %.3f", my_pellets[i].pellet_mfr, my_pellets[i].pellet_weight_grains, my_pellets[i].pellet_caliber_inch);
  } else {
    sprintf(buffer, "%s %.3f %.3f", my_pellets[i].pellet_mfr, my_pellets[i].pellet_weight_grams, my_pellets[i].pellet_caliber_mm);
  }
}

void menuItemGenStringCurSelPellet(uint8_t i, char* buffer) {
  uint8_t pellet_index = get_pellet_index(gun_index);
  sprintf(buffer, "[%s]", my_pellets[pellet_index].pellet_name);
}

static void selectGunCallback(uint8_t param) {
  gun_index = param;
  EEPROM.write(2, gun_index);
  EEPROM.commit();
  pCurrentMenuItem = menuStack[--menuStackIndex];
  build_pellet_menu();
  profile_changed = true;
}

void menuItemGenStringGun(uint8_t i, char* buffer) {
  if (units == UNITS_IMPERIAL) {
    sprintf(buffer, "%s %.3f", my_guns[i].gun_mfr, my_guns[i].gun_caliber_inch);
  } else {
    sprintf(buffer, "%s %.3f", my_guns[i].gun_mfr, my_guns[i].gun_caliber_mm);
  }
}

void menuItemGenStringCurSelGun(uint8_t i, char* buffer) {
  sprintf(buffer, "[%s]", my_guns[gun_index].gun_name);
}

void shotStringStats(shot_stats_t* ss) {
  float speed, dist, sum = 0, sum_of_dists_sqd = 0;
  uint8_t i, scnt = get_string_length(gun_index);
  if (scnt == 0) {
    ss->min = 0;
    ss->max = 0;
    ss->avg = 0;
    ss->es = 0;
    ss->sd = 0;
    return;
  }

  ss->min = 1000000;
  ss->max = 0;

  for (i = 0; i < scnt; i++) {
    speed = get_shot(gun_index, i);
    if (speed < ss->min) {
      ss->min = speed;
    }
    if (speed > ss->max) {
      ss->max = speed;
    }
    sum += speed;
  }

  ss->avg = sum / (float)scnt;
  ss->es = ss->max - ss->min;

  for (i = 0; i < scnt; i++) {
    speed = get_shot(gun_index, i);
    if (speed > ss->avg) {
      dist = (speed - ss->avg);
    } else {
      dist = (ss->avg - speed);
    }
    sum_of_dists_sqd += powf(dist, 2);
  }
  ss->sd = sqrtf(sum_of_dists_sqd / (float)scnt);
}

static void shotStringClearCallback(uint8_t param) {
  clear_string(gun_index);
}

static void shotStringDumpCallback(uint8_t param) {
  shot_stats_t stats;
  uint8_t pellet_index = get_pellet_index(gun_index);
  uint8_t i, scnt = get_string_length(gun_index);
  Serial.printf("Shot String\n");
  Serial.printf("Gun %s %s\n", my_guns[gun_index].gun_mfr, my_guns[gun_index].gun_name);
  if (units == UNITS_IMPERIAL) {
    Serial.printf("Ammo %s %.3f %.3f\n", my_pellets[pellet_index].pellet_name, my_pellets[pellet_index].pellet_caliber_inch, my_pellets[pellet_index].pellet_weight_grains);
  } else {
    Serial.printf("Ammo %s %.3f %.3f\n", my_pellets[pellet_index].pellet_name, my_pellets[pellet_index].pellet_caliber_mm, my_pellets[pellet_index].pellet_weight_grams);
  }

  shotStringStats(&stats);
  Serial.printf("Minimum : %.2f\n", stats.min);
  Serial.printf("Maximum : %.2f\n", stats.max);
  Serial.printf("Average : %.2f\n", stats.avg);
  Serial.printf("ES      : %.2f\n", stats.es);
  Serial.printf("SD      : %.2f\n", stats.sd);

  for (i = 0; i < scnt; i++) {
    Serial.printf("Shot %d: %.2f\n", i + 1, get_shot(gun_index, i));
  }
}

static void shotStringInitCallback(uint8_t param) {
  uint8_t i;
  for (i = 0; i < NUM_GUNS; i++) {
    clear_string(i);
  }
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static void shotStringStatsCallback(uint8_t param) {
  shot_stats_t stats;
  char sbuffer[128];
  shotStringStats(&stats);
  StickCP2.Display.fillScreen(TFT_BLACK);
  render.setFontSize(20);

  sprintf(sbuffer, "Min : %d\n", (int)stats.min);
  render.cdrawString(sbuffer,
                     StickCP2.Display.width() / 2,
                     5,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);

  sprintf(sbuffer, "Max : %d\n", (int)stats.max);
  render.cdrawString(sbuffer,
                     StickCP2.Display.width() / 2,
                     28,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);
  sprintf(sbuffer, "Ave : %d\n", (int)stats.avg);
  render.cdrawString(sbuffer,
                     StickCP2.Display.width() / 2,
                     51,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);
  sprintf(sbuffer, "ES  : %.2f\n", stats.es);
  render.cdrawString(sbuffer,
                     StickCP2.Display.width() / 2,
                     74,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);
  sprintf(sbuffer, "SD  : %.2f\n", stats.sd);
  render.cdrawString(sbuffer,
                     StickCP2.Display.width() / 2,
                     97,
                     TFT_WHITE,
                     TFT_BLACK,
                     Layout::Horizontal);
  sleep(5);
}

static uint8_t review_counter;

void menuItemGenStringShotStringReview(uint8_t, char* buffer) {
  if (get_string_length(gun_index) == 0) {
    sprintf(buffer, "0/0: ---");
  } else {
    sprintf(buffer, "%d/%d: %d", review_counter + 1, get_string_length(gun_index), (int)get_shot(gun_index, review_counter));
    review_counter++;
    if (review_counter >= get_string_length(gun_index)) {
      review_counter = 0;
    }
  }
}

void menuItemGenStringCurSelReview(uint8_t, char* buffer) {
  sprintf(buffer, "");
  review_counter = 0;
}

void shotStringReviewCallback(uint8_t) {
}

static menuItem_t menu_shot_string_review[] = {
  { "Next", NULL, &menu_shot_string_review[0], NULL, NULL, shotStringReviewCallback, 0, NULL },
};

static menuItem_t menu_shot_string[] = {
  { "Stats", NULL, &menu_shot_string[1], NULL, NULL, shotStringStatsCallback, 0, NULL },
  { "Review", menuItemGenStringShotStringReview, &menu_shot_string[2], menu_shot_string_review, menu_shot_string_review, NULL, 0, menuItemGenStringCurSelReview },
  { "Dump", NULL, &menu_shot_string[3], NULL, NULL, shotStringDumpCallback, 0, NULL },
  { "Clear", NULL, &menu_shot_string[4], NULL, NULL, shotStringClearCallback, 0, NULL },
  { "Initialise", NULL, &menu_shot_string[0], NULL, NULL, shotStringInitCallback, 0, NULL }
};

void menuItemGenStringCurShotString(uint8_t, char* buffer) {
  sprintf(buffer, "[%d]", get_string_length(gun_index));
}

static menuItem_t menu_top_level[] = {
  { "Gun", NULL, &menu_top_level[1], NULL, NULL, NULL, 0, menuItemGenStringCurSelGun },
  { "Pellet", NULL, &menu_top_level[2], NULL, NULL, NULL, 0, menuItemGenStringCurSelPellet },
  { "Shot String", NULL, &menu_top_level[3], menu_shot_string, menu_shot_string, NULL, 0, menuItemGenStringCurShotString },
  { "Min. Return", menuItemGenStringSensitivity, &menu_top_level[4], menu_sensitivity, menu_sensitivity, NULL, 0, menuItemGenStringCurSelSensitivity },
  { "Units", NULL, &menu_top_level[5], menu_units, menu_units, NULL, 0, menuItemGenStringCurSelUnits },
  { "Display Flip", NULL, &menu_top_level[6], menu_display_flip, menu_display_flip, NULL, 0, menuItemGenStringCurDisplayFlip },
  { "Power Save", NULL, &menu_top_level[7], menu_power_save, menu_power_save, NULL, 0, menuItemGenStringCurPowerSaving },
  { "Sleep", NULL, &menu_top_level[0], menu_sleep, menu_sleep, NULL, 0, menuItemGenStringCurSleep }

};

bool is_pellet_for_gun(uint8_t pidx) {
  bool result = false;

  if (units == UNITS_IMPERIAL) {
    result = ((my_pellets[pidx].pellet_caliber_inch != 0) && (my_guns[gun_index].gun_caliber_inch != 0)) && (my_pellets[pidx].pellet_caliber_inch == my_guns[gun_index].gun_caliber_inch);
  } else {
    result = ((my_pellets[pidx].pellet_caliber_mm != 0) && (my_guns[gun_index].gun_caliber_mm != 0)) && (my_pellets[pidx].pellet_caliber_mm == my_guns[gun_index].gun_caliber_mm);
  }
  return result;
}

uint8_t num_pellets_for_gun() {
  uint8_t i, pcnt = 0;
  for (i = 0; i < NUM_PELLETS; i++) {
    if (is_pellet_for_gun(i)) {
      pcnt += 1;
    }
  }
  return pcnt;
}

void build_pellet_menu() {
  bool found_pellet;
  uint8_t pellet_index, i, npellets, nctr = 0;
  if (menu_pellet) {
    free(menu_pellet);
  }
  npellets = num_pellets_for_gun();

  menu_pellet = (menuItem_t*)malloc(npellets * sizeof(menuItem_t));

  for (i = 0; i < NUM_PELLETS; i++) {
    if (is_pellet_for_gun(i)) {
      menu_pellet[nctr].menuString = my_pellets[i].pellet_name;
      menu_pellet[nctr].menuItemGenString = NULL;
      menu_pellet[nctr].nextMenuItem = ((nctr == npellets - 1) ? &menu_pellet[0] : &menu_pellet[nctr + 1]);
      menu_pellet[nctr].currentSubMenu = NULL;
      menu_pellet[nctr].subMenu = NULL;
      menu_pellet[nctr].menuItemCallback = selectPelletCallback;
      menu_pellet[nctr].param = i;
      menu_pellet[nctr].menuItemGenCurSelString = menuItemGenStringPellet;
      nctr += 1;
    }
  }

  menu_top_level[1].currentSubMenu = menu_pellet;
  menu_top_level[1].subMenu = menu_pellet;
  found_pellet = false;
  pellet_index = get_pellet_index(gun_index);

  for (i = 0; i < nctr; i++) {
    if (pellet_index == menu_pellet[i].param) {
      found_pellet = true;
      break;
    }
  }
  if (!found_pellet) {
    set_pellet_index(gun_index, menu_pellet[0].param);
  }
}

// Name, Manufacturer, caliber in inches, caliber in mm, profile (speed range)
void build_gun_menu() {
  uint8_t i;
  menu_gun = (menuItem_t*)malloc(NUM_GUNS * sizeof(menuItem_t));
  for (i = 0; i < NUM_GUNS; i++) {
    menu_gun[i].menuString = my_guns[i].gun_name;
    menu_gun[i].menuItemGenString = NULL;
    menu_gun[i].nextMenuItem = ((i == NUM_GUNS - 1) ? &menu_gun[0] : &menu_gun[i + 1]);
    menu_gun[i].currentSubMenu = NULL;
    menu_gun[i].subMenu = NULL;
    menu_gun[i].menuItemCallback = selectGunCallback;
    menu_gun[i].param = i;
    menu_gun[i].menuItemGenCurSelString = menuItemGenStringGun;
  }
  menu_top_level[0].currentSubMenu = menu_gun;
  menu_top_level[0].subMenu = menu_gun;
  build_pellet_menu();
}

static menuItem_t menu_entry = { "Settings", NULL, menu_top_level, menu_top_level, NULL, NULL, 0, NULL };

void singleClick() {
  Serial.println("x1");
  if (renderMenu) {
    dirty = true;
    pCurrentMenuItem->currentSubMenu = pCurrentMenuItem->currentSubMenu->nextMenuItem;
  } else {
    if (power_saving) {
      display_on_at = seconds();
      // u8g2.setPowerSave(0);
      power_saving = false;
    }
  }
}

void doubleClick() {
  Serial.println("x2");
  if (renderMenu) {
    dirty = true;

    if (pCurrentMenuItem->currentSubMenu->menuItemCallback != NULL) {
      (*pCurrentMenuItem->currentSubMenu->menuItemCallback)(pCurrentMenuItem->currentSubMenu->param);
    } else {
      if (pCurrentMenuItem->currentSubMenu != NULL) {
        menuStack[menuStackIndex++] = pCurrentMenuItem;
        pCurrentMenuItem = pCurrentMenuItem->currentSubMenu;
      }
    }
  }
}

void longPressStop() {
  Serial.println("LPS");
  if (renderMenu) {
    if (menuStackIndex == 1) {
      renderMenu = false;
      StickCP2.Display.fillScreen(TFT_BLACK);
      if (profile_changed) {
        ESP.restart();
        while (1) {}
      }
      if (state == STATE_OFFLINE) {
        state = STATE_IDLE;
      }
    } else {
      dirty = true;
      pCurrentMenuItem = menuStack[--menuStackIndex];
    }
  } else if (state == STATE_CONNECTED) {
    profile_changed = false;
    dirty = true;
    renderMenu = true;
    pCurrentMenuItem = &menu_entry;
    menu_entry.currentSubMenu = menu_top_level;
    menuStackIndex = 0;
    menuStack[menuStackIndex++] = pCurrentMenuItem;
  } else if (state == STATE_IDLE) {
    state = STATE_OFFLINE;
    profile_changed = false;
    dirty = true;
    renderMenu = true;
    pCurrentMenuItem = &menu_entry;
    menu_entry.currentSubMenu = menu_top_level;
    menuStackIndex = 0;
    menuStack[menuStackIndex++] = pCurrentMenuItem;
  }
}
