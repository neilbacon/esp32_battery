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
 * 1. Figure out how lib code does it: https://github.com/nkolban/ESP32_BLE_Arduino/blob/master/src/BLEAdvertisedDevice.cpp
 * 2. Expressif's logging library: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/log.html
 *    Requires setting compile time flags, not possible? in Arduino IDE.
 * 3. a layer over #1: https://thingpulse.com/esp32-logging/ appears to have the same issue.
 * 4. https://github.com/thijse/Arduino-Log/ (in Arduino IDE Library Manager). All log messages are compiled normally.
 *    Need to edit it's .h file to exclude all logging from compilation (it's all or nothing).
 * 
 * Move to another IDE e.g. ESP IDE or PlatformIO or build environments e.g. Make which have less magic.
 * 
 * Usage:
 * 1. set LOG_LEVEL to suit
 * 2. log_i("loop", "my str val = %s, my int val = %d \n", "MyStringVal", 11); // e.g. tag = class or function name
 *    prints: "INF loop: my str val = MyStringVal, my int val = 11 \n"
 */ 

const unsigned long time0 = millis();

int log_base(const char *level, const char *tag, const char *format, ...) {
  char buf[1024];
  float secs = (millis() - time0) * 1.0e-3;
  int n = snprintf(buf, sizeof(buf), "%7.3f %s %s: ", secs, level, tag);
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

#include <rom/rtc.h> // rtc_get_reset_reason(cpu)
#define DEEPSLEEP_RESET 5

//const char* reset_reason(int x) {
//  const char *p;
//  switch (x) {
//    case 1 : p = "POWERON_RESET"; break;          /**<1,  Vbat power on reset*/
//    case 3 : p = "SW_RESET"; break;               /**<3,  Software reset digital core*/
//    case 4 : p = "OWDT_RESET"; break;             /**<4,  Legacy watch dog reset digital core*/
//    case 5 : p = "DEEPSLEEP_RESET"; break;        /**<5,  Deep Sleep reset digital core*/
//    case 6 : p = "SDIO_RESET"; break;             /**<6,  Reset by SLC module, reset digital core*/
//    case 7 : p = "TG0WDT_SYS_RESET"; break;       /**<7,  Timer Group0 Watch dog reset digital core*/
//    case 8 : p = "TG1WDT_SYS_RESET"; break;       /**<8,  Timer Group1 Watch dog reset digital core*/
//    case 9 : p = "RTCWDT_SYS_RESET"; break;       /**<9,  RTC Watch dog Reset digital core*/
//    case 10 : p = "INTRUSION_RESET"; break;       /**<10, Instrusion tested to reset CPU*/
//    case 11 : p = "TGWDT_CPU_RESET"; break;       /**<11, Time Group reset CPU*/
//    case 12 : p = "SW_CPU_RESET"; break;          /**<12, Software reset CPU*/
//    case 13 : p = "RTCWDT_CPU_RESET"; break;      /**<13, RTC Watch dog Reset CPU*/
//    case 14 : p = "EXT_CPU_RESET"; break;         /**<14, for APP CPU, reseted by PRO CPU*/
//    case 15 : p = "RTCWDT_BROWN_OUT_RESET"; break;/**<15, Reset when the vdd voltage is not stable*/
//    case 16 : p = "RTCWDT_RTC_RESET"; break;      /**<16, RTC Watch dog reset digital core and rtc module*/
//    default : p = "NO_MEAN"; break;
//  }
//  return p;
//}
//
//const char* reset_reason() { return reset_reason(rtc_get_reset_reason(0)); }


// ************ Configure Bluetooth Security parameters ********************

// I think ESP_BLE_OOB_ENABLE would allow authentication by touch pad or switch rather than PIN. Might want that later.
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
// On wake from deep sleep the program starts from scratch (only RTC_DATA_ATTR data is preserved) and runs startup() then loop().

#define DEVICE_NAME         "NB Motion"
#define SERVICE_UUID        "09d3aac8-dd78-46ef-a712-9523ebd90f1f"
#define CHARACTERISTIC_UUID "27a78b33-b6d8-4811-8563-343389338936"
#define DESCRIPTOR_UUID     "52802a1c-6592-4c3e-ac4b-23f7f68be89d"

#define PIN_PIR              GPIO_NUM_39
#define PIN_LED_AWAKE        GPIO_NUM_26
#define PIN_LED_STAY_AWAKE   GPIO_NUM_25
#define PIN_TOUCH_STAY_AWAKE GPIO_NUM_32



// ************ Interrupt Service Routines ********************

#define THRESHOLD 40

class ISR {
  public:
  
  static volatile bool touchStayAwake;
  static volatile bool pirMotion;
  
  // gets called many times with v < THRESHOLD for one touch, never called with v > THRESHOLD
  // so reset touchStayAwake = false in loop().
  static void touchStayAwakeISR() {
    touchStayAwake = true;
  }
  
  
  
  
  // gets called many times for HIGH then once for LOW
  static void pirChangeISR() {
    pirMotion = digitalRead(PIN_PIR) == HIGH;
  }
};

volatile bool ISR::touchStayAwake = false;
volatile bool ISR::pirMotion = false;



// ************ Timer ********************

#define TIME_CONNECT    60000UL     // pairing & connection timeout (milli secs), client has this long to connect before deep sleep
#define TIME_NOTIFY     10000UL     // notify period (milli secs), after connect client has this long to read values before deep sleep 

class Timer {
  
  unsigned long bedTime = 0; // time at which we will enter deep sleep

  public:
  
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

  bool isBedTime() {
    return millis() > bedTime;
  }
};



// ************ Bluetooth Pairing & Connection ********************

class MySecurity : public BLESecurityCallbacks {

  Timer *timer;

  public:
  
  MySecurity(Timer *t) {
    timer = t;
  }

  // when client says yes pair with us, client must then enter this pass_key 
  void onPassKeyNotify(uint32_t pass_key) {
    log_d("MySecurity", "onPassKeyNotify: pass_key = %u \n", pass_key);
    timer->resetConnectTimer();
  }

  // when client has authenticated, is paired
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
    log_d("MySecurity", "onAuthenticationComplete \n");
    timer->resetNotifyTimer();
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



class MyServer: public BLEServerCallbacks {
  
  private:
  
  Timer *timer;
  BLESecurityCallbacks *security;
  
  int clientCount = 0;
  BLEServer *pServer;
  BLECharacteristic *pCharacteristic;

  public:
  
  MyServer(Timer *t, BLESecurityCallbacks *mySec) {
    timer = t;
    security = mySec;
    
    // Create BLE hierarchy: Device > Service > Characteristic > Descriptors
    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
    BLEDevice::setSecurityCallbacks(security);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(this);
  
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
      d->setValue("PIR motion sensor");
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
  }

  void advertise() {
    BLEDevice::startAdvertising();
    timer->resetConnectTimer();
  }

  void setMotion(bool motion) {
    uint8_t value = motion ? 0xFF : 0x00;
    pCharacteristic->setValue(&value, 1);
  }

  void notifyMotion(bool motion) {
    setMotion(motion);
    pCharacteristic->notify();  
  }

  void onConnect(BLEServer* pServer) {
    ++clientCount;
    log_d("MyServer::onConnect", "clientCount = %d \n", clientCount);
    // advertise(); // example code advertises here, but I don't think that is correct
    timer->resetConnectTimer();
    
    // if first connection since wakeup and time since wakeup is shortish then notify motion then no motion
    // if (rtc_get_reset_reason(0) == DEEPSLEEP_RESET)
    setMotion(true);
  };

  void onDisconnect(BLEServer* pServer) {
    --clientCount;
    log_d("MyServer::onDisconnect", "clientCount = %d \n", clientCount);
    if (clientCount < 1) advertise(); // only supposed to advertise when no clients connected?
  }

};


Timer *timer;
MyServer *myServer;

void setup() {
  Serial.begin(115200);
  log_i("setup", "\n");
  
  
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_LED_AWAKE, OUTPUT);
  pinMode(PIN_LED_STAY_AWAKE, OUTPUT);
  // pinMode not used for touch inputs

  digitalWrite(PIN_LED_AWAKE, HIGH);
  ISR::pirMotion = digitalRead(PIN_PIR) == HIGH;

  timer = new Timer();
  myServer = new MyServer(timer, new MySecurity(timer));
  
  touchAttachInterrupt(PIN_TOUCH_STAY_AWAKE, ISR::touchStayAwakeISR, THRESHOLD);
  attachInterrupt(digitalPinToInterrupt(PIN_PIR), ISR::pirChangeISR, CHANGE);
  
  esp_sleep_enable_ext0_wakeup(PIN_PIR, 1); // 1 = High, 0 = Low
  log_i("setup", "end \n");
}

bool stayAwake = false;
int prevState = 0;
bool prevPirMotion = false;

void loop() {
  log_i("loop", "start, state = %d \n", timer->state);
  if (timer->state == 2) {
    if (prevState != 2 && rtc_get_reset_reason(0) == DEEPSLEEP_RESET) {
      log_i("loop", "client just reconnected after deep sleep\n");
//      delay(1000); // 100 not enough
//      myServer->notifyMotion(true);
      delay(1000);
      myServer->notifyMotion(false);
      prevPirMotion = false;
    } else if (ISR::pirMotion != prevPirMotion) {
      log_i("loop", "pirMotion = %s \n", ISR::pirMotion ? "true" : "false");
      myServer->notifyMotion(ISR::pirMotion);
      timer->resetNotifyTimer();
      prevPirMotion = ISR::pirMotion;
    }
  }
  if (prevState == 0) ISR::touchStayAwake = false; // the ISR seems to get an initial spurious call - not working
  prevState = timer->state;
  
  if (ISR::touchStayAwake) {
    stayAwake = !stayAwake;
    log_i("loop", "stayAwake = %s \n", stayAwake ? "true" : "false");
    digitalWrite(PIN_LED_STAY_AWAKE, stayAwake ? HIGH : LOW);
    ISR::touchStayAwake = false;
  }
  
  if (!stayAwake && timer->isBedTime()) {
    log_i("loop", "deep sleep");
    Serial.flush();
    esp_deep_sleep_start();
  }
  
  delay(100);
  log_i("loop", "end \n");
}
