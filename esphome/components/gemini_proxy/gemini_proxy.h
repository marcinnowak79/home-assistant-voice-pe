#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/ring_buffer.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/media_player/media_player.h"

#include "esp_websocket_client.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace esphome {
namespace gemini_proxy {

// Protocol: ESP32→Proxy
static const uint8_t MSG_AUDIO_IN = 0x01;
static const uint8_t MSG_AUDIO_END = 0x02;

// Protocol: Proxy→ESP32
static const uint8_t MSG_AUDIO_OUT = 0x01;
static const uint8_t MSG_RESPONSE_END = 0x02;
static const uint8_t MSG_RESPONSE_START = 0x06;

enum class SessionState : uint8_t {
  IDLE = 0,
  CONNECTING = 1,
  STREAMING_MIC = 2,
  WAITING_RESPONSE = 3,
  RESPONDING = 4,
};

class GeminiProxy : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_microphone(microphone::Microphone *mic) { this->mic_ = mic; }
  void set_media_player(media_player::MediaPlayer *mp) { this->media_player_ = mp; }
  void set_debug_logging(bool debug_logging) { this->debug_logging_ = debug_logging; }

  /// Returns pending LED phase change, or -1 if none (for YAML interval polling)
  int consume_pending_phase();
  void set_proxy_url(const std::string &url) { this->proxy_url_ = url; }

  void start();
  void stop();
  bool is_running() const;

 protected:
  void connect_();
  void disconnect_(const char *reason = "unknown");
  void send_binary_(uint8_t type, const uint8_t *data, size_t len);
  void reset_session_(bool close_socket, const char *reason = "unknown");
  std::string build_default_audio_url_() const;
  void set_audio_url_(std::string url);
  std::string take_audio_url_();
  void start_audio_tx_task_();
  void stop_audio_tx_task_(const char *reason = "unknown");
  static void audio_tx_task_(void *arg);
  static const char *state_name_(SessionState state);
  void log_state_(const char *event, const char *reason = "");

  static void ws_event_handler_(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);
  void on_ws_data_(const uint8_t *data, size_t len);

  microphone::Microphone *mic_{nullptr};
  media_player::MediaPlayer *media_player_{nullptr};
  std::string proxy_url_;
  bool debug_logging_{false};

  esp_websocket_client_handle_t ws_{nullptr};
  std::atomic<bool> ws_connected_{false};
  std::atomic<SessionState> session_state_{SessionState::IDLE};
  std::atomic<bool> start_mic_requested_{false};
  std::atomic<bool> stop_mic_requested_{false};
  std::atomic<bool> disconnect_requested_{false};
  std::atomic<bool> audio_tx_running_{false};
  std::atomic<bool> audio_tx_stop_requested_{false};
  uint32_t connect_started_ms_{0};
  uint32_t streaming_started_ms_{0};
  uint32_t waiting_response_started_ms_{0};
  uint32_t response_started_ms_{0};
  uint32_t session_id_{0};
  uint32_t last_active_diag_ms_{0};
  SessionState last_logged_state_{SessionState::IDLE};

  // Mic send buffer — use ring buffer for thread safety (mic callback vs main loop)
  std::shared_ptr<RingBuffer> ring_buffer_;
  std::atomic<size_t> dropped_audio_bytes_{0};
  std::atomic<uint32_t> audio_chunks_sent_{0};
  std::atomic<uint32_t> audio_bytes_sent_{0};
  std::atomic<uint32_t> audio_send_failures_{0};

  std::mutex audio_url_mutex_;
  std::mutex ws_mutex_;
  std::string audio_url_;
  std::atomic<bool> play_audio_{false};
};

// Actions for YAML
template<typename... Ts> class StartAction : public Action<Ts...> {
 public:
  explicit StartAction(GeminiProxy *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->start(); }
 protected:
  GeminiProxy *parent_;
};

template<typename... Ts> class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(GeminiProxy *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->stop(); }
 protected:
  GeminiProxy *parent_;
};

}  // namespace gemini_proxy
}  // namespace esphome
