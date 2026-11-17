#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"

namespace esphome::seplos_modbus {

class SeplosModbusDevice;

class SeplosModbus : public uart::UARTDevice, public Component {
 public:
  SeplosModbus() = default;

  void setup() override;

  void loop() override;

  void dump_config() override;

  void register_device(SeplosModbusDevice *device) { this->devices_.push_back(device); }

  float get_setup_priority() const override;

  void send(uint8_t protocol_version, uint8_t address, uint8_t function, uint8_t value);
  void set_rx_timeout(uint16_t rx_timeout) { rx_timeout_ = rx_timeout; }
  void set_flow_control_pin(GPIOPin *flow_control_pin) { this->flow_control_pin_ = flow_control_pin; }
  void set_response_timeout(uint16_t response_timeout) { response_timeout_ = response_timeout; }
  void set_request_gap(uint16_t request_gap) { request_gap_ = request_gap; }

 protected:
  uint16_t rx_timeout_{150};
  GPIOPin *flow_control_pin_{nullptr};

  bool parse_seplos_modbus_byte_(uint8_t byte);
  std::vector<uint8_t> rx_buffer_;
  uint32_t last_seplos_modbus_byte_{0};
  uint32_t last_send_{0};
  std::vector<SeplosModbusDevice *> devices_;

  // Requests are sent one at a time: the next one waits for the reply (or response_timeout_)
  // plus request_gap_. Two packs polled back to back (23 ms apart) both failed on a Ruixu bus.
  struct Request {
    uint8_t protocol_version;
    uint8_t address;
    uint8_t function;
    uint8_t value;
  };
  static constexpr size_t MAX_QUEUED_REQUESTS = 16;
  std::vector<Request> queue_;
  bool waiting_{false};
  uint8_t waiting_address_{0};
  uint32_t bus_free_since_{0};
  uint16_t response_timeout_{500};
  uint16_t request_gap_{50};
  void process_queue_();
  void transmit_(const Request &request);
};

uint16_t crc16(const uint8_t *data, uint8_t len);

class SeplosModbusDevice {
 public:
  void set_parent(SeplosModbus *parent) { parent_ = parent; }
  void set_address(uint8_t address) { address_ = address; }
  void set_pack(uint8_t pack) { pack_ = pack; }
  void set_protocol_version(uint8_t protocol_version) { protocol_version_ = protocol_version; }
  virtual void on_seplos_modbus_data(const std::vector<uint8_t> &data) = 0;
  void send(uint8_t function, uint8_t value) {
    this->parent_->send(this->protocol_version_, this->address_, function, value);
  }

 protected:
  friend SeplosModbus;

  SeplosModbus *parent_;
  uint8_t address_;
  uint8_t pack_;
  uint8_t protocol_version_;
};

}  // namespace esphome::seplos_modbus
