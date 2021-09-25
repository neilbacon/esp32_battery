/*
 * This BLE server:
 * - waits for client connection, enters deep sleep if it doesn't happen by TIME_CONNECT
 * - notifies motion updates, enters deep sleep after TIME_NOTIFY since last update
 * After a deep sleep it restarts from scratch and the client must reconnect.
 * The idea is to make a battery powered device.
 * 
 * You can use nRF Connect on a phone as a test client. 
 * 
 * With an unpaired client, the sequence is:
 * 1. setup() completes
 * 2. loop() called repeatedly 
 * 3. when the client connects: MyServerCallbacks::onConnect is called
 * 4. when the client says yes, pair with the device: MySecurity::onPassKeyNotify prints the random PIN
 * 5. when the client enters this PIN: MySecurity::onAuthenticationComplete is called
 * 6. the client can then poll or elect to recieve notifications
 * 
 * With a previously paired client, the sequence is the same except without step 4.
 * 
 * What will happen with multiple simultaneous clients?
 * 
 * esphome BLE client can't use BLE secirity (PIN) and requires server mac_address to be configured,
 * which is all rather limiting.
 * https://esphome.io/components/sensor/ble_client.html
 * If I do my own code in Arduino IDE, how can I get the data into Home Assistant? 
 * Using MQ?
 * Could I use the esphome API?
 * Custom Sensor? https://esphome.io/components/sensor/custom.html
 * 
 * Can BLE servers form a mesh? Yes, but its a whole lot of new standards & protocols:
 * https://en.wikipedia.org/wiki/Bluetooth_mesh_networking
 * I think its supported by Expressif's ESP-IDE, but not in Arduino IDE.
 */

#include <stdio.h>
#include <stdarg.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <BLEDescriptor.h>
#include <BLE2902.h>



// ************ Roll my own logging framework ********************

/* 
 * What are the alternatives? 
 * 1. Expressif's logging library: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/log.html
 *    Requires setting compile time flags, not possible? in Arduino IDE.
 * 2. a layer over #1: https://thingpulse.com/esp32-logging/ appears to have the same issue.
 * 3. https://github.com/thijse/Arduino-Log/ (in Arduino IDE Library Manager). All log messages are compiled normally.
 *    Need to edit it's .h file to exclude all logging from compilation (it's all or nothing).
 * 
 * It looks like serious developers move to other IDE's (e.g. ESP IDE or PlatformIO)
 * or build environments (e.g. Make) which 1 & 2 are designed for.
 * 
 * Usage:
 * 1. set LOG_LEVEL to suit
 * 2. log_i("loop", "my str val = %s, my int val = %d \n", "MyStringVal", 11); // e.g. tag = class or function name
 *    prints: "INF loop: my str val = MyStringVal, my int val = 11 \n"
 */ 
int log_base(const char *level, const char *tag, const char *format, ...) {
  char buf[1024];
  int n = snprintf(buf, sizeof(buf), "%s %s: ", level, tag);
  va_list args;
  va_start(args, format);
  int m = vsnprintf(buf+n, sizeof(buf) - n, format, args);
  va_end(args);
  Serial.print(buf);
  return n + m;
}

#define LOG_NONE  (0)
#define LOG_ERROR (1)
#define LOG_INFO  (2)
#define LOG_DEBUG (3)

#define LOG_LEVEL LOG_DEBUG

#if LOG_LEVEL >= LOG_DEBUG
#define log_d(tag, format, ...) log_base("DBG", tag, format, ##__VA_ARGS__)
#else
#define log_d(tag, format, ...)
#endif

#if LOG_LEVEL >= LOG_INFO
#define log_i(tag, format, ...) log_base("INF", tag, format, ##__VA_ARGS__)
#else
#define log_i(tag, format, ...)
#endif

#if LOG_LEVEL >= LOG_ERROR
#define log_e(tag, format, ...) log_base("ERR", tag, format, ##__VA_ARGS__)
#else
#define log_e(tag, format, ...)
#endif

#include <rom/rtc.h> // rtc_get_reset_reason()

