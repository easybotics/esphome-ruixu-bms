# Ruixu changes

This fork of [syssi/esphome-seplos-bms](https://github.com/syssi/esphome-seplos-bms) reads Ruixu battery packs
(for example the 314 Ah 16S rack packs) over RS485. Ruixu answers the same Pylontech-style 0x42 request as Seplos,
with a slightly different frame, so the component decodes it as its own layout.

Example YAML:
* One pack: [esp32-s3-ruixu-example.yaml](esp32-s3-ruixu-example.yaml)
* Four packs, with bank totals: [esp32-s3-ruixu-multiple-example.yaml](esp32-s3-ruixu-multiple-example.yaml)

## Requirements

* ESPHome 2026.1.0 or newer (the upstream component's minimum).
* Pin the component to a release tag in your YAML, for example
  `github://easybotics/esphome-ruixu-bms@ruixu-v2.0.0`, so a later change here can't alter your device on the next build.

## How the Ruixu frame differs from Seplos

`telemetry_layout` on each `seplos_bms` entry picks the decoder: `auto` (default), `seplos` or `ruixu`. `auto` uses the
Ruixu layout when `protocol_version` is `0x21`, so configs from the 2024 version need no change.

| Field | Seplos | Ruixu |
|---|---|---|
| Command group byte before the cell count | yes | no |
| Current | 0.01 A | 0.1 A |
| State of charge / health | 16 bit, 0.1 % | 1 byte, whole % |
| Rated capacity | yes | not sent |
| Temperatures | 6 | 7 (the first 6 are published) |

The full byte map, from a captured Ruixu response, is in the comment above `on_ruixu_telemetry_data_()` in
[seplos_bms.cpp](components/seplos_bms/seplos_bms.cpp). Values were checked against an EG4 FlexBOSS21 reading the same
three-pack bank over CAN: current, voltage, state of charge, state of health and cycle count agree.

## Updating from the 2024 version (2.0.0, November 2026)

* Two years of upstream changes merged in. Each pack now has an `online_status` binary sensor, and a pack that misses
  5 polls in a row shows its values as unknown instead of keeping the last ones.
* The examples:
  * pin the component to a release tag;
  * save the daily energy counters every 15 minutes instead of every 24 hours, and zero them when the ESP boots on a
    later day than they were saved. Before, a reboot after a crash or power loss could carry old totals into the day;
  * add uptime, WiFi signal and reset reason sensors;
  * the four-pack example declared `bms2` twice (it did not compile), and the single-pack example did not parse (its
    sensor block was not indented) and had no `time:` component, which the daily energy sensors need.
* Keep the `name:` of every sensor when you update your YAML. ESPHome builds each entity's unique ID from its name, so
  Home Assistant keeps the same entities, history and dashboards.

Hardware used:
* M5Stack ESP32 S3 Lite: https://docs.m5stack.com/en/core/AtomS3%20Lite
* Same on Digikey: https://www.digikey.com/en/products/detail/m5stack-technology-co-ltd/c124/18070571

* M5Stack RS485 Isolated Transceiver: https://docs.m5stack.com/en/unit/iso485
* Same on Digikey: https://www.digikey.com/en/products/detail/m5stack-technology-co-ltd/u094/13913913

![image](https://github.com/user-attachments/assets/0db5d84c-af54-481e-a879-0a90579338a6)

```
type: entities
entities:
  - entity: sensor.bms_batt_3_state_of_charge
    name: SOC %
    state_header: batt 3
    type: custom:multiple-entity-row
    styles:
      width: 40px
    hide: true
    unit: false
    entities:
      - entity: sensor.bms_batt_0_state_of_charge
        name: batt 0
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_1_state_of_charge
        name: batt 1
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_2_state_of_charge
        name: batt 2
        unit: false
        styles:
          width: 40px
  - entity: sensor.bms_batt_3_voltage_total
    name: Voltage Total V
    state_header: batt 3
    type: custom:multiple-entity-row
    styles:
      width: 40px
    unit: false
    entities:
      - entity: sensor.bms_batt_0_voltage_total
        name: batt 0
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_1_voltage_total
        name: batt 1
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_2_voltage_total
        name: batt 2
        unit: false
        styles:
          width: 40px
  - entity: sensor.bms_batt_3_power_net
    name: Power kW
    state_header: batt 3
    type: custom:multiple-entity-row
    styles:
      width: 40px
    unit: false
    entities:
      - entity: sensor.bms_bank_power_net
        name: total
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_0_power_net
        name: batt 0
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_1_power_net
        name: batt 1
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_2_power_net
        name: batt 2
        unit: false
        styles:
          width: 40px
  - entity: sensor.bms_batt_3_current
    name: Current A
    state_header: batt 3
    type: custom:multiple-entity-row
    styles:
      width: 40px
    unit: false
    entities:
      - entity: sensor.bms_bank_current
        name: total
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_0_current
        name: batt 0
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_1_current
        name: batt 1
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_2_current
        name: batt 2
        unit: false
        styles:
          width: 40px
  - entity: sensor.bms_batt_3_voltage_cell_max
    name: Cell Max V
    state_header: batt 3
    type: custom:multiple-entity-row
    styles:
      width: 40px
    unit: false
    entities:
      - entity: sensor.bms_bank_voltage_cell_max
        name: max
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_0_voltage_cell_max
        name: batt 0
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_1_voltage_cell_max
        name: batt 1
        unit: false
        styles:
          width: 40px
      - entity: sensor.bms_batt_2_voltage_cell_max
        name: batt 2
        unit: false
        styles:
          width: 40px
```

![Screenshot 2024-09-19 114359](https://github.com/user-attachments/assets/ea822be3-417e-490c-8183-b6bbc3ccd5fa)
![Screenshot 2024-09-20 082150](https://github.com/user-attachments/assets/6daf8db2-ffa2-4d8c-a2d0-6f54777ec8dc)
![Screenshot 2024-09-20 082053](https://github.com/user-attachments/assets/2ba57e48-41db-478d-aec3-f01ff8b5f23f)

Connect to the LINK OUT port only - I used pins 7 / 8 - connect the green GND pair to the RS485 GND connection point

![Screenshot 2024-09-20 100034](https://github.com/user-attachments/assets/95d44375-aad9-4864-b88f-39d03d8e03a1)


Connect the two middle CAN pair (blue) to your inverter BMS CAN port - keep the Green pair connected


![Screenshot 2024-09-20 095856](https://github.com/user-attachments/assets/2d0cd401-c811-4fda-9d35-396ce6ae4522)
White Brown: A

Brown: B

Green Pair: GND

![PXL_20240919_003338818 (Medium)](https://github.com/user-attachments/assets/2c440124-a615-4b3d-b4ce-2dbe3245321c)


![PXL_20240919_003403359 (Medium)](https://github.com/user-attachments/assets/7986ef5f-b4ff-45ed-808d-0f87c804f068)

Cell voltage Max / Min getting closer over multiple cycles :) showing the balancing is doing something! 

![image](https://github.com/user-attachments/assets/8b6b54d3-a771-4289-ad78-cf58c770635a)








# esphome-seplos-bms

ESPHome component to monitor a Seplos BMS via UART, RS485 or BLE

## Supported devices

* 1101-SP05 (reported by [@JacquesdeBruyn](https://github.com/syssi/esphome-seplos-bms/issues/37))
* 1101-SP15 (reported by [@NosIreland](https://github.com/syssi/esphome-seplos-bms/issues/1))
* 1101-SP16 (reported by [@atze09](https://github.com/syssi/esphome-seplos-bms/issues/28))
* 1101-ZH26 (reported by [@faizan-elite](https://github.com/syssi/esphome-seplos-bms/issues/2))
* 1101-MZ02 (reported by [@fajera81](https://github.com/syssi/esphome-seplos-bms/discussions/33))
* 1101-SP76 (reported by [@bagges](https://github.com/syssi/esphome-seplos-bms/issues/46))
* 1101-SP101, PUSUNG-135 (reported by [@manznOnly](https://github.com/syssi/esphome-seplos-bms/discussions/50#discussioncomment-5630209))
* 1101-10E-SP76-16S (reported by [@tobox](https://github.com/syssi/esphome-seplos-bms/discussions/42))
* 1101-10E-JK06-16S (Apex 48200, Apex BMS 48V200A, reported by [@Pho3niX90](https://github.com/syssi/esphome-seplos-bms/issues/74))
* Boqiang BMS007-LD43-16S-HW (reported by [@xdilian](https://github.com/syssi/esphome-seplos-bms/discussions/43)) requires custom settings
  ```
  protocol_version: 0x26
  override_pack: 1
  ```
* Boqiang BMS001-HS01-15S (reported by [@xdilian](https://github.com/syssi/esphome-seplos-bms/discussions/43)) requires custom settings
  ```
  protocol_version: 0x26
  override_pack: 1
  override_cell_count: 10
  ```
* Seplos BMS V3.0 Type C, B-48200-C (BMS16S200A-SP05B, FW 1.3, [@Goaheadz](https://github.com/syssi/esphome-seplos-bms/discussions/98)) using [esp8266-seplos-v3-example.yaml](esp32-seplos-v3-example.yaml)

## Untested devices

* EMU10xx
* 11XX Series

## Schematics

```
                  RS485                      UART
┌────────────┐              ┌──────────┐                ┌─────────┐
│            │              │          │<----- RX ----->│         │
│   Seplos   │<-----B- ---->│  RS485   │<----- TX ----->│ ESP32/  │
│    BMS     │<---- A+ ---->│  to TTL  │<----- GND ---->│ ESP8266 │
│            │<--- GND ---->│  module  │<----- 3.3V --->│         │<-- VCC
│            │              │          │                │         │<-- GND
└────────────┘              └──────────┘                └─────────┘

```

Please make sure to power the RS485 module with 3.3V because it affects the TTL (transistor-transistor logic) voltage between RS485 module and ESP.

### RJ45 jack

|  Pin  | Purpose | RS485-to-TTL pin | Color T-568B |
|:-----:|:--------|:-----------------|--------------|
| **1** | **B-**  | **B-**           | Orange-White |
| **2** | **A+**  | **A+**           | Orange       |
| **3** | **GND** | **GND**          | Green-White  |
|   4   | NC      |                  |              |
|   5   | NC      |                  |              |
|   6   | GND     |                  |              |
|   7   | A+      |                  |              |
|   8   | B-      |                  |              |

Please be aware of the different RJ45 pinout colors ([T-568A vs. T-568B](images/rj45-colors-t568a-vs-t568.png)).

## Requirements

* [ESPHome 2026.1.0 or higher](https://github.com/esphome/esphome/releases).
* ESPHome 2026.10.0 or higher for the Seplos V3 RS485 examples using `modbus_controller` (`allow_broadcast_read`)
* Generic ESP32 or ESP8266 board

## Installation

You can install this component with [ESPHome external components feature](https://esphome.io/components/external_components.html) like this:
```yaml
external_components:
  - source: github://syssi/esphome-seplos-bms@main
```

or just use the `esp32-example.yaml` as proof of concept:

```bash
# Install esphome
pip3 install esphome

# Clone this external component
git clone https://github.com/syssi/esphome-seplos-bms.git
cd esphome-seplos-bms

# Create a secrets.yaml containing some setup specific secrets
cat > secrets.yaml <<EOF
wifi_ssid: MY_WIFI_SSID
wifi_password: MY_WIFI_PASSWORD

mqtt_host: MY_MQTT_HOST
mqtt_username: MY_MQTT_USERNAME
mqtt_password: MY_MQTT_PASSWORD

# Required for the BLE examples only. Use the MAC address of your BMS.
bms0_mac_address: MY_BMS_MAC_ADDRESS
EOF

# Validate the configuration, create a binary, upload it, and start logs
# If you use a esp8266 run the esp8266-examle.yaml
esphome run esp32-example.yaml
```

![Screen recording](install.gif)

## Example response all sensors enabled

```
[I][seplos_bms:031]: Telemetry frame received
[D][sensor:124]: 'seplos-bms cell voltage 1': Sending state 3.28800 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 2': Sending state 3.30400 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 3': Sending state 3.31600 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 4': Sending state 3.28700 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 5': Sending state 3.31000 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 6': Sending state 3.30100 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 7': Sending state 3.29700 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 8': Sending state 3.29300 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 9': Sending state 3.30500 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 10': Sending state 3.31200 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 11': Sending state 3.30400 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 12': Sending state 3.31100 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 13': Sending state 3.30700 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 14': Sending state 3.29000 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 15': Sending state 3.29400 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms cell voltage 16': Sending state 3.28900 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms min cell voltage': Sending state 3.28700 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms max cell voltage': Sending state 3.31600 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms max voltage cell': Sending state 3.00000  with 0 decimals of accuracy
[D][sensor:124]: 'seplos-bms min voltage cell': Sending state 4.00000  with 0 decimals of accuracy
[D][sensor:124]: 'seplos-bms delta cell voltage': Sending state 0.02900 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms average cell voltage': Sending state 3.30050 V with 3 decimals of accuracy
[D][sensor:124]: 'seplos-bms temperature 1': Sending state 29.82000 °C with 0 decimals of accuracy
[D][sensor:124]: 'seplos-bms temperature 2': Sending state 29.76000 °C with 0 decimals of accuracy
[D][sensor:124]: 'seplos-bms temperature 3': Sending state 29.67000 °C with 0 decimals of accuracy
[D][sensor:124]: 'seplos-bms temperature 4': Sending state 29.82000 °C with 0 decimals of accuracy
[D][sensor:124]: 'seplos-bms environment temperature': Sending state 29.81000 °C with 0 decimals of accuracy
[D][sensor:124]: 'seplos-bms mosfet temperature': Sending state 29.78000 °C with 0 decimals of accuracy
[D][sensor:124]: 'seplos-bms current': Sending state -6.54000 A with 2 decimals of accuracy
[D][sensor:124]: 'seplos-bms total voltage': Sending state 52.80000 V with 2 decimals of accuracy
[D][sensor:124]: 'seplos-bms power': Sending state -345.31198 W with 2 decimals of accuracy
[D][sensor:124]: 'seplos-bms charging power': Sending state 0.00000 W with 2 decimals of accuracy
[D][sensor:124]: 'seplos-bms discharging power': Sending state 345.31198 W with 2 decimals of accuracy
[D][sensor:124]: 'seplos-bms residual capacity': Sending state 133.86000 Ah with 2 decimals of accuracy
[D][sensor:124]: 'seplos-bms battery capacity': Sending state 170.00000 Ah with 2 decimals of accuracy
[D][sensor:124]: 'seplos-bms state of charge': Sending state 78.70000 % with 1 decimals of accuracy
[D][sensor:124]: 'seplos-bms rated capacity': Sending state 180.00000 Ah with 2 decimals of accuracy
[D][sensor:124]: 'seplos-bms charging cycles': Sending state 70.00000  with 0 decimals of accuracy
[D][sensor:124]: 'seplos-bms state of health': Sending state 100.00000 % with 1 decimals of accuracy
[D][sensor:124]: 'seplos-bms port voltage': Sending state 52.79000 V with 2 decimals of accuracy
```

## Known issues and limitations

None.

## Protocol

See [SEPLOS BMS Communication Protocol_V2.0.pdf](docs/SEPLOS%20BMS%20Communication%20Protocol_V2.0.pdf) and [Seplos 48v 100A BMS RS485 Protocol.pdf](docs/Seplos%2048v%20100A%20BMS%20RS485%20Protocol.pdf).

```
$ echo -ne "~20004642E00200FD37\r" | hexdump -ve '1/1 "%.2X."'
      7E.32.30.30.30.34.36.34.32.45.30.30.32.30.30.46.44.33.37.0D.

# Get pack #0 telemetry data (CID2 `0x42`)
TX -> "~20004642E00200FD37\r"
RX <- "~2000460010960001100CD70CE90CF40CD60CEF0CE50CE10CDC0CE90CF00CE80CEF0CEA0CDA0CDE0CD8060BA60BA00B970BA60BA50BA2FD5C14A0344E0A426803134650004603E8149F0000000000000000DC6C\r"

# Get system parameters (CID2 `0x47`)
TX -> "~200046470000FDA9\r"
RX <- ?

# Get protocol version (CID2 `0x4F`)
TX -> "~2000464F0000FD9A\r"
RX <- ?

# Get manufacturer info (CID2 `0x51`)
TX -> "~200046510000FDAE\r"
RX <- "~20004600C040313130312D5350313520020743414E50726F746F636F6C3A536F666172202020F046\r"

# Get management info (pylontech only?)
TX -> "~200046920000FDA9\r"
RX <- ?

# Get module serial number (pylontech only?)
TX -> "~200046930000FDA8\r"
RX <- ?
```

## Debugging

If this component doesn't work out of the box for your device please update your configuration to enable the debug output of the UART component and increase the log level to the see outgoing and incoming serial traffic:

```
logger:
  level: DEBUG

uart:
  id: uart_0
  baud_rate: 9600
  tx_pin: ${tx_pin}
  rx_pin: ${rx_pin}
  rx_buffer_size: 384
  debug:
    dummy_receiver: false
    direction: BOTH
    after:
      delimiter: "\r"
    sequence:
      - lambda: UARTDebug::log_string(direction, bytes);
```

## References

* https://github.com/Frankkkkk/python-pylontech/blob/master/pylontech/pylontech.py
* https://diysolarforum.com/threads/simple-seplos-bms-protocol-decode-bash-script.34993/
* https://github.com/celsworth/lxp-pylon-utils/tree/master/lib/pylon/packet
* https://github.com/meteosat007/solar-pylontech
* https://github.com/BrucePerens/seplos_c
