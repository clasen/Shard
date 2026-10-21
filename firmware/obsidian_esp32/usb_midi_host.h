// USB host for class-compliant MIDI controllers (ESP-IDF usb_host on the native OTG port).
// Finds the first MIDI Streaming interface, reads its bulk IN endpoint and forwards each
// 4-byte USB-MIDI event packet to a callback.
#pragma once

#include <Arduino.h>
#include <string.h>
#include "usb/usb_host.h"

namespace midihost {

using PacketHandler = void (*)(const uint8_t packet[4]);

struct State {
  usb_host_client_handle_t client = nullptr;
  usb_device_handle_t dev = nullptr;
  usb_transfer_t* xfer = nullptr;
  uint8_t iface = 0, epIn = 0;
  uint16_t maxPacket = 64;
  bool claimed = false;
  volatile bool connected = false;
  char name[64] = "";
  PacketHandler handler = nullptr;
};
inline State gState;

inline void onTransfer(usb_transfer_t* t) {
  State& s = gState;
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    for (int i = 0; i + 3 < t->actual_num_bytes; i += 4) {
      const uint8_t* p = t->data_buffer + i;
      if ((p[0] & 0x0f) != 0 && s.handler) s.handler(p);
    }
  }
  if (s.claimed && t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED) {
    t->num_bytes = s.maxPacket;
    usb_host_transfer_submit(t);
  }
}

inline void readName(usb_device_handle_t dev, char* out, size_t len) {
  out[0] = 0;
  usb_device_info_t info;
  if (usb_host_device_info(dev, &info) != ESP_OK || !info.str_desc_product) return;
  const usb_str_desc_t* d = info.str_desc_product;
  size_t n = 0;
  for (int i = 0; i < (d->bLength - 2) / 2 && n + 1 < len; i++) {
    uint16_t c = d->wData[i];
    out[n++] = c < 128 ? (char)c : '?';
  }
  out[n] = 0;
}

inline void openDevice(uint8_t address) {
  State& s = gState;
  if (s.dev) return;  // one controller at a time
  if (usb_host_device_open(s.client, address, &s.dev) != ESP_OK) return;

  const usb_config_desc_t* cfg = nullptr;
  if (usb_host_get_active_config_descriptor(s.dev, &cfg) != ESP_OK) return;
  const uint8_t* p = (const uint8_t*)cfg;
  const uint8_t* end = p + cfg->wTotalLength;
  bool inMidi = false, found = false;
  while (p + 2 <= end && p[0] >= 2) {
    uint8_t len = p[0], type = p[1];
    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= 9) {
      inMidi = p[5] == 0x01 && p[6] == 0x03;  // Audio class, MIDI Streaming subclass
      if (inMidi) s.iface = p[2];
    } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && len >= 7 && inMidi) {
      uint8_t addr = p[2], attr = p[3];
      if ((addr & 0x80) && (attr & 0x03) == 0x02) {
        s.epIn = addr;
        s.maxPacket = p[4] | (p[5] << 8);
        found = true;
        break;
      }
    }
    p += len;
  }
  readName(s.dev, s.name, sizeof(s.name));
  if (!found) {
    Serial.printf("USB device '%s' has no MIDI input, ignoring\n", s.name);
    usb_host_device_close(s.client, s.dev);
    s.dev = nullptr;
    return;
  }
  if (usb_host_interface_claim(s.client, s.dev, s.iface, 0) != ESP_OK) {
    Serial.println("USB MIDI: interface claim failed");
    return;
  }
  s.claimed = true;
  usb_host_transfer_alloc(s.maxPacket, 0, &s.xfer);
  s.xfer->device_handle = s.dev;
  s.xfer->bEndpointAddress = s.epIn;
  s.xfer->callback = onTransfer;
  s.xfer->context = nullptr;
  s.xfer->num_bytes = s.maxPacket;
  usb_host_transfer_submit(s.xfer);
  s.connected = true;
  Serial.printf("USB MIDI controller connected: '%s' (iface %u, ep 0x%02x)\n", s.name, s.iface, s.epIn);
}

inline void closeDevice() {
  State& s = gState;
  s.connected = false;
  if (s.claimed) { usb_host_interface_release(s.client, s.dev, s.iface); s.claimed = false; }
  if (s.xfer) { usb_host_transfer_free(s.xfer); s.xfer = nullptr; }
  if (s.dev) { usb_host_device_close(s.client, s.dev); s.dev = nullptr; }
  Serial.println("USB MIDI controller disconnected");
}

inline void onClientEvent(const usb_host_client_event_msg_t* msg, void*) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) openDevice(msg->new_dev.address);
  else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) closeDevice();
}

inline void libTask(void*) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

inline void clientTask(void*) {
  for (;;) usb_host_client_handle_events(gState.client, portMAX_DELAY);
}

// Starts the host stack; handler runs in the client task (core `core`).
inline bool begin(PacketHandler handler, int core) {
  gState.handler = handler;
  usb_host_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.skip_phy_setup = false;
  cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&cfg) != ESP_OK) return false;
  xTaskCreatePinnedToCore(libTask, "usbhost", 4096, nullptr, 5, nullptr, core);

  usb_host_client_config_t ccfg;
  memset(&ccfg, 0, sizeof(ccfg));
  ccfg.is_synchronous = false;
  ccfg.max_num_event_msg = 5;
  ccfg.async.client_event_callback = onClientEvent;
  ccfg.async.callback_arg = nullptr;
  if (usb_host_client_register(&ccfg, &gState.client) != ESP_OK) return false;
  xTaskCreatePinnedToCore(clientTask, "usbmidi", 4096, nullptr, 5, nullptr, core);
  return true;
}

}  // namespace midihost
