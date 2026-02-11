import esphome.codegen as cg
import esphome.config_validation as cv

from esphome.components import ble_client, light, binary_sensor
from esphome.const import CONF_OUTPUT_ID

CONF_BLE_CLIENT_ID = "ble_client_id"
CONF_CONNECTED_SENSOR_ID = "connected_sensor_id"

DEPENDENCIES = ["ble_client"]

neewer_ns = cg.esphome_ns.namespace("neewer_component")
NeewerRGBCCTLight = neewer_ns.class_(
    "NeewerRGBCCTLight",
    light.LightOutput,
    cg.Component,
)

CONFIG_SCHEMA = light.LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(NeewerRGBCCTLight),
        cv.Required(CONF_BLE_CLIENT_ID): cv.use_id(ble_client.BLEClient),
        cv.Optional(CONF_CONNECTED_SENSOR_ID): cv.use_id(binary_sensor.BinarySensor),
    }
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])

    # ✅ IMPORTANT: this makes loop() run (and fixes HA disconnected)
    await cg.register_component(var, config)

    await light.register_light(var, config)

    client = await cg.get_variable(config[CONF_BLE_CLIENT_ID])
    cg.add(var.set_ble_client(client))

    if CONF_CONNECTED_SENSOR_ID in config:
        bs = await cg.get_variable(config[CONF_CONNECTED_SENSOR_ID])
        cg.add(var.set_connected_sensor(bs))
