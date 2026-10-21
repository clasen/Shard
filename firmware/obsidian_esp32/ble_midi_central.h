// BLE-MIDI central: scans for the first peripheral advertising the BLE-MIDI service, subscribes to
// its MIDI I/O characteristic and decodes the incoming packets (timestamps, running status) into
// plain MIDI messages. Reconnects after a disconnect.
#pragma once

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLESecurity.h>

namespace blemidi {

using MessageHandler = void (*)(uint8_t status, uint8_t d1, uint8_t d2);

inline const BLEUUID kService("03B80E5A-EDE8-4B33-A751-6CE34EC4C700");
inline const BLEUUID kIo("7772E5DB-3868-4112-A1A9-F2669D106BF3");

struct State {
  MessageHandler handler = nullptr;
  BLEClient* client = nullptr;
  BLEAdvertisedDevice* found = nullptr;
  volatile bool connected = false;
  volatile bool scanning = false;
  volatile uint32_t packets = 0;
  String name;
};
inline State gState;

// One BLE-MIDI packet: header, then [timestamp] status data... with running status allowed.
inline void decode(const uint8_t* d, size_t n) {
  State& s = gState;
  s.packets++;
  if (n < 2 || !(d[0] & 0x80)) return;
  uint8_t status = 0;
  bool sysex = false;
  size_t i = 1;
  while (i < n) {
    if (d[i] & 0x80) {  // timestamp low byte, followed by a status or a running-status data byte
      if (++i >= n) break;
      if (d[i] & 0x80) {
        if (sysex && d[i] == 0xf7) { sysex = false; i++; continue; }
        status = d[i++];
        sysex = status == 0xf0;
        if (status >= 0xf8) { s.handler(status, 0, 0); continue; }
      }
    }
    if (sysex || status == 0 || status >= 0xf0) { i++; continue; }
    int len = (status & 0xe0) == 0xc0 ? 1 : 2;  // program change and channel pressure carry one byte
    if (i + len > n) break;
    s.handler(status, d[i], len == 2 ? d[i + 1] : 0);
    i += len;
  }
}

struct ScanCallbacks : BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    State& s = gState;
    if (s.found || !dev.haveServiceUUID() || !dev.isAdvertisingService(kService)) return;
    s.found = new BLEAdvertisedDevice(dev);
    BLEDevice::getScan()->stop();
  }
};

struct ClientCallbacks : BLEClientCallbacks {
  void onConnect(BLEClient*) override {}
  void onDisconnect(BLEClient*) override { gState.connected = false; }
};

inline void onScanDone(BLEScanResults) { gState.scanning = false; }

inline void startScan() {
  State& s = gState;
  BLEScan* scan = BLEDevice::getScan();
  scan->clearResults();
  s.scanning = scan->start(5, onScanDone, false);
}

inline bool connect() {
  State& s = gState;
  s.name = s.found->getName();
  Serial.printf("BLE MIDI: connecting to '%s' (%s)\n", s.name.c_str(), s.found->getAddress().toString().c_str());
  if (!s.client->connect(s.found)) return false;
  // BLE-MIDI peripherals only notify over an encrypted link: pair (Just Works) before subscribing.
  BLESecurity::resetSecurity();
  if (!s.client->secureConnection()) Serial.println("BLE MIDI: pairing failed, trying unencrypted");
  BLERemoteService* svc = s.client->getService(kService);
  BLERemoteCharacteristic* io = svc ? svc->getCharacteristic(kIo) : nullptr;
  if (!io || !io->canNotify()) {
    Serial.println("BLE MIDI: device has no MIDI I/O characteristic");
    s.client->disconnect();
    return false;
  }
  io->registerForNotify([](BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) { decode(data, len); });
  s.connected = true;
  Serial.printf("BLE MIDI controller connected: '%s'\n", s.name.c_str());
  return true;
}

// Starts scanning; `handler` runs in the Bluetooth task.
inline void begin(MessageHandler handler, const char* deviceName) {
  State& s = gState;
  s.handler = handler;
  BLEDevice::init(deviceName);
  BLESecurity* security = new BLESecurity();
  security->setCapability(ESP_IO_CAP_NONE);
  security->setAuthenticationMode(true, false, true);  // bonding, no MITM (no PIN), secure connections
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);  // the service UUID may only be in the scan response
  scan->setInterval(100);
  scan->setWindow(99);
  s.client = BLEDevice::createClient();
  s.client->setClientCallbacks(new ClientCallbacks());
  startScan();
}

// Call from loop(): connects to a found device, or rescans while disconnected.
inline void poll() {
  State& s = gState;
  static bool wasConnected = false;
  if (wasConnected && !s.connected) Serial.printf("BLE MIDI controller disconnected: '%s'\n", s.name.c_str());
  wasConnected = s.connected;
  if (s.connected) return;
  if (s.found) {  // the scan was stopped from onResult, which may not report completion
    s.scanning = false;
    if (!connect()) Serial.println("BLE MIDI: connection failed, scanning again");
    delete s.found;
    s.found = nullptr;
    wasConnected = s.connected;
    if (s.connected) return;
  }
  if (!s.scanning) startScan();
}

}  // namespace blemidi
