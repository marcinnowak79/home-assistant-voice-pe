#include "gemini_proxy.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace esphome {
namespace gemini_proxy {

static const char *TAG = "gemini_proxy";
static std::atomic<int> g_set_phase{-1};  // -1 = no change, >=0 = set voice_assistant_phase

void GeminiProxy::setup() {
  ESP_LOGW(TAG, "Setup, url: %s", this->proxy_url_.c_str());

  // 64KB after mono16 conversion is ~2 seconds of audio at 16kHz.
  this->ring_buffer_ = RingBuffer::create(65536);

  // Register mic callback — write to ring buffer only when session active
  if (this->mic_ != nullptr) {
    this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
      if (this->session_state_.load() != SessionState::STREAMING_MIC || data.size() < 8)
        return;
      std::shared_ptr<RingBuffer> rb = this->ring_buffer_;
      if (!rb)
        return;

      size_t frame_count = data.size() / 8;  // stereo 32-bit frames
      std::vector<int16_t> mono(frame_count);
      const auto *samples = reinterpret_cast<const int32_t *>(data.data());
      for (size_t i = 0; i < frame_count; i++) {
        mono[i] = static_cast<int16_t>(samples[i * 2] >> 16);  // channel 0 -> mono16
      }

      size_t bytes = mono.size() * sizeof(int16_t);
      size_t written = rb->write_without_replacement(mono.data(), bytes, 0, true);
      if (written < bytes) {
        this->dropped_audio_bytes_.fetch_add(bytes - written);
      }
    });
  }
}

void GeminiProxy::loop() {
  if (this->disconnect_requested_.exchange(false)) {
    this->disconnect_();
  }

  // If WS dropped while session active, reset state (safe — runs on main thread)
  SessionState state = this->session_state_.load();
  if (state == SessionState::CONNECTING && millis() - this->connect_started_ms_ > 5000) {
    ESP_LOGE(TAG, "WS connect timed out");
    this->reset_session_(false);
    this->disconnect_requested_.store(true);
    g_set_phase.store(1);
    state = SessionState::IDLE;
  }

  if (state != SessionState::IDLE && state != SessionState::CONNECTING && !this->ws_connected_.load()) {
    ESP_LOGW(TAG, "WS lost during session, resetting");
    this->reset_session_(false);
    g_set_phase.store(1);  // idle
    state = SessionState::IDLE;
  }

  if (this->start_mic_requested_.exchange(false)) {
    if (this->mic_ != nullptr && !this->mic_->is_running()) {
      this->mic_->start();
    }
    if (this->ring_buffer_) {
      this->ring_buffer_->reset();
    }
    this->dropped_audio_bytes_.store(0);
    this->session_state_.store(SessionState::STREAMING_MIC);
    g_set_phase.store(3);  // listening phase — green spin
    ESP_LOGW(TAG, ">>> ACTIVE ws=%d", this->ws_connected_.load());
  }

  // Send buffered mic data to proxy
  if (this->session_state_.load() == SessionState::STREAMING_MIC && this->ws_connected_.load() && this->ring_buffer_) {
    size_t available = this->ring_buffer_->available();
    if (available > 0) {
      size_t to_read = std::min(available, (size_t) 4096);
      uint8_t buf[4096];
      size_t read = this->ring_buffer_->read((void *) buf, to_read, 0);
      if (read > 0) {
        this->send_binary_(MSG_AUDIO_IN, buf, read);
      }
    }

    size_t dropped = this->dropped_audio_bytes_.exchange(0);
    if (dropped > 0) {
      ESP_LOGW(TAG, "Dropped %u bytes of microphone audio", static_cast<unsigned>(dropped));
    }
  }

  // Play audio URL via media_player (triggered from WS thread, executed here on main thread)
  if (this->play_audio_.exchange(false) && this->media_player_ != nullptr) {
    std::string audio_url = this->take_audio_url_();
    ESP_LOGW(TAG, "Playing: %s", audio_url.c_str());
    this->media_player_->make_call()
        .set_media_url(audio_url)
        .set_announcement(true)
        .perform();
  }
}

void GeminiProxy::start() {
  SessionState expected = SessionState::IDLE;
  if (!this->session_state_.compare_exchange_strong(expected, SessionState::CONNECTING)) {
    ESP_LOGW(TAG, "Already active, ignoring start (state=%u)", static_cast<unsigned>(expected));
    return;
  }

  ESP_LOGW(TAG, ">>> START");
  this->connect_started_ms_ = millis();
  this->play_audio_.store(false);
  if (this->ring_buffer_) {
    this->ring_buffer_->reset();
  }
  this->connect_();
}

void GeminiProxy::stop() {
  ESP_LOGI(TAG, "Stopping");
  if (this->ws_connected_.load()) {
    this->send_binary_(MSG_AUDIO_END, nullptr, 0);
  }
  this->reset_session_(false);
  this->disconnect_requested_.store(true);
  g_set_phase.store(1);
}

void GeminiProxy::connect_() {
  this->disconnect_();

  esp_websocket_client_config_t config = {};
  config.uri = this->proxy_url_.c_str();
  config.buffer_size = 4096;
  config.task_stack = 8192;

  this->ws_ = esp_websocket_client_init(&config);
  if (this->ws_ == nullptr) {
    ESP_LOGE(TAG, "WS init failed");
    return;
  }

  esp_websocket_register_events(this->ws_, WEBSOCKET_EVENT_ANY,
                                 ws_event_handler_, this);
  esp_err_t err = esp_websocket_client_start(this->ws_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WS start failed: %s", esp_err_to_name(err));
    this->reset_session_(true);
    g_set_phase.store(1);
  }
}

