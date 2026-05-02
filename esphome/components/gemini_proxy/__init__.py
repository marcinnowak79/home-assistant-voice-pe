"""Minimal ESPHome component: Gemini Proxy WebSocket client.

Only handles WebSocket connection + audio streaming.
Everything else (wake word, LEDs, WiFi, speaker) stays in YAML.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome import automation
from esphome.components import microphone, speaker, media_player

CODEOWNERS = []
DEPENDENCIES = ["wifi"]

CONF_MICROPHONE = "microphone"
CONF_SPEAKER = "speaker"
CONF_MEDIA_PLAYER = "media_player"
CONF_PROXY_URL = "proxy_url"

gemini_proxy_ns = cg.esphome_ns.namespace("gemini_proxy")
GeminiProxy = gemini_proxy_ns.class_("GeminiProxy", cg.Component)

# Actions callable from YAML
StartAction = gemini_proxy_ns.class_("StartAction", automation.Action)
StopAction = gemini_proxy_ns.class_("StopAction", automation.Action)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(GeminiProxy),
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Required(CONF_MEDIA_PLAYER): cv.use_id(media_player.MediaPlayer),
        cv.Required(CONF_PROXY_URL): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


# Register actions so YAML can call gemini_proxy.start / gemini_proxy.stop
@automation.register_action("gemini_proxy.start", StartAction, automation.maybe_simple_id({
    cv.GenerateID(): cv.use_id(GeminiProxy),
}))
async def start_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action("gemini_proxy.stop", StopAction, automation.maybe_simple_id({
    cv.GenerateID(): cv.use_id(GeminiProxy),
}))
async def stop_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))

    mp = await cg.get_variable(config[CONF_MEDIA_PLAYER])
    cg.add(var.set_media_player(mp))

    cg.add(var.set_proxy_url(config[CONF_PROXY_URL]))

    # esp_websocket_client is included in ESP-IDF SDK — no extra library needed
