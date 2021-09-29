
#include <stdio.h>
#include <stdarg.h>
#include <unordered_set>
#include <unordered_map>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>



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

#define LOG_LEVEL LOG_INFO

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



// ****** Manage a BLEClient (for a BLEServer we connect to) ********

static BLEUUID serviceUUID("09d3aac8-dd78-46ef-a712-9523ebd90f1f");
static BLEUUID charUUID("27a78b33-b6d8-4811-8563-343389338936");

class MyClient : public BLEClientCallbacks {
  public:
  
  MyClient(std::string ad) {
    addr = ad;
    pClient = BLEDevice::createClient();
    pServ = NULL;
    pChar = NULL;
    connected = false;
  }
  
  ~MyClient(){
    delete pClient;
    if (pServ != nullptr) delete pServ;
    if (pChar != nullptr) delete pChar;
  }

  bool init() {
    pClient->setClientCallbacks(this);
    pClient->connect(BLEAddress(addr));
    BLERemoteService* pServ = pClient->getService(serviceUUID);
    if (pServ != nullptr) {
      BLERemoteCharacteristic *pChar = pServ->getCharacteristic(charUUID);
      if (pChar != nullptr && pChar->canNotify()) {
        // lambda function for callback
        auto addr2 = addr;
        auto cb = [addr2] (BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
          log_i("notifyCallback", "address = 0x%s, characteristic = 0x%s, length = %d, data = 0x%x, isNotify = %s\n", addr2.c_str(), pChar->getUUID().toString().c_str(), length, *pData, isNotify ? "true" : "false");
        };
        if (pChar->canRead()) {
          // send initial value to callback (about 0.8s before 1st callback invocation)
          uint8_t v = pChar->readUInt8();
          cb(pChar, &v, 1, false);
        }
        pChar->registerForNotify(cb);
        log_i("MyClient::init", "succeeded\n");
        return true;
      }
    }
    log_i("MyClient::init", "failed\n");
    return false;
  }

  void onConnect(BLEClient* x) {
    connected = true;
    log_i("MyClient::onConnect", "address = 0x%s\n", addr.c_str());
  }

  void onDisconnect(BLEClient* x) {
    connected = false;
    if (pServ != nullptr) {
      delete pServ;
      pServ = NULL;
    }
    if (pChar != nullptr) {
      delete pChar;
      pChar = NULL;
    }
    log_i("MyClient::onDisconnect", "address = 0x%s\n", addr.c_str());
  }

  bool connected;

  private:
  
  std::string addr;
  BLEClient*  pClient;
  BLERemoteService* pServ;
  BLERemoteCharacteristic *pChar;
};



#define DEVICE_NAME "NB Client"

class Clients: public BLEAdvertisedDeviceCallbacks {

  std::unordered_set<std::string> addresses; // of servers offering serviceUUID found by last scan 
  std::unordered_map<std::string, MyClient*> clients; // a client for every server ever connected to (since boot)

  // first few scans complete in 4 to 6ms, but then they take about 1s! 
  const int scanFreq = 50; // run scan only once every scanFreq loops
  int loopCount = 0;       // mod scanFreq
  
  const int scanTimeout = 5; // secs
  BLEScan* pBLEScan;

  // scan result callback 
  void onResult(BLEAdvertisedDevice d) {
    log_d("onResult", "Advertised Device: %s \n", d.toString().c_str());
    if (d.haveServiceUUID() && d.isAdvertisingService(serviceUUID)) {
      addresses.insert(d.getAddress().toString());
    }
  }

  public:

  void setup() {
    Serial.begin(115200);
    log_d("Clients::setup", "%s \n", DEVICE_NAME);
    BLEDevice::init(DEVICE_NAME);
    pBLEScan = BLEDevice::getScan(); // initialise scanning (run in loop() not here)
    // if callback is used:
    // - no results are returned by BLEScan::start()
    // - callback gets many duplicate results (no duplicates if callback not used)
    pBLEScan->setAdvertisedDeviceCallbacks(this);
    pBLEScan->setActiveScan(true); // active scan uses more power, but get results faster
    pBLEScan->setInterval(40);
    pBLEScan->setWindow(30);  // less or equal setInterval value
  }
  
  void loop() {
    auto tag = "Clients::loop";
    log_i(tag, "start\n");
    
    // We want to re-establish connections and get callbacks happening ASAP, so first try re-connecting
    for (auto itr : clients) {
      if (!itr.second->connected) {
        bool x = itr.second->init();
        log_i(tag, "reconnect client address 0x%s %s\n", itr.first.c_str(), x ? "succeeded" : "failed");
      }
    }

    if (loopCount == 0) {
      pBLEScan->start(scanTimeout, false);
      pBLEScan->clearResults();   // release memory (probably a noop when using callback)
      log_i(tag, "scan complete\n");
      
      for (auto &addr : addresses) {
        auto itr = clients.find(addr);
        log_i(tag, "scanned address %s %s\n", addr.c_str(), itr == clients.end() ? "not found in clients" : "found in clients");
        if (itr == clients.end()) {
          // add MyClient (unless it doesn't have the characteristic we expect)
          auto p = new MyClient(addr);
          if (p->init()) {
            clients.insert(std::pair<std::string, MyClient*>(addr, p));
            log_i(tag, "MyClient added\n");
          } else {
            delete p;
            log_i(tag, "MyClient not added because init() failed\n");
          }
        } else if (!itr->second->connected) {
          bool x = itr->second->init();
          log_i(tag, "MyClient was disconnected, init() %s\n", x ? "succeeded" : "failed");
        }
      }
      addresses.clear();
    }
    loopCount = (loopCount + 1) % scanFreq;
    log_i(tag, "end\n");
    delay(200);
  }
};


// Crashes with: CORRUPT HEAP: Bad head at 0x3ffd3004. Expected 0xabba1234 got 0x3ffd30c8
// then reboots. Incorrect memory usage?

Clients cs;
void setup() { cs.setup(); }
void loop() { cs.loop(); } 