void GeminiProxy::disconnect_() {
  if (this->ws_ != nullptr) {
    esp_websocket_client_stop(this->ws_);
    esp_websocket_client_destroy(this->ws_);
    this->ws_ = nullptr;
  }
  this->ws_connected_.store(false);
}

void GeminiProxy::send_binary_(uint8_t type, const uint8_t *data, size_t len) {
  if (!this->ws_connected_.load() || this->ws_ == nullptr)
    return;

  std::vector<uint8_t> frame(1 + len);
  frame[0] = type;
  if (data != nullptr && len > 0) {
    memcpy(&frame[1], data, len);
  }

  esp_err_t err = esp_websocket_client_send_bin(this->ws_,
                                                (const char *) frame.data(),
                                                frame.size(),
                                                pdMS_TO_TICKS(50));
  if (err < 0) {
    ESP_LOGW(TAG, "WS send failed: %d", err);
  }
}

void GeminiProxy::reset_session_(bool close_socket) {
  this->session_state_.store(SessionState::IDLE);
  this->start_mic_requested_.store(false);
  this->play_audio_.store(false);
  if (this->ring_buffer_) {
    this->ring_buffer_->reset();
  }
  if (close_socket) {
    this->disconnect_();
  }
}

std::string GeminiProxy::build_default_audio_url_() const {
  std::string http_url = this->proxy_url_;
  if (http_url.rfind("ws://", 0) == 0) {
    http_url = "http://" + http_url.substr(5);
  } else if (http_url.rfind("wss://", 0) == 0) {
    http_url = "https://" + http_url.substr(6);
  }
  size_t colon = http_url.rfind(':');
  size_t slash = http_url.find('/', http_url.find("//") == std::string::npos ? 0 : http_url.find("//") + 2);
  if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
    http_url = http_url.substr(0, colon) + ":8766/response.wav";
  }
  return http_url;
}

void GeminiProxy::set_audio_url_(std::string url) {
  std::lock_guard<std::mutex> lock(this->audio_url_mutex_);
  this->audio_url_ = std::move(url);
}

std::string GeminiProxy::take_audio_url_() {
  std::lock_guard<std::mutex> lock(this->audio_url_mutex_);
  return this->audio_url_;
}

void GeminiProxy::ws_event_handler_(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data) {
  auto *self = static_cast<GeminiProxy *>(arg);
  esp_websocket_event_data_t *ws_data = static_cast<esp_websocket_event_data_t *>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "WS connected");
      self->ws_connected_.store(true);
      if (self->session_state_.load() == SessionState::CONNECTING) {
        self->start_mic_requested_.store(true);
      }
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "WS disconnected");
      self->ws_connected_.store(false);
      break;

    case WEBSOCKET_EVENT_DATA:
      if (ws_data->op_code == 0x02 && ws_data->data_len > 0) {
        self->on_ws_data_((const uint8_t *) ws_data->data_ptr, ws_data->data_len);
      }
      break;

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "WS error");
      break;
  }
}

void GeminiProxy::on_ws_data_(const uint8_t *data, size_t len) {
  if (len < 1)
    return;

  uint8_t msg_type = data[0];

  switch (msg_type) {
    case MSG_AUDIO_OUT:
      break;  // Ignored — using HTTP streaming

    case 0x04:  // MSG_STATE_LISTENING
      g_set_phase.store(3);  // listening — green spin
      break;

    case 0x05:  // MSG_STATE_THINKING
      break;  // Keep green spinning

    case MSG_RESPONSE_START: {
      ESP_LOGW(TAG, "Response start → streaming audio");
      this->session_state_.store(SessionState::RESPONDING);

      std::string http_url;
      if (len > 1) {
        http_url.assign(reinterpret_cast<const char *>(data + 1), len - 1);
      }
      if (!http_url.empty() && http_url[0] == '/') {
        std::string base_url = this->build_default_audio_url_();
        size_t path_start = base_url.find('/', base_url.find("//") == std::string::npos ? 0 : base_url.find("//") + 2);
        if (path_start != std::string::npos) {
          base_url = base_url.substr(0, path_start);
        }
        http_url = base_url + http_url;
      }
      if (http_url.empty()) {
        http_url = this->build_default_audio_url_();
      }
      this->set_audio_url_(http_url);
      g_set_phase.store(5);  // replying phase
      this->play_audio_.store(true);  // loop() will trigger media_player
      break;
    }

    case MSG_RESPONSE_END: {
      ESP_LOGW(TAG, "Response end");
      this->reset_session_(false);
      this->disconnect_requested_.store(true);
      // Keep the replying LED phase until the media player reports that playback ended.
      // This avoids a dark gap while the HTTP audio stream drains through the speaker pipeline.
      break;
    }

    default:
      break;
  }
}

int GeminiProxy::consume_pending_phase() {
  return g_set_phase.exchange(-1);
}

bool GeminiProxy::is_running() const {
  return this->session_state_.load() != SessionState::IDLE;
}

}  // namespace gemini_proxy
}  // namespace esphome