const char* reset_reason() {
  const char *p;
  switch (rtc_get_reset_reason(0)) {
    case 1 : p = "POWERON_RESET"; break;          /**<1,  Vbat power on reset*/
    case 3 : p = "SW_RESET"; break;               /**<3,  Software reset digital core*/
    case 4 : p = "OWDT_RESET"; break;             /**<4,  Legacy watch dog reset digital core*/
    case 5 : p = "DEEPSLEEP_RESET"; break;        /**<5,  Deep Sleep reset digital core*/
    case 6 : p = "SDIO_RESET"; break;             /**<6,  Reset by SLC module, reset digital core*/
    case 7 : p = "TG0WDT_SYS_RESET"; break;       /**<7,  Timer Group0 Watch dog reset digital core*/
    case 8 : p = "TG1WDT_SYS_RESET"; break;       /**<8,  Timer Group1 Watch dog reset digital core*/
    case 9 : p = "RTCWDT_SYS_RESET"; break;       /**<9,  RTC Watch dog Reset digital core*/
    case 10 : p = "INTRUSION_RESET"; break;       /**<10, Instrusion tested to reset CPU*/
    case 11 : p = "TGWDT_CPU_RESET"; break;       /**<11, Time Group reset CPU*/
    case 12 : p = "SW_CPU_RESET"; break;          /**<12, Software reset CPU*/
    case 13 : p = "RTCWDT_CPU_RESET"; break;      /**<13, RTC Watch dog Reset CPU*/
    case 14 : p = "EXT_CPU_RESET"; break;         /**<14, for APP CPU, reseted by PRO CPU*/
    case 15 : p = "RTCWDT_BROWN_OUT_RESET"; break;/**<15, Reset when the vdd voltage is not stable*/
    case 16 : p = "RTCWDT_RTC_RESET"; break;      /**<16, RTC Watch dog reset digital core and rtc module*/
    default : p = "NO_MEAN"; break;
  }
  return p;
}


// ************ Configure Bluetooth Security parameters ********************

// I think ESP_BLE_OOB_ENABLE would allow authentication by touch pad or switch.
// #include <esp_gap_ble_api.h> // exists, but missing:
// enum value: ESP_BLE_SM_OOB_SUPPORT
// #define ESP_BLE_OOB_DISABLE 0
// #define ESP_BLE_OOB_ENABLE  1

typedef struct SecParam { 
  esp_ble_sm_param_t key; 
  uint8_t val; 
} SecParam;

// Adapted from: https://github.com/nkolban/esp32-snippets/issues/793
void setSecurityParams() {
  SecParam xs[] = {
    // { XESP_BLE_SM_SET_STATIC_PASSKEY, 123456 }, // comment out for random PIN
    { ESP_BLE_SM_AUTHEN_REQ_MODE, ESP_LE_AUTH_REQ_SC_MITM_BOND }, // bonding with peer device after authentication
    { ESP_BLE_SM_IOCAP_MODE, ESP_IO_CAP_OUT }, // set the IO capability to No output No input (no screen/keyboard)
    { ESP_BLE_SM_MAX_KEY_SIZE, 16 }, // key size should be 7~16 bytes
    { ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_DISABLE },
    // { ESP_BLE_SM_OOB_SUPPORT, ESP_BLE_OOB_DISABLE },
    
    /* If your BLE device act as a Slave, the init_key means you hope which types of key of the master should distribut to you,
    and the response key means which key you can distribut to the Master;
    If your BLE device act as a master, the response key means you hope which types of key of the slave should distribut to you,
    and the init key means which key you can distribut to the slave.
    */
    { ESP_BLE_SM_SET_INIT_KEY, ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK },
    { ESP_BLE_SM_SET_RSP_KEY, ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK },
  };
  for (SecParam& x : xs) {
    esp_ble_gap_set_security_param(x.key, &x.val, sizeof(x.val));
  }
}



// ************ State & reset time to deep sleep ********************

// In deep sleep only parts of the chip still on are:
// RTC controller, RTC peripherals and RTC memory defined as: RTC_DATA_ATTR uint32_t value = 0;
// On wake from deep sleep the propgram starts from scratch (only RTC_DATA_ATTR data is preserved) and runs startup() then loop().
#define TIME_CONNECT    60000UL     // pairing & connection timeout (milli secs), client has this long to connect before deep sleep
#define TIME_NOTIFY     10000UL     // notify period (milli secs), after connect client has this long to read values before deep sleep 


unsigned long bedTime = 0; // time at which we will enter deep sleep
int state = 0;

void resetConnectTimer() {
  log_d("resetConnectTimer", "\n");
  state = 1;
  bedTime = millis() + TIME_CONNECT;
}

void resetNotifyTimer() {
  log_d("resetNotifyTimer", "\n");
  state = 2;
  bedTime = millis() + TIME_NOTIFY;
}



// ************ Code and data used in Bluetooth callbacks *************

void advertise() {
  BLEDevice::startAdvertising();
  resetConnectTimer();
}



// ************ Bluetooth Pairing & Connection ********************

class MySecurity : public BLESecurityCallbacks {

  // when client says yes pair with us, client must then enter this pass_key 
  void onPassKeyNotify(uint32_t pass_key) {
    log_d("MySecurity", "onPassKeyNotify: pass_key = %u \n", pass_key);
    resetConnectTimer();
  }

  // when client has authenticated, is paired
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
    log_d("MySecurity", "onAuthenticationComplete \n");
    resetNotifyTimer();
  }

  // following are not called in this program (maybe they are for clients?)

  uint32_t onPassKeyRequest() {
    log_d("MySecurity", "onPassKeyRequest \n");
    return 123456;
  }
  
  bool onConfirmPIN(uint32_t pass_key) {
    log_d("MySecurity", "onConfirmPIN: pass_key = %u \n", pass_key);
    return true;
  }
  
  bool onSecurityRequest() {
    log_d("MySecurity", "onSecurityRequest \n");
    return true;
  }

};



