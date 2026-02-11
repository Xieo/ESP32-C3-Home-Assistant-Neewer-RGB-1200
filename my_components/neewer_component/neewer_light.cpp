#include "neewer_light.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/core/log.h"

#include <esp_gattc_api.h>

namespace esphome {
namespace neewer_component {

static const char *const TAG = "neewer_component";

static esp_bt_uuid_t make_uuid128(const uint8_t raw[16]) {
  esp_bt_uuid_t uuid{};
  uuid.len = ESP_UUID_LEN_128;
  std::memcpy(uuid.uuid.uuid128, raw, 16);
  return uuid;
}

// UUIDs (from your Python)
static const uint8_t SVC_BE[16] = {0x69,0x40,0x00,0x01,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x99};
static const uint8_t SVC_LE[16] = {0x99,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x69};

static const uint8_t CHR_BE[16] = {0x69,0x40,0x00,0x02,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x99};
static const uint8_t CHR_LE[16] = {0x99,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,0x93,0xF3,0xA3,0xB5,0x02,0x00,0x40,0x69};

static const auto SVC_UUID_BE = esp32_ble::ESPBTUUID::from_uuid(make_uuid128(SVC_BE));
static const auto SVC_UUID_LE = esp32_ble::ESPBTUUID::from_uuid(make_uuid128(SVC_LE));
static const auto CHR_UUID_BE = esp32_ble::ESPBTUUID::from_uuid(make_uuid128(CHR_BE));
static const auto CHR_UUID_LE = esp32_ble::ESPBTUUID::from_uuid(make_uuid128(CHR_LE));

static inline uint8_t clamp_u8_100(int v) {
  if (v < 0) return 0;
  if (v > 100) return 100;
  return (uint8_t) v;
}

static inline void rgb_to_hsv_deg(float r, float g, float b, int &h_deg, int &s_100, int &v_100) {
  float maxv = fmaxf(r, fmaxf(g, b));
  float minv = fminf(r, fminf(g, b));
  float delta = maxv - minv;

  float h = 0.0f;
  float s = (maxv <= 0.00001f) ? 0.0f : (delta / maxv);
  float v = maxv;

  if (delta > 0.00001f) {
    if (maxv == r) h = 60.0f * fmodf(((g - b) / delta), 6.0f);
    else if (maxv == g) h = 60.0f * (((b - r) / delta) + 2.0f);
    else h = 60.0f * (((r - g) / delta) + 4.0f);
  } else {
    h = 0.0f;
  }

  if (h < 0.0f) h += 360.0f;
  h_deg = (int) lroundf(h);
  if (h_deg >= 360) h_deg = 359;

  s_100 = (int) lroundf(s * 100.0f);
  if (s_100 < 0) s_100 = 0;
  if (s_100 > 100) s_100 = 100;

  v_100 = (int) lroundf(v * 100.0f);
  if (v_100 < 0) v_100 = 0;
  if (v_100 > 100) v_100 = 100;
}

void NeewerRGBCCTLight::set_ble_client(ble_client::BLEClient *client) {
  client_ = client;
  if (client_ != nullptr) {
    client_->register_ble_node(this);
    this->node_state = esp32_ble_tracker::ClientState::IDLE;
  }
}

light::LightTraits NeewerRGBCCTLight::get_traits() {
  auto traits = light::LightTraits();

  // ✅ This forces HA to show: Color wheel + Color temperature (NO cold/warm sliders)
  traits.set_supported_color_modes({light::ColorMode::RGB, light::ColorMode::COLOR_TEMPERATURE});

  traits.set_min_mireds(118);  // ~8500K
  traits.set_max_mireds(400);  // ~2500K
  return traits;
}

void NeewerRGBCCTLight::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t,
                                           esp_ble_gattc_cb_param_t *) {
  if (client_ == nullptr) return;

  if (event == ESP_GATTC_DISCONNECT_EVT || event == ESP_GATTC_CLOSE_EVT) {
    handles_ready_ = false;
    write_handle_ = 0;
    this->node_state = esp32_ble_tracker::ClientState::IDLE;
    ESP_LOGW(TAG, "BLE disconnected (handles cleared)");
    return;
  }

  if (event == ESP_GATTC_SEARCH_CMPL_EVT) {
    auto *svc = client_->get_service(SVC_UUID_BE);
    bool use_be = true;
    if (svc == nullptr) {
      svc = client_->get_service(SVC_UUID_LE);
      use_be = false;
    }
    if (svc == nullptr) {
      ESP_LOGW(TAG, "Service not found during discovery");
      this->node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
      return;
    }

    auto *chr = svc->get_characteristic(use_be ? CHR_UUID_BE : CHR_UUID_LE);
    if (chr == nullptr) chr = svc->get_characteristic(use_be ? CHR_UUID_LE : CHR_UUID_BE);
    if (chr == nullptr) {
      ESP_LOGW(TAG, "Characteristic not found during discovery");
      this->node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
      return;
    }

    write_handle_ = chr->handle;
    handles_ready_ = (write_handle_ != 0);
    ESP_LOGI(TAG, "✅ Cached write handle: 0x%04X", write_handle_);

    this->node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
  }
}

bool NeewerRGBCCTLight::write_packet_(const uint8_t *data, size_t len) {
  if (client_ == nullptr || !client_->connected()) return false;
  if (!handles_ready_ || write_handle_ == 0) return false;

  esp_err_t err = esp_ble_gattc_write_char(
      client_->get_gattc_if(),
      client_->get_conn_id(),
      write_handle_,
      (uint16_t) len,
      (uint8_t *) data,
      ESP_GATT_WRITE_TYPE_NO_RSP,
      ESP_GATT_AUTH_REQ_NONE);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Write failed err=%d", (int) err);
    return false;
  }
  return true;
}

