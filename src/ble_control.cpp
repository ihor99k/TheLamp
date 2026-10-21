#include "ble_control.h"

#include <NimBLEDevice.h>

namespace {

constexpr const char* SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char* RX_UUID      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  // phone -> lamp
constexpr const char* TX_UUID      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  // lamp -> phone

constexpr size_t CMD_MAX = 128;
struct Cmd {
  char text[CMD_MAX];
};

// Writes arrive on the NimBLE task; the queue hands them over to loop().
QueueHandle_t queue;
NimBLECharacteristic* tx;

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* ch, NimBLEConnInfo&) override {
    NimBLEAttValue value = ch->getValue();
    Cmd cmd;
    size_t n = value.length();
    if (n > CMD_MAX - 1) n = CMD_MAX - 1;
    memcpy(cmd.text, value.data(), n);
    cmd.text[n] = 0;
    xQueueSend(queue, &cmd, 0);  // drop if loop() is stuck rather than block BLE
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    Serial.printf("BLE: connected %s\n", info.getAddress().toString().c_str());
    // Ask for a 15-30 ms connection interval (units of 1.25 ms): commands arrive sooner.
    server->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
    Serial.printf("BLE: MTU %u\n", mtu);
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    Serial.printf("BLE: disconnected (reason %d)\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

}  // namespace

namespace ble {

void begin(const char* name) {
  queue = xQueueCreate(16, sizeof(Cmd));

  NimBLEDevice::init(name);
  NimBLEDevice::setMTU(247);  // lets the full state JSON fit in one notification

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);
  NimBLECharacteristic* rx =
      service->createCharacteristic(RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  tx = service->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(name);
  adv->addServiceUUID(SERVICE_UUID);
  adv->start();
  Serial.printf("BLE: advertising as \"%s\"\n", name);
}

bool poll(char* out, size_t len) {
  Cmd cmd;
  if (!queue || xQueueReceive(queue, &cmd, 0) != pdTRUE) return false;
  strlcpy(out, cmd.text, len);
  return true;
}

void publish(const String& state) {
  if (!tx) return;
  tx->setValue(reinterpret_cast<const uint8_t*>(state.c_str()), state.length());
  tx->notify();
}

}  // namespace ble