// ************ Bluetooth Clients connecting to us (the server)  ********************

int clientCount = 0;

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    ++clientCount;
    log_d("MyServerCallbacks", "onConnect: clientCount = %d \n", clientCount);
    // advertise(); // example code advertises here, but I don't think that is correct
    resetConnectTimer();
  };

  void onDisconnect(BLEServer* pServer) {
    --clientCount;
    log_d("MyServerCallbacks", "onDisconnect: clientCount = %d \n", clientCount);
    if (clientCount < 1) advertise(); // only supposed to advertise when no clients connected?
  }
};



// ************ UUIDs for our Bluetooth LE Service ********************

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DESCRIPTOR_UUID     "9df14c09-530b-4531-9585-0c61d3dd4637"



BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;

#define PIN_PIR              GPIO_NUM_39
#define PIN_LED_AWAKE        GPIO_NUM_26
#define PIN_LED_STAY_AWAKE   GPIO_NUM_25
#define PIN_TOUCH_STAY_AWAKE GPIO_NUM_32



// ************ Interrupt Service Routines ********************

#define THRESHOLD 40
volatile bool touchStayAwake = false;

// gets called many times with v < THRESHOLD for one touch, never called with v > THRESHOLD
// so reset touchStayAwake = false in loop().
void touchStayAwakeISR() {
  touchStayAwake = true;
  // int v = touchRead(PIN_TOUCH_STAY_AWAKE);
  // log_d("toggleStayAwake", "touchStayAwake = %s, v = %d \n", touchStayAwake ? "true" : "false", v);
}



volatile bool pirMotion = false;

// gets called many times for HIGH then once for LOW
void pirChangeISR() {
  pirMotion = digitalRead(PIN_PIR) == HIGH;
}



void setup() {
  Serial.begin(115200);
  log_i("setup", "reset reason = %s \n", reset_reason());
  
  // Create BLE hierarchy: Device > Service > Characteristic > Descriptors
  BLEDevice::init("ESP32");
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(new MySecurity());
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_WRITE  |
                      BLECharacteristic::PROPERTY_NOTIFY |
                      BLECharacteristic::PROPERTY_INDICATE
                    );

  // https://www.bluetooth.com/specifications/gatt/viewer?attributeXmlFile=org.bluetooth.descriptor.gatt.client_characteristic_configuration.xml
  pCharacteristic->addDescriptor(new BLE2902()); // provides standard GATT interactions with client
  
  { // Arduino BLE version 1.2.1 is current in Ardiono IDE, multi-line code to set value
    // 24 Sep 2021 master: value can be set in ctor
    BLEDescriptor* d = new BLEDescriptor(DESCRIPTOR_UUID);
    d->setValue("PIR motion");
    pCharacteristic->addDescriptor(d);
  }
  
  // Start the service
  pService->start();

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true); // what does this do?
  pAdvertising->setMinPreferred(0x06); // "helps with iPhone connections issue" what does this do?
  pAdvertising->setMaxPreferred(0x12);
  advertise();

  setSecurityParams();
  
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_LED_AWAKE, OUTPUT);
  pinMode(PIN_LED_STAY_AWAKE, OUTPUT);
  // pinMode not used for touch inputs

  digitalWrite(PIN_LED_AWAKE, HIGH);
  touchAttachInterrupt(PIN_TOUCH_STAY_AWAKE, touchStayAwakeISR, THRESHOLD);
  pirMotion = digitalRead(PIN_PIR) == HIGH;
  attachInterrupt(digitalPinToInterrupt(PIN_PIR), pirChangeISR, CHANGE);
  
  esp_sleep_enable_ext0_wakeup(PIN_PIR, 1); // 1 = High, 0 = Low
  log_i("setup", "end \n");
}

bool stayAwake = false;

bool firstTime = true;
bool prevPirMotion = false;

void loop() {
  log_i("loop", "start \n");
  if (firstTime || pirMotion != prevPirMotion) {
    log_i("loop", "pirMotion = %s \n", pirMotion ? "true" : "false");
    uint32_t value = pirMotion ? 0xFFFFFFFF : 0x00000000;
    pCharacteristic->setValue(value); // transmitted little endian (LSB 1st)
    pCharacteristic->notify();
    if (state == 2) resetNotifyTimer();
    prevPirMotion = pirMotion;
  }
  if (firstTime) {
    touchStayAwake = firstTime = false;    
  }
  if (touchStayAwake) {
    stayAwake = !stayAwake;
    log_i("loop", "stayAwake = %s \n", stayAwake ? "true" : "false");
    digitalWrite(PIN_LED_STAY_AWAKE, stayAwake ? HIGH : LOW);
    touchStayAwake = false;
  }
  if (!stayAwake && millis() > bedTime) {
    log_i("loop", "deep sleep");
    Serial.flush();
    esp_deep_sleep_start();
  }
  delay(300);
  log_i("loop", "end \n");
}
