#include "seplos_modbus.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

// Fallback for ESPHome < 2026.10.0
#ifndef ESPHOME_LOG_TAG
#define ESPHOME_LOG_TAG(name, tag) static const char *const name = tag
#endif

namespace esphome::seplos_modbus {

ESPHOME_LOG_TAG(TAG, "seplos_modbus");

static constexpr size_t MAX_HEX_DUMP_BYTES = 100;

static const uint16_t MAX_RESPONSE_SIZE = 340;

// Return codes (RTN, byte 3 of a reply). 0x01-0x06 and 0x90/0x91 are in the Pylontech RS485 protocol
// V3.3 (2018) and Seplos V2.0; 0x07 and 0xE1-0xE4 are Seplos-only, 0x09 is PACE-only.
static const char *return_code_to_string(uint8_t rtn) {
  switch (rtn) {
    case 0x00:
      return "normal";
    case 0x01:
      return "VER error";
    case 0x02:
      return "CHKSUM error (frame checksum)";
    case 0x03:
      return "LCHKSUM error (length checksum), request arrived garbled";
    case 0x04:
      return "CID2 invalid (unknown command)";
    case 0x05:
      return "command format error";
    case 0x06:
      return "invalid data";
    case 0x07:
      return "no data";
    case 0x09:
      return "operation or write error";
    case 0x90:
      return "ADR error";
    case 0x91:
      return "internal communication error (between packs)";
    case 0xE1:
      return "CID1 invalid";
    case 0xE2:
      return "command execution failed";
    case 0xE3:
      return "device fault";
    case 0xE4:
      return "invalid permission";
    default:
      return "undocumented";
  }
}

void SeplosModbus::setup() {
  if (this->flow_control_pin_ != nullptr) {
    this->flow_control_pin_->setup();
  }
}
void SeplosModbus::loop() {
  const uint32_t now = millis();

  if (now - this->last_seplos_modbus_byte_ > this->rx_timeout_) {
    char hex_buf[format_hex_pretty_size(MAX_HEX_DUMP_BYTES)];
    ESP_LOGVV(TAG, "Buffer cleared due to timeout: %s", format_hex_pretty_to(hex_buf, this->rx_buffer_, '.'));
    this->rx_buffer_.clear();
    this->last_seplos_modbus_byte_ = now;
  }

  while (this->available()) {
    uint8_t byte;
    this->read_byte(&byte);
    if (this->parse_seplos_modbus_byte_(byte)) {
      this->last_seplos_modbus_byte_ = now;
    } else {
      char hex_buf[format_hex_pretty_size(MAX_HEX_DUMP_BYTES)];
      ESP_LOGVV(TAG, "Buffer cleared due to reset: %s", format_hex_pretty_to(hex_buf, this->rx_buffer_, '.'));
      this->rx_buffer_.clear();
    }
  }

  this->process_queue_();
}

void SeplosModbus::process_queue_() {
  const uint32_t now = millis();
  if (this->waiting_) {
    if (now - this->last_send_ < this->response_timeout_)
      return;
    ESP_LOGD(TAG, "No reply from 0x%02X within %u ms", this->waiting_address_, this->response_timeout_);
    this->waiting_ = false;
    this->bus_free_since_ = now;
  }
  if (this->queue_.empty() || now - this->bus_free_since_ < this->request_gap_)
    return;

  const Request request = this->queue_.front();
  this->queue_.erase(this->queue_.begin());
  this->transmit_(request);
}

uint16_t chksum(const uint8_t data[], const uint16_t len) {
  uint16_t checksum = 0x00;
  for (uint16_t i = 0; i < len; i++) {
    checksum = checksum + data[i];
  }
  checksum = ~checksum;
  checksum += 1;
  return checksum;
}

uint16_t lchksum(const uint16_t len) {
  uint16_t lchecksum = 0x0000;

  if (len == 0)
    return 0x0000;

  lchecksum = (len & 0xf) + ((len >> 4) & 0xf) + ((len >> 8) & 0xf);
  lchecksum = ~(lchecksum % 16) + 1;

  return (lchecksum << 12) + len;  // 4 byte checksum + 12 bytes length
}

uint8_t ascii_hex_to_byte(char a, char b) {
  a = (a <= '9') ? a - '0' : (a & 0x7) + 9;
  b = (b <= '9') ? b - '0' : (b & 0x7) + 9;

  return (a << 4) + b;
}

static char byte_to_ascii_hex(uint8_t v) { return v >= 10 ? 'A' + (v - 10) : '0' + v; }
std::string byte_to_ascii_hex(const uint8_t *data, size_t length) {
  if (length == 0)
    return "";
  std::string ret;
  ret.resize(2 * length);
  for (size_t i = 0; i < length; i++) {
    ret[2 * i] = byte_to_ascii_hex((data[i] & 0xF0) >> 4);
    ret[2 * i + 1] = byte_to_ascii_hex(data[i] & 0x0F);
  }
  return ret;
}

bool SeplosModbus::parse_seplos_modbus_byte_(uint8_t byte) {
  size_t at = this->rx_buffer_.size();
  this->rx_buffer_.push_back(byte);
  const uint8_t *raw = &this->rx_buffer_[0];

  // Start of frame
  if (at == 0) {
    if (raw[0] != 0x7E) {
      // Ruixu packs share the RS485 pair with their own binary 0xEF..0x16 frames,
      // so stray bytes are normal there: log them at the most verbose level only.
      ESP_LOGVV(TAG, "Invalid header: 0x%02X", raw[0]);

      // return false to reset buffer
      return false;
    }

    return true;
  }

  // End of frame '\r'
  if (raw[at] != 0x0D)
    return true;

  if (at > MAX_RESPONSE_SIZE) {
    ESP_LOGW(TAG, "Maximum response size exceeded. Flushing RX buffer...");
    return false;
  }

  // Smallest frame we can use: '~', VER ADR CID1 RTN as 8 ASCII chars, 4 checksum chars, CR (at == 13).
  // Shorter ones (e.g. a stray 0x7E followed by 0x0D within a few bytes) would underflow data_len,
  // and the address (byte 1) and return code (byte 3) must be present.
  if (at < 13) {
    ESP_LOGVV(TAG, "Frame too short (%zu bytes), dropped", at + 1);
    return false;
  }

  uint16_t data_len = at - 4 - 1;
  uint16_t computed_crc = chksum(raw + 1, data_len);
  uint16_t remote_crc = uint16_t(ascii_hex_to_byte(raw[at - 4], raw[at - 3])) << 8 |
                        (uint16_t(ascii_hex_to_byte(raw[at - 2], raw[at - 1])) << 0);
  if (computed_crc != remote_crc) {
    ESP_LOGW(TAG, "CRC check failed! 0x%04X != 0x%04X", computed_crc, remote_crc);
    return false;
  }

  std::vector<uint8_t> data;
  for (uint16_t i = 1; i < data_len; i = i + 2) {
    data.push_back(ascii_hex_to_byte(raw[i], raw[i + 1]));
  }

  uint8_t address = data[1];
  const uint8_t return_code = data[3];

  if (return_code != 0x00) {
    // An error reply carries no data. It may not come from the addressed pack (a garbled request
    // to a secondary pack was answered by the first pack), so report it against the outstanding request.
    if (this->waiting_) {
      ESP_LOGW(TAG, "Request 0x%02X to 0x%02X failed: 0x%02X answered with error 0x%02X (%s)", this->waiting_function_,
               this->waiting_address_, address, return_code, return_code_to_string(return_code));
      this->waiting_ = false;
      this->bus_free_since_ = millis();
    } else {
      ESP_LOGW(TAG, "Unrequested error reply from 0x%02X: 0x%02X (%s)", address, return_code,
               return_code_to_string(return_code));
    }
    return false;
  }

  uint8_t request_function = 0;
  if (this->waiting_ && address == this->waiting_address_) {
    request_function = this->waiting_function_;
    this->waiting_ = false;
    this->bus_free_since_ = millis();
  }

  bool found = false;
  for (auto *device : this->devices_) {
    if (device->address_ == address) {
      device->last_request_function_ = request_function;
      device->on_seplos_modbus_data(data);
      found = true;
    }
  }

  if (!found) {
    ESP_LOGW(TAG, "Got SeplosModbus frame from unknown address 0x%02X! ", address);
  }

  // return false to reset buffer
  return false;
}

void SeplosModbus::dump_config() {
  ESP_LOGCONFIG(TAG, "SeplosModbus:");
  LOG_PIN("  Flow Control Pin: ", this->flow_control_pin_);
  ESP_LOGCONFIG(TAG, "  RX timeout: %d ms", this->rx_timeout_);
  ESP_LOGCONFIG(TAG, "  Response timeout: %u ms", this->response_timeout_);
  ESP_LOGCONFIG(TAG, "  Request gap: %u ms", this->request_gap_);
}
float SeplosModbus::get_setup_priority() const {
  // After UART bus
  return setup_priority::BUS - 1.0f;
}

void SeplosModbus::send(uint8_t protocol_version, uint8_t address, uint8_t function, uint8_t value) {
  for (const auto &queued : this->queue_) {
    if (queued.protocol_version == protocol_version && queued.address == address && queued.function == function &&
        queued.value == value) {
      ESP_LOGV(TAG, "Request 0x%02X to 0x%02X already queued", function, address);
      return;
    }
  }
  if (this->queue_.size() >= MAX_QUEUED_REQUESTS) {
    ESP_LOGW(TAG, "Request queue full, dropping request 0x%02X to 0x%02X", function, address);
    return;
  }
  this->queue_.push_back(Request{protocol_version, address, function, value});
  this->process_queue_();
}

void SeplosModbus::transmit_(const Request &request) {
  const uint8_t protocol_version = request.protocol_version;
  const uint8_t address = request.address;
  const uint8_t function = request.function;
  const uint8_t value = request.value;
  if (this->flow_control_pin_ != nullptr)
    this->flow_control_pin_->digital_write(true);

  const uint16_t lenid = lchksum(1 * 2);
  std::vector<uint8_t> data;
  data.push_back(protocol_version);  // VER
  data.push_back(address);           // ADDR
  data.push_back(0x46);              // CID1
  data.push_back(function);          // CID2 (0x42)
  data.push_back(lenid >> 8);        // LCHKSUM (0xE0)
  data.push_back(lenid >> 0);        // LENGTH (0x02)
  data.push_back(value);             // VALUE (0x00)

  const uint16_t frame_len = data.size();
  std::string payload = "~";  // SOF (0x7E)
  payload.append(byte_to_ascii_hex(data.data(), frame_len));

  uint16_t crc = chksum((const uint8_t *) payload.data() + 1, payload.size() - 1);
  data.push_back(crc >> 8);  // CHKSUM (0xFD)
  data.push_back(crc >> 0);  // CHKSUM (0x37)

  payload.append(byte_to_ascii_hex(data.data() + frame_len, data.size() - frame_len));  // Append checksum
  payload.append("\r");                                                                 // EOF (0x0D)

  ESP_LOGD(TAG, "Send frame: %s", payload.c_str());

  this->write_str(payload.c_str());
  this->flush();

  if (this->flow_control_pin_ != nullptr)
    this->flow_control_pin_->digital_write(false);

  this->waiting_ = true;
  this->waiting_address_ = address;
  this->waiting_function_ = function;
  this->last_send_ = millis();
}

}  // namespace esphome::seplos_modbus
