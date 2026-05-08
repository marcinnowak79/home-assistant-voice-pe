# Home Assistant Voice: Preview Edition

This is the ESPHome source code of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).

## Gemini Live Proxy Beta

This fork is an experimental first beta of a Gemini Live based voice flow for Home Assistant Voice PE.

It exists because I wanted to reduce perceived response latency and make the conversation feel more natural. The standard Home Assistant Voice Assistant flow did not give me the streaming behavior I wanted: it records the utterance, sends it after capture, then waits for the response. This fork instead streams microphone audio from the device to a local Home Assistant add-on, which forwards the audio to Gemini Live and streams response audio back to the device.

The matching Home Assistant add-on is here:

- [Gemini Live Proxy add-on](https://github.com/marcinnowak79/gemini-live-proxy)

This is not an official Home Assistant, ESPHome, Nabu Casa, or Google project. It was vibe-coded as a working experiment and is published mainly as inspiration for people exploring lower-latency voice assistant flows. Treat it as beta software: read the code, adapt it to your setup, and do not assume production-level stability or security hardening.

See [the documentation](https://voice-pe.home-assistant.io/) for set up and troubleshooting.

If you need to re-install the firmware, [use this installer](https://esphome.github.io/home-assistant-voice-pe/).

## Firmware Changes

This fork includes `home-assistant-voice-gemini.yaml` and a local ESPHome component at `esphome/components/gemini_proxy`.

### Prerequisites

- Home Assistant Voice PE connected over USB, or available over OTA after the first flash
- Python 3 with ESPHome installed, or the ESPHome add-on in Home Assistant
- A running Gemini Live Proxy add-on reachable from the Voice PE device

Install ESPHome locally:

```bash
python3 -m venv esphome_venv
. esphome_venv/bin/activate
pip install esphome
```

### Configuration

Create a local `secrets.yaml` from `secrets.example.yaml`:

```yaml
wifi_ssid: "YOUR_WIFI_SSID"
wifi_password: "YOUR_WIFI_PASSWORD"
gemini_proxy_url: "ws://homeassistant.local:8765"
```

`secrets.yaml` is ignored by git and must not be published. If `homeassistant.local` is not resolvable from the Voice PE device, use the Home Assistant IP address.

Wake word selection is compiled into the ESPHome firmware. To change it, edit the substitutions at the top of `home-assistant-voice-gemini.yaml` before compiling:

```yaml
wake_word_model: custom_wake_words/dzefrej/manifest.json
wake_word_id: dzefrej
wake_word_cutoff_slight: '217'
wake_word_cutoff_moderate: '196'
wake_word_cutoff_very: '176'
```

The model path must point to a microWakeWord manifest included in the firmware source tree. After changing the wake word model, compile and upload the firmware again.

### Compile and Upload

```bash
esphome config home-assistant-voice-gemini.yaml
esphome compile home-assistant-voice-gemini.yaml
esphome upload home-assistant-voice-gemini.yaml
```

For a USB upload, pass the serial device explicitly:

```bash
esphome upload home-assistant-voice-gemini.yaml --device /dev/cu.usbmodemXXXX
```

On Linux this is usually `/dev/ttyACM0` or `/dev/ttyUSB0`.