void NeewerRGBCCTLight::write_state(light::LightState *state) {
  if (client_ == nullptr) return;

  if (!client_->connected()) {
    ESP_LOGW(TAG, "BLE not connected; skip");
    return;
  }
  if (!handles_ready_) {
    ESP_LOGW(TAG, "Write handle not ready yet; skip");
    return;
  }

  // ✅ Detect which UI mode HA is using
  const auto mode = state->current_values.get_color_mode();
  const bool on = state->current_values.is_on();

  // POWER (12 bytes)
  {
    uint8_t pkt[12];
    const uint8_t header[10] = {0x78, 0x8D, 0x08, 0xE4, 0x7D, 0x33, 0x77, 0x85, 0x52, 0x81};
    for (int i = 0; i < 10; i++) pkt[i] = header[i];
    pkt[10] = on ? 0x01 : 0x02;

    uint16_t sum = 0;
    for (int i = 0; i < 11; i++) sum += pkt[i];
    pkt[11] = (uint8_t) (sum & 0xFF);

    write_packet_(pkt, sizeof(pkt));
  }

  if (!on) return;

  // =========================
  // COLOR TEMPERATURE MODE
  // =========================
  if (mode == light::ColorMode::COLOR_TEMPERATURE) {
    float ct_mireds = state->current_values.get_color_temperature();
    float brightness = state->current_values.get_brightness();

    int bri_100 = (int) lroundf(brightness * 100.0f);
    bri_100 = (int) clamp_u8_100(bri_100);

    float kelvin = (ct_mireds > 1.0f) ? (1000000.0f / ct_mireds) : 4000.0f;
    if (kelvin < 2500.0f) kelvin = 2500.0f;
    if (kelvin > 8500.0f) kelvin = 8500.0f;

    int temp_byte = (int) lroundf(kelvin / 100.0f);
    if (temp_byte < 25) temp_byte = 25;
    if (temp_byte > 85) temp_byte = 85;

    uint8_t pkt[16];
    const uint8_t header[10] = {0x78, 0x90, 0x0C, 0xE4, 0x7D, 0x33, 0x77, 0x85, 0x52, 0x87};
    for (int i = 0; i < 10; i++) pkt[i] = header[i];

    pkt[10] = (uint8_t) bri_100;
    pkt[11] = (uint8_t) temp_byte;
    pkt[12] = 0x32;
    pkt[13] = 0x00;
    pkt[14] = 0x00;

    uint16_t sum = 0;
    for (int i = 0; i < 15; i++) sum += pkt[i];
    pkt[15] = (uint8_t) (sum & 0xFF);

    write_packet_(pkt, sizeof(pkt));
    return;
  }

  // =========================
  // RGB MODE
  // =========================
  float r, g, b;
  state->current_values_as_rgb(&r, &g, &b);

  float brightness = state->current_values.get_brightness();
  int bri_100 = (int) lroundf(brightness * 100.0f);
  bri_100 = (int) clamp_u8_100(bri_100);

  int h_deg = 0, s_100 = 0, v_100 = 0;
  rgb_to_hsv_deg(r, g, b, h_deg, s_100, v_100);

  uint8_t pkt[16];
  const uint8_t header[10] = {0x78, 0x8F, 0x0C, 0xE4, 0x7D, 0x33, 0x77, 0x85, 0x52, 0x86};
  for (int i = 0; i < 10; i++) pkt[i] = header[i];

  pkt[10] = (uint8_t) (h_deg & 0xFF);
  pkt[11] = (uint8_t) ((h_deg >> 8) & 0xFF);
  pkt[12] = (uint8_t) clamp_u8_100(s_100);
  pkt[13] = (uint8_t) clamp_u8_100(bri_100);
  pkt[14] = 0x00;

  uint16_t sum = 0;
  for (int i = 0; i < 15; i++) sum += pkt[i];
  pkt[15] = (uint8_t) (sum & 0xFF);

  write_packet_(pkt, sizeof(pkt));
}

}  // namespace neewer_component
}  // namespace esphome
