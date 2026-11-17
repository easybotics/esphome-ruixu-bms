import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_BATTERY_CHARGING,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_HEAT,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import CONF_SEPLOS_BMS_ID, SEPLOS_BMS_COMPONENT_SCHEMA

DEPENDENCIES = ["seplos_bms"]

CODEOWNERS = ["@syssi"]

CONF_ONLINE_STATUS = "online_status"
# Ruixu 0x44 alarm/status reply (telemetry_layout ruixu only)
CONF_CHARGING_MOSFET = "charging_mosfet"
CONF_DISCHARGING_MOSFET = "discharging_mosfet"
CONF_PRECHARGE_MOSFET = "precharge_mosfet"
CONF_CHARGING = "charging"
CONF_DISCHARGING = "discharging"
CONF_HEATING = "heating"
CONF_FULLY_CHARGED = "fully_charged"
CONF_ALARM = "alarm"

# key: binary_sensor_schema kwargs
BINARY_SENSOR_DEFS = {
    CONF_ONLINE_STATUS: {
        "device_class": DEVICE_CLASS_CONNECTIVITY,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    },
    CONF_CHARGING_MOSFET: {
        "device_class": DEVICE_CLASS_POWER,
        "icon": "mdi:battery-arrow-up",
    },
    CONF_DISCHARGING_MOSFET: {
        "device_class": DEVICE_CLASS_POWER,
        "icon": "mdi:battery-arrow-down",
    },
    CONF_PRECHARGE_MOSFET: {
        "device_class": DEVICE_CLASS_POWER,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    },
    CONF_CHARGING: {"device_class": DEVICE_CLASS_BATTERY_CHARGING},
    CONF_DISCHARGING: {"icon": "mdi:battery-minus"},
    CONF_HEATING: {"device_class": DEVICE_CLASS_HEAT},
    CONF_FULLY_CHARGED: {"icon": "mdi:battery-check"},
    CONF_ALARM: {"device_class": DEVICE_CLASS_PROBLEM},
}

CONFIG_SCHEMA = SEPLOS_BMS_COMPONENT_SCHEMA.extend(
    {
        cv.Optional(key): binary_sensor.binary_sensor_schema(**kwargs)
        for key, kwargs in BINARY_SENSOR_DEFS.items()
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_SEPLOS_BMS_ID])
    for key in BINARY_SENSOR_DEFS:
        if key in config:
            conf = config[key]
            sens = await binary_sensor.new_binary_sensor(conf)
            cg.add(getattr(hub, f"set_{key}_binary_sensor")(sens))
