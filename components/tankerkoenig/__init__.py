"""Fuel prices: a tile, a card and a top bar item from the template sensor over the Tankerkönig integration.

The tile and the card get the template sensor from the app (a tile of an entity); the top bar item reads its lists
itself through the text sensors of plugin.yaml, with the three settings there. register_plugin() takes the id,
version, tile memory and the screen's texts from the manifest and translations/ beside this folder.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number, select, smart_display, switch, text_sensor
from esphome.const import CONF_ID

DEPENDENCIES = ["smart_display"]
AUTO_LOAD = ["text_sensor", "switch", "select", "number"]

# The fuels in the order of FUEL_KEYS in the C++.
FUELS = ["e5", "e10", "diesel"]

tankerkoenig_ns = cg.esphome_ns.namespace("tankerkoenig")
Tankerkoenig = tankerkoenig_ns.class_("Tankerkoenig", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Tankerkoenig),
        cv.Optional("names"): cv.use_id(text_sensor.TextSensor),
        cv.Optional("alert"): cv.use_id(switch.Switch),
        cv.Optional("alert_fuel"): cv.use_id(select.Select),
        cv.Optional("alert_price"): cv.use_id(number.Number),
    }
    | {cv.Optional(fuel): cv.use_id(text_sensor.TextSensor) for fuel in FUELS}
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
    if "names" in config:
        cg.add(var.set_names(await cg.get_variable(config["names"])))
    for index, fuel in enumerate(FUELS):
        if fuel in config:
            cg.add(var.set_prices(index, await cg.get_variable(config[fuel])))
    if "alert" in config:
        cg.add(var.set_alert(await cg.get_variable(config["alert"])))
    if "alert_fuel" in config:
        cg.add(var.set_alert_fuel(await cg.get_variable(config["alert_fuel"])))
    if "alert_price" in config:
        cg.add(var.set_alert_price(await cg.get_variable(config["alert_price"])))
