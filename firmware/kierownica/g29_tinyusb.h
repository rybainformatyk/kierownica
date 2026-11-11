/* =============================================================================
 *  G29 przez TinyUSB - ESP32-S3 / ESP32-S2 (USB Mode: USB-OTG)
 * =============================================================================
 *
 *  Opis raportu: g29_raport.h. Gniazdo USB (natywne, nie UART) jest
 *  urzadzeniem zlozonym: port COM (lacze z aplikacja) + G29, jak w Leonardo.
 *  Odlaczyc G29 sie nie da - zniknalby tez port COM.
 *
 *  VID/PID trzeba ustawic, zanim ruszy stos USB, dlatego "USB CDC On Boot"
 *  musi byc wylaczone (Disabled), a port COM tworzy szkic (wheelCdc).
 *  Skrypt wgraj_firmware.ps1 ustawia to sam.
 *
 *  Raport idzie przez tud_hid_n_report(), a nie USBHID::SendReport() - ta
 *  druga czeka na potwierdzenie hosta, a petla nie moze czekac.
 * =============================================================================
 */

#pragma once

#include <Arduino.h>
#include "USB.h"
#include "USBHID.h"
#include "g29_raport.h"

static USBHID g29Hid;

class G29Usb : public USBHIDDevice {
public:
  G29Usb() { memcpy(report, G29_NEUTRAL, G29_REPORT_SIZE); }

  // Przed USB.begin(): tozsamosc G29 i interfejs HID.
  void begin() {
    USB.VID(G29_VID);
    USB.PID(G29_PID);
    USB.firmwareVersion(G29_BCD_DEVICE);
    USB.manufacturerName(G29_MANUFACTURER);
    USB.productName(G29_PRODUCT);
    USB.serialNumber("G29");
    USBHID::addDevice(this, sizeof(G29_REPORT_DESC));
    g29Hid.begin();
  }

  bool setReport(const uint8_t *p, uint8_t len) {
    if (len != G29_REPORT_SIZE) return false;
    portENTER_CRITICAL(&lock);
    if (memcmp(report, p, G29_REPORT_SIZE) != 0) {
      memcpy(report, p, G29_REPORT_SIZE);
      dirty = true;
    }
    portEXIT_CRITICAL(&lock);
    return true;
  }

  void setNeutral() { setReport(G29_NEUTRAL, G29_REPORT_SIZE); }

  bool hostConfigured() { return (bool)USB; }
  void setAttached(bool) {}
  bool attached() const { return true; }

  void poll() {
    if (!USB) {
      wasConfigured = false;
      return;
    }
    if (!wasConfigured) {
      wasConfigured = true;
      dirty = true;
    }
    if (!dirty || !tud_hid_n_ready(0)) return;

    uint8_t copy[G29_REPORT_SIZE];
    portENTER_CRITICAL(&lock);
    memcpy(copy, report, G29_REPORT_SIZE);
    dirty = false;
    portEXIT_CRITICAL(&lock);
    if (!tud_hid_n_report(0, 0, copy, G29_REPORT_SIZE)) dirty = true;
  }

  uint8_t takeOutput(uint8_t *dst) {
    if (!outPending) return 0;
    portENTER_CRITICAL(&lock);
    uint8_t n = outLen;
    memcpy(dst, outBuf, n);
    outPending = false;
    portEXIT_CRITICAL(&lock);
    return n;
  }

  // --- wolane przez stos TinyUSB (inne zadanie FreeRTOS) ---
  uint16_t _onGetDescriptor(uint8_t *buffer) override {
    memcpy(buffer, G29_REPORT_DESC, sizeof(G29_REPORT_DESC));
    return sizeof(G29_REPORT_DESC);
  }

  void _onOutput(uint8_t, const uint8_t *buffer, uint16_t len) override {
    const uint8_t keep = (len > G29_OUT_MAX) ? G29_OUT_MAX : (uint8_t)len;
    portENTER_CRITICAL(&lock);
    memcpy(outBuf, buffer, keep);
    outLen = keep;
    outPending = true;
    portEXIT_CRITICAL(&lock);
  }

private:
  portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
  uint8_t  report[G29_REPORT_SIZE];
  volatile bool dirty = true;
  bool     wasConfigured = false;

  volatile bool outPending = false;
  uint8_t  outLen = 0;
  uint8_t  outBuf[G29_OUT_MAX];
};
