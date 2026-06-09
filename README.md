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

Wake word selection is compiled into the ESPHome firmware. It is controlled by the substitutions at
the top of `home-assistant-voice-gemini.yaml`. After changing them, compile and upload again.

```yaml
wake_word_model: custom_wake_words/dzefrej/manifest.json
wake_word_id: dzefrej
wake_word_cutoff_slight: '253'      # ~0.99 probability, strictest (fewest false triggers)
wake_word_cutoff_moderate: '250'    # ~0.98 probability, default
wake_word_cutoff_very: '247'        # ~0.97 probability, most sensitive
```

The three cutoffs are the probability thresholds (0–255, roughly `value / 255`) for the firmware's
"Slightly / Moderately / Very" sensitivity levels — higher is stricter. The values above are tuned for
the `dzefrej` model.

**Use a stock wake word (simplest — nothing to add to the repo)**

`wake_word_model` accepts a URL, so you can point it straight at an official microWakeWord manifest.
The model and its `.tflite` are downloaded at build time:

```yaml
wake_word_model: https://raw.githubusercontent.com/esphome/micro-wake-word-models/main/models/v2/okay_nabu.json
wake_word_id: okay_nabu
```

Available stock models (v2): `okay_nabu`, `hey_jarvis`, `hey_mycroft`, `alexa` — swap the filename in the
URL and set `wake_word_id` to match. Each stock manifest ships its own probability cutoff; the
`wake_word_cutoff_*` values above stay in effect for the sensitivity select, so leave them or adjust to
taste.

**Use a custom model**

Drop the model's `manifest.json` and its `.tflite` into `custom_wake_words/<id>/`, then point the
substitutions at it:

```yaml
wake_word_model: custom_wake_words/<id>/manifest.json
wake_word_id: <id>
```

`wake_word_id` must match the `id` you want in ESPHome; the manifest's `model:` field must name a
`.tflite` that sits next to it.

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

### Troubleshooting

**Build fails with `Component not found: sensor.` / `- platform: rotary_encoder` (or `fatal error: flac_decoder.h`)**

This happens on ESPHome **2026.5+** when the config still pulls the old `kahrendt/esphome` fork
(`const`, `media_source`, `sendspin`) via `external_components`. The streaming media-player work
(PR [#14933](https://github.com/esphome/esphome/pull/14933)) was merged into ESPHome core, and the
old fork no longer compiles against it: its `const` is missing `CONF_B_CONSTANT` (which breaks the
import of the whole `sensor` component — `rotary_encoder` is just the first platform under it, not
the real cause), and its `sendspin` includes the now-relocated `flac_decoder.h`.

Fix: pull the latest version of this branch. The fork's `external_components` block was removed —
core now provides `const`, `media_source` and `sendspin`. If you maintain your own copy, delete that
`kahrendt/esphome` source block.

**Wake word works but you hear no Gemini response; logs show `http_media_source: Unable to determine file type`**

The proxy streams the response as `audio/wav`, but core ESPHome only compiles in the audio decoders
that are explicitly requested. Make sure this block is present (it is in the latest version):

```yaml
audio:
  codecs:
    wav:
```

**After flashing from the CLI, Home Assistant loses the connection and the wake word stops working**

`api:` uses `encryption:` with no key, because the Home Assistant ESPHome add-on injects the device's
key at build time. If you build/flash from the **CLI** instead, the firmware comes up with a different
key and HA can no longer connect — and because the wake word is started by `voice_assistant:
on_client_connected`, it never starts listening.

To flash from the CLI, bake in the device's existing key: copy it from Home Assistant
(Settings → Devices & Services → ESPHome → your device → encryption key, or the ESPHome add-on's
`secrets.yaml` → `api_encryption_key`), put it in your local `secrets.yaml` as `api_encryption_key`,
and set:

```yaml
api:
  encryption:
    key: !secret api_encryption_key
```

Or simply flash via the Home Assistant ESPHome add-on, which handles the key for you.
