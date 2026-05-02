# Home Assistant Voice: Preview Edition

This is the ESPHome source code of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).

See [the documentation](https://voice-pe.home-assistant.io/) for set up and troubleshooting.

If you need to re-install the firmware, [use this installer](https://esphome.github.io/home-assistant-voice-pe/).

## Gemini Live Proxy Firmware

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
