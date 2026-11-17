#include "seplos_bms.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

// Fallback for ESPHome < 2026.10.0
#ifndef ESPHOME_LOG_TAG
#define ESPHOME_LOG_TAG(name, tag) static const char *const name = tag
#endif

namespace esphome::seplos_bms {

ESPHOME_LOG_TAG(TAG, "seplos_bms");

static constexpr size_t MAX_HEX_DUMP_BYTES = 100;

static void log_hex_chunked(const char *tag, const uint8_t *data, size_t size) {
  char buf[format_hex_pretty_size(100)];
  for (size_t i = 0; i < size; i += 100) {
    size_t len = std::min<size_t>(100, size - i);
    ESP_LOGVV(tag, "  %s", format_hex_pretty_to(buf, sizeof(buf), data + i, len, '.'));
  }
}

static const uint8_t MAX_NO_RESPONSE_COUNT = 5;

void SeplosBms::on_seplos_modbus_data(const std::vector<uint8_t> &data) {
  this->reset_online_status_tracker_();

  // num_of_cells   frame_size   data_len
  // 8              65           118 (0x76)   guessed
  // 14             77           142 (0x8E)
  // 15             79           146 (0x92)
  // 16             81           150 (0x96)
  if (this->telemetry_layout_ == TELEMETRY_LAYOUT_RUIXU && this->last_request_function_ == 0x44) {
    this->on_ruixu_alarm_data_(data);
    return;
  }
  if (this->telemetry_layout_ == TELEMETRY_LAYOUT_RUIXU) {
    // Ruixu has no command group byte, so the cell count is byte 7 rather than 8
    const uint8_t cells = data.size() < 44 ? 0 : (this->override_cell_count_ ? this->override_cell_count_ : data[7]);
    if (cells >= 8 && cells <= 16) {
      this->on_ruixu_telemetry_data_(data);
      return;
    }
  } else if (data.size() >= 44 && data[8] >= 8 && data[8] <= 16) {
    this->on_telemetry_data_(data);
    return;
  }

  char hex_buf[format_hex_pretty_size(MAX_HEX_DUMP_BYTES)];
  ESP_LOGW(TAG, "Unhandled data received (data_len: 0x%02X): %s", data[5], format_hex_pretty_to(hex_buf, data, '.'));
}

void SeplosBms::on_telemetry_data_(const std::vector<uint8_t> &data) {
  auto seplos_get_16bit = [&](size_t i) -> uint16_t {
    return (uint16_t(data[i + 0]) << 8) | (uint16_t(data[i + 1]) << 0);
  };

  ESP_LOGI(TAG, "Telemetry frame (%zu bytes) received", data.size());
  log_hex_chunked(TAG, data.data(), data.size());

  // ->
  // 0x2000460010960001100CD70CE90CF40CD60CEF0CE50CE10CDC0CE90CF00CE80CEF0CEA0CDA0CDE0CD8060BA60BA00B970BA60BA50BA2FD5C14A0344E0A426803134650004603E8149F0000000000000000
  // 0x26004600307600011000000000000000000000000000000000000000000000000000000000000000000608530853085308530BAC0B9000000000002D0213880001E6B8
  //
  // *Data*
  //
  // Byte   Address Content: Description                      Decoded content               Coeff./Unit
  //   0    0x20             Protocol version      VER        2.0
  //   1    0x00             Device address        ADR
  //   2    0x46             Device type           CID1       Lithium iron phosphate battery BMS
  //   3    0x00             Function code         CID2       0x00: Normal, 0x01 VER error, 0x02 Chksum error, ...
  //   4    0x10             Data length checksum  LCHKSUM
  //   5    0x96             Data length           LENID      150 / 2 = 75
  //   6      0x00           Data flag
  //   7      0x01           Command group
  ESP_LOGV(TAG, "Command group: %d", data[7]);
  //   8      0x10           Number of cells                  16
  uint8_t cells = (this->override_cell_count_) ? this->override_cell_count_ : data[8];

  ESP_LOGV(TAG, "Number of cells: %d", cells);
  //   9      0x0C 0xD7      Cell voltage 1                   3287 * 0.001f = 3.287         V
  //   11     0x0C 0xE9      Cell voltage 2                   3305 * 0.001f = 3.305         V
  //   ...    ...            ...
  //   39     0x0C 0xD8      Cell voltage 16                                                V
  float min_cell_voltage = 100.0f;
  float max_cell_voltage = -100.0f;
  float average_cell_voltage = 0.0f;
  uint8_t min_voltage_cell = 0;
  uint8_t max_voltage_cell = 0;
  for (uint8_t i = 0; i < std::min((uint8_t) 16, cells); i++) {
    float cell_voltage = (float) seplos_get_16bit(9 + (i * 2)) * 0.001f;
    average_cell_voltage = average_cell_voltage + cell_voltage;
    if (cell_voltage < min_cell_voltage) {
      min_cell_voltage = cell_voltage;
      min_voltage_cell = i + 1;
    }
    if (cell_voltage > max_cell_voltage) {
      max_cell_voltage = cell_voltage;
      max_voltage_cell = i + 1;
    }
    this->publish_state_(this->cells_[i].cell_voltage_sensor_, cell_voltage);
  }
  average_cell_voltage = average_cell_voltage / cells;

  this->publish_state_(this->min_cell_voltage_sensor_, min_cell_voltage);
  this->publish_state_(this->max_cell_voltage_sensor_, max_cell_voltage);
  this->publish_state_(this->max_voltage_cell_sensor_, (float) max_voltage_cell);
  this->publish_state_(this->min_voltage_cell_sensor_, (float) min_voltage_cell);
  this->publish_state_(this->delta_cell_voltage_sensor_, max_cell_voltage - min_cell_voltage);
  this->publish_state_(this->average_cell_voltage_sensor_, average_cell_voltage);

  uint8_t offset = 9 + (cells * 2);

  //   41     0x06           Number of temperatures           6                             V
  uint8_t temperature_sensors = data[offset];
  ESP_LOGV(TAG, "Number of temperature sensors: %d", temperature_sensors);

  //   42     0x0B 0xA6      Temperature sensor 1             (2982 - 2731) * 0.1f = 25.1          °C
  //   44     0x0B 0xA0      Temperature sensor 2             (2976 - 2731) * 0.1f = 24.5          °C
  //   46     0x0B 0x97      Temperature sensor 3             (2967 - 2731) * 0.1f = 23.6          °C
  //   48     0x0B 0xA6      Temperature sensor 4             (2982 - 2731) * 0.1f = 25.1          °C
  //   50     0x0B 0xA5      Environment temperature          (2981 - 2731) * 0.1f = 25.0          °C
  //   52     0x0B 0xA2      Mosfet temperature               (2978 - 2731) * 0.1f = 24.7          °C
  for (uint8_t i = 0; i < std::min((uint8_t) 6, temperature_sensors); i++) {
    float raw_temperature = (float) seplos_get_16bit(offset + 1 + (i * 2));
    this->publish_state_(this->temperatures_[i].temperature_sensor_, (raw_temperature - 2731.0f) * 0.1f);
  }
  offset = offset + 1 + (temperature_sensors * 2);

  //   54     0xFD 0x5C      Charge/discharge current         signed int?                   A
  float current = (float) ((int16_t) seplos_get_16bit(offset)) * 0.01f;
  this->publish_state_(this->current_sensor_, current);

  //   56     0x14 0xA0      Total battery voltage            5280 * 0.01f = 52.80          V
  float total_voltage = (float) seplos_get_16bit(offset + 2) * 0.01f;
  this->publish_state_(this->total_voltage_sensor_, total_voltage);

  float power = total_voltage * current;
  this->publish_state_(this->power_sensor_, power);
  this->publish_state_(this->charging_power_sensor_, std::max(0.0f, power));               // 500W vs 0W -> 500W
  this->publish_state_(this->discharging_power_sensor_, std::abs(std::min(0.0f, power)));  // -500W vs 0W -> 500W

  //   58     0x34 0x4E      Residual capacity                13390 * 0.01f = 133.90        Ah
  this->publish_state_(this->residual_capacity_sensor_, (float) seplos_get_16bit(offset + 4) * 0.01f);

  //   60     0x0A           Custom number                    10
  //   61     0x42 0x68      Battery capacity                 17000 * 0.01f = 170.00        Ah
  this->publish_state_(this->battery_capacity_sensor_, (float) seplos_get_16bit(offset + 7) * 0.01f);

  //   63     0x03 0x13      Stage of charge                  787 * 0.1f = 78.7             %
  this->publish_state_(this->state_of_charge_sensor_, (float) seplos_get_16bit(offset + 9) * 0.1f);

  //   65     0x46 0x50      Rated capacity                   18000 * 0.01f = 180.00        Ah
  this->publish_state_(this->rated_capacity_sensor_, (float) seplos_get_16bit(offset + 11) * 0.01f);

  if (data.size() < offset + 13 + 2) {
    return;
  }

  //   67     0x00 0x46      Number of cycles                 70
  this->publish_state_(this->charging_cycles_sensor_, (float) seplos_get_16bit(offset + 13));

  if (data.size() < offset + 15 + 2) {
    return;
  }

  //   69     0x03 0xE8      State of health                  1000 * 0.1f = 100.0           %
  this->publish_state_(this->state_of_health_sensor_, (float) seplos_get_16bit(offset + 15) * 0.1f);

  if (data.size() < offset + 17 + 2) {
    return;
  }

  //   71     0x14 0x9F      Port voltage                     5279 * 0.01f = 52.79          V
  this->publish_state_(this->port_voltage_sensor_, (float) seplos_get_16bit(offset + 17) * 0.01f);

  //   73     0x00 0x00      Reserved
  //   75     0x00 0x00      Reserved
  //   77     0x00 0x00      Reserved
  //   79     0x00 0x00      Reserved
}

void SeplosBms::dump_config() {
  ESP_LOGCONFIG(TAG, "SeplosBms:");
  ESP_LOGCONFIG(TAG, "  Telemetry layout: %s",
                this->telemetry_layout_ == TELEMETRY_LAYOUT_RUIXU ? "Ruixu" : "Seplos");
  ESP_LOGCONFIG(TAG, "  Alarm/status (0x44) requests: %s", YESNO(this->alarm_data_wanted_()));
  LOG_BINARY_SENSOR("", "Charging MOSFET", this->charging_mosfet_binary_sensor_);
  LOG_BINARY_SENSOR("", "Discharging MOSFET", this->discharging_mosfet_binary_sensor_);
  LOG_BINARY_SENSOR("", "Pre-charge MOSFET", this->precharge_mosfet_binary_sensor_);
  LOG_BINARY_SENSOR("", "Charging", this->charging_binary_sensor_);
  LOG_BINARY_SENSOR("", "Discharging", this->discharging_binary_sensor_);
  LOG_BINARY_SENSOR("", "Heating", this->heating_binary_sensor_);
  LOG_BINARY_SENSOR("", "Fully charged", this->fully_charged_binary_sensor_);
  LOG_BINARY_SENSOR("", "Alarm", this->alarm_binary_sensor_);
  LOG_TEXT_SENSOR("", "Status bytes", this->status_bytes_text_sensor_);
  LOG_SENSOR("", "Minimum Cell Voltage", this->min_cell_voltage_sensor_);
  LOG_SENSOR("", "Maximum Cell Voltage", this->max_cell_voltage_sensor_);
  LOG_SENSOR("", "Minimum Voltage Cell", this->min_voltage_cell_sensor_);
  LOG_SENSOR("", "Maximum Voltage Cell", this->max_voltage_cell_sensor_);
  LOG_SENSOR("", "Delta Cell Voltage", this->delta_cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 1", this->cells_[0].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 2", this->cells_[1].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 3", this->cells_[2].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 4", this->cells_[3].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 5", this->cells_[4].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 6", this->cells_[5].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 7", this->cells_[6].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 8", this->cells_[7].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 9", this->cells_[8].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 10", this->cells_[9].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 11", this->cells_[10].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 12", this->cells_[11].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 13", this->cells_[12].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 14", this->cells_[13].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 15", this->cells_[14].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 16", this->cells_[15].cell_voltage_sensor_);
  LOG_SENSOR("", "Temperature 1", this->temperatures_[0].temperature_sensor_);
  LOG_SENSOR("", "Temperature 2", this->temperatures_[1].temperature_sensor_);
  LOG_SENSOR("", "Temperature 3", this->temperatures_[2].temperature_sensor_);
  LOG_SENSOR("", "Temperature 4", this->temperatures_[3].temperature_sensor_);
  LOG_SENSOR("", "Temperature 5", this->temperatures_[4].temperature_sensor_);
  LOG_SENSOR("", "Temperature 6", this->temperatures_[5].temperature_sensor_);
  LOG_SENSOR("", "Total Voltage", this->total_voltage_sensor_);
  LOG_SENSOR("", "Current", this->current_sensor_);
  LOG_SENSOR("", "Power", this->power_sensor_);
  LOG_SENSOR("", "Charging Power", this->charging_power_sensor_);
  LOG_SENSOR("", "Discharging Power", this->discharging_power_sensor_);
  LOG_SENSOR("", "Charging cycles", this->charging_cycles_sensor_);
  LOG_SENSOR("", "State of charge", this->state_of_charge_sensor_);
  LOG_SENSOR("", "Residual capacity", this->residual_capacity_sensor_);
  LOG_SENSOR("", "Battery capacity", this->battery_capacity_sensor_);
  LOG_SENSOR("", "Rated capacity", this->rated_capacity_sensor_);
  LOG_SENSOR("", "Average Cell Voltage", this->average_cell_voltage_sensor_);
  LOG_SENSOR("", "State of health", this->state_of_health_sensor_);
  LOG_SENSOR("", "Port Voltage", this->port_voltage_sensor_);
}

float SeplosBms::get_setup_priority() const {
  // After UART bus
  return setup_priority::BUS - 1.0f;
}

// Ruixu packs (e.g. 314 Ah 16S) answer the 0x42 request with a Pylontech-style frame that
// differs from Seplos: no command group byte, current in 0.1 A, no rated capacity, and
// SOC/SOH as single whole-percent bytes. Captured from a Ruixu pack (address 0x00):
//
// ~21004600808000 10 0CE0...0CDE 07 0BAE0BB20BAF0BB00BC60BB80BC0 0000 1491 48A7 04 7AA8 0001 3B 64 0000 E056
//
// Byte   Content                       Example     Decoded
//   0    Protocol version              0x21
//   1    Device address                0x00
//   2    Device type (CID1)            0x46
//   3    Return code (CID2)            0x00        0x00: Normal
//   4    Length checksum + length      0x80 0x80   128 ASCII chars = 64 data bytes
//   6    Data flag                     0x00
//   7    Number of cells               0x10        16
//   8    Cell voltage 1..n             0x0C 0xE0   3296 * 0.001 = 3.296 V
//   o    Number of temperatures        0x07        7 (the first 6 are published)
//   o+1  Temperature 1..n              0x0B 0xAE   (2990 - 2731) * 0.1 = 25.9 C
//   t    Current                       0x00 0x00   signed, * 0.1 A (charging is positive)
//   t+2  Total voltage                 0x14 0x91   5265 * 0.01 = 52.65 V
//   t+4  Residual capacity             0x48 0xA7   18599 * 0.01 = 185.99 Ah
//   t+6  Custom number                 0x04
//   t+7  Battery capacity              0x7A 0xA8   31400 * 0.01 = 314.00 Ah
//   t+9  Number of cycles              0x00 0x01   1
//   t+11 State of charge               0x3B        59 %
//   t+12 State of health               0x64        100 %
//   t+13 Reserved                      0x00 0x00
void SeplosBms::on_ruixu_telemetry_data_(const std::vector<uint8_t> &data) {
  auto get_16bit = [&](size_t i) -> uint16_t { return (uint16_t(data[i + 0]) << 8) | (uint16_t(data[i + 1]) << 0); };

  ESP_LOGI(TAG, "Ruixu telemetry frame (%zu bytes) received", data.size());
  log_hex_chunked(TAG, data.data(), data.size());

  uint8_t cells = (this->override_cell_count_) ? this->override_cell_count_ : data[7];
  size_t offset = 8 + (cells * 2);
  if (data.size() < offset + 1) {
    ESP_LOGW(TAG, "Ruixu telemetry frame too short (%zu bytes) for %u cells", data.size(), cells);
    return;
  }
  uint8_t temperature_sensors = data[offset];
  size_t tail = offset + 1 + (temperature_sensors * 2);
  if (data.size() < tail + 13) {
    ESP_LOGW(TAG, "Ruixu telemetry frame too short (%zu bytes) for %u cells and %u temperatures", data.size(), cells,
             temperature_sensors);
    return;
  }

  float min_cell_voltage = 100.0f;
  float max_cell_voltage = -100.0f;
  float average_cell_voltage = 0.0f;
  uint8_t min_voltage_cell = 0;
  uint8_t max_voltage_cell = 0;
  uint8_t published_cells = std::min((uint8_t) 16, cells);
  for (uint8_t i = 0; i < published_cells; i++) {
    float cell_voltage = (float) get_16bit(8 + (i * 2)) * 0.001f;
    average_cell_voltage += cell_voltage;
    if (cell_voltage < min_cell_voltage) {
      min_cell_voltage = cell_voltage;
      min_voltage_cell = i + 1;
    }
    if (cell_voltage > max_cell_voltage) {
      max_cell_voltage = cell_voltage;
      max_voltage_cell = i + 1;
    }
    this->publish_state_(this->cells_[i].cell_voltage_sensor_, cell_voltage);
  }
  if (published_cells > 0) {
    average_cell_voltage = average_cell_voltage / published_cells;
    this->publish_state_(this->min_cell_voltage_sensor_, min_cell_voltage);
    this->publish_state_(this->max_cell_voltage_sensor_, max_cell_voltage);
    this->publish_state_(this->max_voltage_cell_sensor_, (float) max_voltage_cell);
    this->publish_state_(this->min_voltage_cell_sensor_, (float) min_voltage_cell);
    this->publish_state_(this->delta_cell_voltage_sensor_, max_cell_voltage - min_cell_voltage);
    this->publish_state_(this->average_cell_voltage_sensor_, average_cell_voltage);
  }

  ESP_LOGV(TAG, "Number of temperature sensors: %d", temperature_sensors);
  for (uint8_t i = 0; i < std::min((uint8_t) 6, temperature_sensors); i++) {
    float raw_temperature = (float) get_16bit(offset + 1 + (i * 2));
    this->publish_state_(this->temperatures_[i].temperature_sensor_, (raw_temperature - 2731.0f) * 0.1f);
  }

  float current = (float) ((int16_t) get_16bit(tail)) * 0.1f;
  this->publish_state_(this->current_sensor_, current);

  float total_voltage = (float) get_16bit(tail + 2) * 0.01f;
  this->publish_state_(this->total_voltage_sensor_, total_voltage);

  float power = total_voltage * current;
  this->publish_state_(this->power_sensor_, power);
  this->publish_state_(this->charging_power_sensor_, std::max(0.0f, power));
  this->publish_state_(this->discharging_power_sensor_, std::abs(std::min(0.0f, power)));

  this->publish_state_(this->residual_capacity_sensor_, (float) get_16bit(tail + 4) * 0.01f);
  this->publish_state_(this->battery_capacity_sensor_, (float) get_16bit(tail + 7) * 0.01f);
  this->publish_state_(this->charging_cycles_sensor_, (float) get_16bit(tail + 9));
  this->publish_state_(this->state_of_charge_sensor_, (float) data[tail + 11]);
  this->publish_state_(this->state_of_health_sensor_, (float) data[tail + 12]);
}

void SeplosBms::update() {
  this->track_online_status_();
  this->send(0x42, this->pack_);
  if (this->alarm_data_wanted_())
    this->send(0x44, this->pack_);
}

bool SeplosBms::alarm_data_wanted_() const {
  return this->telemetry_layout_ == TELEMETRY_LAYOUT_RUIXU &&
         (this->charging_mosfet_binary_sensor_ != nullptr || this->discharging_mosfet_binary_sensor_ != nullptr ||
          this->precharge_mosfet_binary_sensor_ != nullptr || this->charging_binary_sensor_ != nullptr ||
          this->discharging_binary_sensor_ != nullptr || this->heating_binary_sensor_ != nullptr ||
          this->fully_charged_binary_sensor_ != nullptr || this->alarm_binary_sensor_ != nullptr ||
          this->errors_text_sensor_ != nullptr || this->status_bytes_text_sensor_ != nullptr);
}

// Ruixu reply to 0x44 (alarm/status). Captured from an idle pack (current 0 A, SOC 59 %):
//
// ~21004600204A00 10 00x16 07 00x7 00 00 08 00 06 00 00 00 00 00 00 EFA6
//
// Byte  Content                                  Example
//   6   Data flag                                0x00
//   7   Number of cells M                        0x10
//   8   Cell states, M bytes                     0x00 (00 normal, 01 below limit, 02 above limit, F0 other)
//   o   Number of temperatures N, then N states  0x07, 0x00 x7
//   t   Current state, pack voltage state        0x00 0x00
//  t+2  Number of status bytes P                 0x08
//  t+3  Status 1..5, then P-5 unknown bytes      0x00 0x06 0x00 0x00 0x00 0x00 0x00 0x00
//
// Status bit meanings are Pylontech RS485 V3.3 (section 3.5). The layout matches the capture; the bit
// meanings are inferred until confirmed in each state, so the raw bytes are also published.
void SeplosBms::on_ruixu_alarm_data_(const std::vector<uint8_t> &data) {
  ESP_LOGD(TAG, "Ruixu alarm frame (%zu bytes) received", data.size());
  log_hex_chunked(TAG, data.data(), data.size());

  if (data.size() < 9) {
    ESP_LOGW(TAG, "Ruixu alarm frame too short (%zu bytes)", data.size());
    return;
  }
  const uint8_t cells = data[7];
  const size_t offset = 8 + cells;
  if (cells > 16 || data.size() < offset + 1) {
    ESP_LOGW(TAG, "Ruixu alarm frame has %u cells in %zu bytes", cells, data.size());
    return;
  }
  const uint8_t temperatures = data[offset];
  const size_t states = offset + 1 + temperatures;  // current state, voltage state, P
  if (temperatures > 16 || data.size() < states + 3) {
    ESP_LOGW(TAG, "Ruixu alarm frame too short (%zu bytes) for %u temperatures", data.size(), temperatures);
    return;
  }
  const uint8_t current_state = data[states];
  const uint8_t voltage_state = data[states + 1];
  const uint8_t status_count = data[states + 2];
  const size_t status = states + 3;
  if (status_count < 5 || data.size() < status + status_count) {
    ESP_LOGW(TAG, "Ruixu alarm frame: %u status bytes in %zu bytes", status_count, data.size());
    return;
  }
  const uint8_t s1 = data[status];
  const uint8_t s2 = data[status + 1];
  const uint8_t s3 = data[status + 2];
  const uint8_t s4 = data[status + 3];
  const uint8_t s5 = data[status + 4];

  // Status 2: bit2 discharge MOSFET, bit1 charge MOSFET, bit0 pre-charge MOSFET
  this->publish_state_(this->discharging_mosfet_binary_sensor_, (s2 & 0x04) != 0);
  this->publish_state_(this->charging_mosfet_binary_sensor_, (s2 & 0x02) != 0);
  this->publish_state_(this->precharge_mosfet_binary_sensor_, (s2 & 0x01) != 0);
  // Status 3: bit7 charge current, bit6 discharge current, bit5 heater, bit3 fully charged
  this->publish_state_(this->charging_binary_sensor_, (s3 & 0x80) != 0);
  this->publish_state_(this->discharging_binary_sensor_, (s3 & 0x40) != 0);
  this->publish_state_(this->heating_binary_sensor_, (s3 & 0x20) != 0);
  this->publish_state_(this->fully_charged_binary_sensor_, (s3 & 0x08) != 0);

  std::string alarms;
  auto add = [&alarms](const char *text) {
    if (!alarms.empty())
      alarms += "; ";
    alarms += text;
  };
  auto state_name = [](uint8_t state) -> const char * {
    switch (state) {
      case 0x01:
        return "low";
      case 0x02:
        return "high";
      default:
        return "fault";
    }
  };
  char buf[48];
  for (uint8_t i = 0; i < cells; i++) {
    if (data[8 + i] != 0x00) {
      snprintf(buf, sizeof(buf), "Cell %u %s", i + 1, state_name(data[8 + i]));
      add(buf);
    }
  }
  for (uint8_t i = 0; i < temperatures; i++) {
    if (data[offset + 1 + i] != 0x00) {
      snprintf(buf, sizeof(buf), "Temperature %u %s", i + 1, state_name(data[offset + 1 + i]));
      add(buf);
    }
  }
  if (current_state != 0x00) {
    snprintf(buf, sizeof(buf), "Current %s", state_name(current_state));
    add(buf);
  }
  if (voltage_state != 0x00) {
    snprintf(buf, sizeof(buf), "Pack voltage %s", state_name(voltage_state));
    add(buf);
  }
  static const char *const STATUS1[8] = {"Pack overvoltage",       "Cell undervoltage",     "Charge overcurrent",
                                         nullptr,                  "Discharge overcurrent", "Discharge overtemperature",
                                         "Charge overtemperature", "Pack undervoltage"};
  for (uint8_t bit = 0; bit < 8; bit++) {
    if ((s1 & (1 << bit)) && STATUS1[bit] != nullptr)
      add(STATUS1[bit]);
  }
  for (uint8_t bit = 0; bit < 8; bit++) {
    if (s4 & (1 << bit)) {
      snprintf(buf, sizeof(buf), "Cell %u voltage error", bit + 1);
      add(buf);
    }
    if (s5 & (1 << bit)) {
      snprintf(buf, sizeof(buf), "Cell %u voltage error", bit + 9);
      add(buf);
    }
  }
  this->publish_state_(this->alarm_binary_sensor_, !alarms.empty());
  this->publish_state_(this->errors_text_sensor_, alarms.empty() ? std::string("No alarms") : alarms);

  // Raw status for confirming the bit meanings: the P status bytes, then current and voltage state
  std::string raw;
  for (uint8_t i = 0; i < status_count; i++) {
    snprintf(buf, sizeof(buf), i ? " %02X" : "%02X", data[status + i]);
    raw += buf;
  }
  snprintf(buf, sizeof(buf), " | cur %02X volt %02X", current_state, voltage_state);
  raw += buf;
  this->publish_state_(this->status_bytes_text_sensor_, raw);
}

void SeplosBms::publish_state_(binary_sensor::BinarySensor *binary_sensor, const bool &state) {
  if (binary_sensor == nullptr)
    return;

  binary_sensor->publish_state(state);
}

void SeplosBms::publish_state_(sensor::Sensor *sensor, float value) {
  if (sensor == nullptr)
    return;

  sensor->publish_state(value);
}

void SeplosBms::publish_state_(text_sensor::TextSensor *text_sensor, const std::string &state) {
  if (text_sensor == nullptr)
    return;

  text_sensor->publish_state(state);
}

void SeplosBms::track_online_status_() {
  if (this->no_response_count_ < MAX_NO_RESPONSE_COUNT) {
    this->no_response_count_++;
  }
  if (this->no_response_count_ == MAX_NO_RESPONSE_COUNT) {
    this->publish_device_unavailable_();
    this->no_response_count_++;
  }
}

void SeplosBms::reset_online_status_tracker_() {
  this->no_response_count_ = 0;
  this->publish_state_(this->online_status_binary_sensor_, true);
}

void SeplosBms::publish_device_unavailable_() {
  this->publish_state_(this->online_status_binary_sensor_, false);
  this->publish_state_(this->errors_text_sensor_, "Offline");

  this->publish_state_(this->min_cell_voltage_sensor_, NAN);
  this->publish_state_(this->max_cell_voltage_sensor_, NAN);
  this->publish_state_(this->min_voltage_cell_sensor_, NAN);
  this->publish_state_(this->max_voltage_cell_sensor_, NAN);
  this->publish_state_(this->delta_cell_voltage_sensor_, NAN);
  this->publish_state_(this->average_cell_voltage_sensor_, NAN);
  this->publish_state_(this->total_voltage_sensor_, NAN);
  this->publish_state_(this->current_sensor_, NAN);
  this->publish_state_(this->power_sensor_, NAN);
  this->publish_state_(this->charging_power_sensor_, NAN);
  this->publish_state_(this->discharging_power_sensor_, NAN);
  this->publish_state_(this->state_of_charge_sensor_, NAN);
  this->publish_state_(this->residual_capacity_sensor_, NAN);
  this->publish_state_(this->battery_capacity_sensor_, NAN);
  this->publish_state_(this->rated_capacity_sensor_, NAN);
  this->publish_state_(this->charging_cycles_sensor_, NAN);
  this->publish_state_(this->state_of_health_sensor_, NAN);
  this->publish_state_(this->port_voltage_sensor_, NAN);

  for (auto &temperature : this->temperatures_) {
    this->publish_state_(temperature.temperature_sensor_, NAN);
  }

  for (auto &cell : this->cells_) {
    this->publish_state_(cell.cell_voltage_sensor_, NAN);
  }
}

}  // namespace esphome::seplos_bms
