#include "gemini_proxy.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace esphome {
namespace gemini_proxy {

static const char *TAG = "gemini_proxy";
static std::atomic<int> g_set_phase{-1};  // -1 = no change, >=0 = set voice_assistant_phase

const char *GeminiProxy::state_name_(SessionState state) {
  switch (state) {
    case SessionState::IDLE:
      return "IDLE";
    case SessionState::CONNECTING:
      return "CONNECTING";
    case SessionState::STREAMING_MIC:
      return "STREAMING_MIC";
    case SessionState::WAITING_RESPONSE:
      return "WAITING_RESPONSE";
    case SessionState::RESPONDING:
      return "RESPONDING";
    default:
      return "UNKNOWN";
  }
}

void GeminiProxy::log_state_(const char *event, const char *reason) {
  if (!this->debug_logging_)
    return;
  uint32_t now = millis();
  SessionState state = this->session_state_.load();
  uint32_t stream_ms = this->streaming_started_ms_ == 0 ? 0 : now - this->streaming_started_ms_;
  uint32_t wait_ms = this->waiting_response_started_ms_ == 0 ? 0 : now - this->waiting_response_started_ms_;
  uint32_t response_ms = this->response_started_ms_ == 0 ? 0 : now - this->response_started_ms_;
  ESP_LOGW(TAG,
           "[diag] event=%s reason=%s sid=%u state=%s ws=%d mic=%d stream_ms=%u wait_ms=%u response_ms=%u "
           "chunks=%u bytes=%u failures=%u play_audio=%d",
           event, reason, this->session_id_, state_name_(state), this->ws_connected_.load(),
           this->mic_ != nullptr && this->mic_->is_running(), stream_ms, wait_ms, response_ms,
           this->audio_chunks_sent_.load(), this->audio_bytes_sent_.load(),
           this->audio_send_failures_.load(), this->play_audio_.load());
}

void GeminiProxy::setup() {
  ESP_LOGCONFIG(TAG, "Gemini proxy URL: %s", this->proxy_url_.c_str());

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
  if (this->stop_mic_requested_.exchange(false)) {
    if (this->mic_ != nullptr && this->mic_->is_running()) {
      this->mic_->stop();
      if (this->debug_logging_)
        ESP_LOGW(TAG, "[diag] mic_stop_called sid=%u", this->session_id_);
    } else {
      if (this->debug_logging_)
        ESP_LOGW(TAG, "[diag] mic_stop_skipped sid=%u mic_present=%d mic_running=%d", this->session_id_,
                 this->mic_ != nullptr, this->mic_ != nullptr && this->mic_->is_running());
    }
  }

  if (this->disconnect_requested_.exchange(false)) {
    this->log_state_("loop_disconnect_requested", "async_request");
    this->disconnect_("loop_disconnect_requested");
  }

  // If WS dropped while session active, reset state (safe — runs on main thread)
  SessionState state = this->session_state_.load();
  uint32_t now = millis();
  if (state != this->last_logged_state_) {
    this->last_logged_state_ = state;
    this->log_state_("loop_state_transition", "state_changed");
  }
  if (this->debug_logging_ && state != SessionState::IDLE && now - this->last_active_diag_ms_ > 1000) {
    this->last_active_diag_ms_ = now;
    int media_state = this->media_player_ == nullptr ? -1 : static_cast<int>(this->media_player_->state);
    bool announcing = this->media_player_ != nullptr &&
                      this->media_player_->state == media_player::MediaPlayerState::MEDIA_PLAYER_STATE_ANNOUNCING;
    ESP_LOGW(TAG,
             "[diag] active_heartbeat sid=%u state=%s ws=%d mic=%d media_state=%d announcing=%d "
             "ring_available=%u dropped=%u play_audio=%d",
             this->session_id_, state_name_(state), this->ws_connected_.load(),
             this->mic_ != nullptr && this->mic_->is_running(), media_state, announcing,
             this->ring_buffer_ ? static_cast<unsigned>(this->ring_buffer_->available()) : 0,
             static_cast<unsigned>(this->dropped_audio_bytes_.load()), this->play_audio_.load());
  }
  if (state == SessionState::CONNECTING && millis() - this->connect_started_ms_ > 5000) {
    ESP_LOGE(TAG, "WS connect timed out");
    this->log_state_("connect_timeout", "connect_5s");
    this->reset_session_(false, "connect_timeout");
    this->disconnect_requested_.store(true);
    g_set_phase.store(1);
    state = SessionState::IDLE;
  }

  if (state != SessionState::IDLE && state != SessionState::CONNECTING && !this->ws_connected_.load()) {
    this->log_state_("ws_lost_during_session", "ws_connected_false");
    this->reset_session_(false, "ws_lost_during_session");
    g_set_phase.store(1);  // idle
    state = SessionState::IDLE;
  }

  if (this->start_mic_requested_.exchange(false)) {
    this->log_state_("start_mic_requested", "loop");
    if (this->mic_ != nullptr && !this->mic_->is_running()) {
      this->mic_->start();
      if (this->debug_logging_)
        ESP_LOGW(TAG, "[diag] mic_start_called sid=%u", this->session_id_);
    } else {
      if (this->debug_logging_)
        ESP_LOGW(TAG, "[diag] mic_start_skipped sid=%u mic_present=%d mic_running=%d", this->session_id_,
                 this->mic_ != nullptr, this->mic_ != nullptr && this->mic_->is_running());
    }
    if (this->ring_buffer_) {
      this->ring_buffer_->reset();
    }
    this->dropped_audio_bytes_.store(0);
    this->audio_chunks_sent_.store(0);
    this->audio_bytes_sent_.store(0);
    this->audio_send_failures_.store(0);
    this->streaming_started_ms_ = millis();
    this->waiting_response_started_ms_ = 0;
    this->response_started_ms_ = 0;
    this->session_state_.store(SessionState::STREAMING_MIC);
    this->start_audio_tx_task_();
    if (!this->capture_mode_) {
      g_set_phase.store(3);  // listening phase
    }
    this->log_state_("active_streaming", "ws_connected");
  }

  if (this->session_state_.load() == SessionState::WAITING_RESPONSE &&
      millis() - this->waiting_response_started_ms_ > 20000) {
    this->log_state_("response_wait_timeout", "wait_20s");
    this->reset_session_(false, "response_wait_timeout");
    this->disconnect_requested_.store(true);
    g_set_phase.store(1);
  }

  size_t dropped = this->dropped_audio_bytes_.exchange(0);
  if (dropped > 0) {
    ESP_LOGW(TAG, "Dropped %u bytes of microphone audio", static_cast<unsigned>(dropped));
  }

  // Play audio URL via media_player (triggered from WS thread, executed here on main thread)
  if (this->play_audio_.exchange(false) && this->media_player_ != nullptr) {
    std::string audio_url = this->take_audio_url_();
    if (this->debug_logging_)
      ESP_LOGW(TAG, "[diag] media_play_begin sid=%u url=%s media_state=%d announcing=%d",
               this->session_id_, audio_url.c_str(), static_cast<int>(this->media_player_->state),
               this->media_player_->state == media_player::MediaPlayerState::MEDIA_PLAYER_STATE_ANNOUNCING);
    this->media_player_->make_call()
        .set_media_url(audio_url)
        .set_announcement(true)
        .perform();
    if (this->debug_logging_)
      ESP_LOGW(TAG, "[diag] media_play_perform_done sid=%u media_state=%d announcing=%d",
               this->session_id_, static_cast<int>(this->media_player_->state),
               this->media_player_->state == media_player::MediaPlayerState::MEDIA_PLAYER_STATE_ANNOUNCING);
  } else if (this->play_audio_.load() && this->media_player_ == nullptr) {
    if (this->debug_logging_)
      ESP_LOGW(TAG, "[diag] media_play_pending_without_media_player sid=%u", this->session_id_);
  }
}

void GeminiProxy::start() {
  SessionState expected = SessionState::IDLE;
  if (!this->session_state_.compare_exchange_strong(expected, SessionState::CONNECTING)) {
    ESP_LOGW(TAG, "Already active, ignoring start (state=%s)", state_name_(expected));
    this->log_state_("start_ignored", "not_idle");
    return;
  }

  this->session_id_++;
  this->connect_started_ms_ = millis();
  this->streaming_started_ms_ = 0;
  this->waiting_response_started_ms_ = 0;
  this->response_started_ms_ = 0;
  this->play_audio_.store(false);
  this->stop_mic_requested_.store(false);
  this->capture_mode_ = false;
  this->last_active_diag_ms_ = 0;
  this->last_logged_state_ = SessionState::CONNECTING;
  if (this->ring_buffer_) {
    this->ring_buffer_->reset();
  }
  this->log_state_("start", "wake_word");
  this->connect_();
}

void GeminiProxy::capture(const std::string &sample_type, uint32_t duration_ms) {
  SessionState expected = SessionState::IDLE;
  if (!this->session_state_.compare_exchange_strong(expected, SessionState::CONNECTING)) {
    ESP_LOGW(TAG, "Already active, ignoring capture (state=%s)", state_name_(expected));
    this->log_state_("capture_ignored", "not_idle");
    return;
  }

  this->session_id_++;
  this->connect_started_ms_ = millis();
  this->streaming_started_ms_ = 0;
  this->waiting_response_started_ms_ = 0;
  this->response_started_ms_ = 0;
  this->play_audio_.store(false);
  this->stop_mic_requested_.store(false);
  this->last_active_diag_ms_ = 0;
  this->last_logged_state_ = SessionState::CONNECTING;
  this->capture_mode_ = true;
  this->capture_sample_type_ = sample_type.empty() ? "unknown" : sample_type;
  this->capture_duration_ms_ = duration_ms == 0 ? 2000 : duration_ms;
  if (this->ring_buffer_) {
    this->ring_buffer_->reset();
  }
  ESP_LOGI(TAG, "Starting audio capture sample type=%s duration=%ums",
           this->capture_sample_type_.c_str(), static_cast<unsigned>(this->capture_duration_ms_));
  this->log_state_("capture_start", this->capture_sample_type_.c_str());
  this->connect_();
}

void GeminiProxy::stop() {
  this->log_state_("stop_called", "yaml_or_action");
  if (this->ws_connected_.load()) {
    this->send_binary_(MSG_AUDIO_END, nullptr, 0);
  }
  this->reset_session_(false, "stop_called");
  this->disconnect_requested_.store(true);
  g_set_phase.store(1);
}

void GeminiProxy::connect_() {
  this->disconnect_("connect_replaces_existing");

  esp_websocket_client_config_t config = {};
  config.uri = this->proxy_url_.c_str();
  config.buffer_size = 4096;
  config.task_stack = 8192;
  config.disable_auto_reconnect = true;

  this->ws_ = esp_websocket_client_init(&config);
  if (this->ws_ == nullptr) {
    ESP_LOGE(TAG, "WS init failed");
    this->log_state_("ws_init_failed", "init_null");
    return;
  }

  esp_websocket_register_events(this->ws_, WEBSOCKET_EVENT_ANY,
                                 ws_event_handler_, this);
  esp_err_t err = esp_websocket_client_start(this->ws_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WS start failed: %s", esp_err_to_name(err));
    this->reset_session_(true, "ws_start_failed");
    g_set_phase.store(1);
  } else {
    this->log_state_("ws_start_requested", "connect");
  }
}

void GeminiProxy::disconnect_(const char *reason) {
  this->log_state_("disconnect_begin", reason);
  this->stop_audio_tx_task_(reason);
  if (this->ws_ != nullptr) {
    if (this->debug_logging_)
      ESP_LOGW(TAG, "[diag] websocket_stop_destroy sid=%u reason=%s", this->session_id_, reason);
    std::lock_guard<std::mutex> lock(this->ws_mutex_);
    esp_websocket_client_stop(this->ws_);
    esp_websocket_client_destroy(this->ws_);
    this->ws_ = nullptr;
  }
  this->ws_connected_.store(false);
  this->log_state_("disconnect_end", reason);
}

void GeminiProxy::send_binary_(uint8_t type, const uint8_t *data, size_t len) {
  std::lock_guard<std::mutex> lock(this->ws_mutex_);
  if (!this->ws_connected_.load() || this->ws_ == nullptr)
    return;

  std::vector<uint8_t> frame(1 + len);
  frame[0] = type;
  if (data != nullptr && len > 0) {
    memcpy(&frame[1], data, len);
  }

  // Audio is sent from a dedicated task, so a longer timeout no longer blocks
  // ESPHome's main loop. Short timeouts make esp_websocket_client treat normal
  // backpressure as a fatal transport error.
  TickType_t timeout = pdMS_TO_TICKS(1000);
  esp_err_t err = esp_websocket_client_send_bin(this->ws_,
                                                (const char *) frame.data(),
                                                frame.size(),
                                                timeout);
  if (err < 0) {
    this->audio_send_failures_.fetch_add(1);
    ESP_LOGW(TAG, "WS send failed: %d type=%u len=%u sid=%u state=%s", err, type,
             static_cast<unsigned>(len), this->session_id_, state_name_(this->session_state_.load()));
  } else if (type == MSG_AUDIO_IN) {
    this->audio_chunks_sent_.fetch_add(1);
    this->audio_bytes_sent_.fetch_add(len);
  }
}

void GeminiProxy::reset_session_(bool close_socket, const char *reason) {
  this->log_state_("reset_session_begin", reason);
  this->session_state_.store(SessionState::IDLE);
  this->capture_mode_ = false;
  this->start_mic_requested_.store(false);
  this->stop_mic_requested_.store(true);
  this->stop_audio_tx_task_(reason);
  this->play_audio_.store(false);
  if (this->ring_buffer_) {
    this->ring_buffer_->reset();
  }
  if (close_socket) {
    this->disconnect_(reason);
  }
  this->log_state_("reset_session_end", reason);
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

void GeminiProxy::start_audio_tx_task_() {
  if (this->audio_tx_running_.exchange(true)) {
    if (this->debug_logging_)
      ESP_LOGW(TAG, "[diag] audio_tx_already_running sid=%u", this->session_id_);
    return;
  }
  this->audio_tx_stop_requested_.store(false);
  BaseType_t ok = xTaskCreatePinnedToCore(audio_tx_task_, "gemini_audio_tx", 8192, this, 5, nullptr, 1);
  if (ok != pdPASS) {
    this->audio_tx_running_.store(false);
    this->audio_tx_stop_requested_.store(false);
    ESP_LOGE(TAG, "Audio TX task start failed sid=%u", this->session_id_);
  } else {
    if (this->debug_logging_)
      ESP_LOGW(TAG, "[diag] audio_tx_start sid=%u", this->session_id_);
  }
}

void GeminiProxy::stop_audio_tx_task_(const char *reason) {
  if (!this->audio_tx_running_.load())
    return;
  this->audio_tx_stop_requested_.store(true);
  uint32_t start = millis();
  while (this->audio_tx_running_.load() && millis() - start < 1500) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (this->audio_tx_running_.load()) {
    ESP_LOGW(TAG, "Audio TX stop timeout sid=%u reason=%s", this->session_id_, reason);
  } else {
    if (this->debug_logging_)
      ESP_LOGW(TAG, "[diag] audio_tx_stopped sid=%u reason=%s", this->session_id_, reason);
  }
}

void GeminiProxy::audio_tx_task_(void *arg) {
  auto *self = static_cast<GeminiProxy *>(arg);
  if (self->debug_logging_)
    ESP_LOGW(TAG, "[diag] audio_tx_task_enter sid=%u", self->session_id_);
  uint32_t idle_loops = 0;
  uint32_t started_ms = millis();
  bool capture_active_at_start = self->capture_mode_;
  bool capture_end_sent = false;
  if (capture_active_at_start) {
    ESP_LOGI(TAG, "Capture TX start sid=%u type=%s duration=%ums", self->session_id_,
             self->capture_sample_type_.c_str(), static_cast<unsigned>(self->capture_duration_ms_));
    self->send_binary_(MSG_CAPTURE_START,
                       reinterpret_cast<const uint8_t *>(self->capture_sample_type_.data()),
                       self->capture_sample_type_.size());
  }

  while (!self->audio_tx_stop_requested_.load() &&
         self->session_state_.load() == SessionState::STREAMING_MIC) {
    if (self->capture_mode_ && millis() - started_ms >= self->capture_duration_ms_) {
      self->send_binary_(MSG_AUDIO_END, nullptr, 0);
      capture_end_sent = true;
      self->log_state_("capture_audio_end", "duration_elapsed");
      self->session_state_.store(SessionState::IDLE);
      self->capture_mode_ = false;
      self->stop_mic_requested_.store(true);
      if (self->ring_buffer_) {
        self->ring_buffer_->reset();
      }
      self->disconnect_requested_.store(true);
      g_set_phase.store(1);
      break;
    }

    if (!self->ws_connected_.load() || !self->ring_buffer_) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    size_t available = self->ring_buffer_->available();
    if (available < 2048) {
      if (++idle_loops >= 100) {
        idle_loops = 0;
        if (self->debug_logging_)
          ESP_LOGW(TAG, "[diag] audio_tx_waiting sid=%u available=%u", self->session_id_,
                   static_cast<unsigned>(available));
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    idle_loops = 0;
    size_t to_read = std::min(available, (size_t) 1024);
    uint8_t buf[1024];
    size_t read = self->ring_buffer_->read((void *) buf, to_read, 0);
    if (read > 0) {
      self->send_binary_(MSG_AUDIO_IN, buf, read);
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

  if (capture_active_at_start && !capture_end_sent) {
    ESP_LOGW(TAG, "Capture TX ended early sid=%u state=%s stop=%d ws=%d; sending AUDIO_END",
             self->session_id_, state_name_(self->session_state_.load()),
             self->audio_tx_stop_requested_.load(), self->ws_connected_.load());
    if (self->ws_connected_.load()) {
      self->send_binary_(MSG_AUDIO_END, nullptr, 0);
    }
    self->capture_mode_ = false;
    self->stop_mic_requested_.store(true);
    self->disconnect_requested_.store(true);
  }

  if (self->debug_logging_)
    ESP_LOGW(TAG, "[diag] audio_tx_task_exit sid=%u stop=%d state=%s ws=%d chunks=%u bytes=%u failures=%u",
             self->session_id_, self->audio_tx_stop_requested_.load(),
             state_name_(self->session_state_.load()), self->ws_connected_.load(),
             self->audio_chunks_sent_.load(), self->audio_bytes_sent_.load(),
             self->audio_send_failures_.load());
  self->audio_tx_stop_requested_.store(false);
  self->audio_tx_running_.store(false);
  vTaskDelete(nullptr);
}

void GeminiProxy::ws_event_handler_(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data) {
  auto *self = static_cast<GeminiProxy *>(arg);
  esp_websocket_event_data_t *ws_data = static_cast<esp_websocket_event_data_t *>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      if (self->debug_logging_)
        ESP_LOGI(TAG, "WS connected sid=%u state=%s", self->session_id_,
                 state_name_(self->session_state_.load()));
      self->ws_connected_.store(true);
      if (self->session_state_.load() == SessionState::CONNECTING) {
        self->start_mic_requested_.store(true);
      }
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      self->log_state_("ws_event_disconnected", "websocket_event");
      self->ws_connected_.store(false);
      break;

    case WEBSOCKET_EVENT_DATA:
      if (ws_data->op_code == 0x02 && ws_data->data_len > 0) {
        uint8_t msg_type = static_cast<uint8_t>(ws_data->data_ptr[0]);
        if (self->debug_logging_)
          ESP_LOGW(TAG, "[diag] ws_data sid=%u type=%u len=%u state=%s", self->session_id_, msg_type,
                   static_cast<unsigned>(ws_data->data_len), state_name_(self->session_state_.load()));
        self->on_ws_data_((const uint8_t *) ws_data->data_ptr, ws_data->data_len);
      } else {
        if (self->debug_logging_)
          ESP_LOGW(TAG, "[diag] ws_data_ignored sid=%u opcode=%d len=%u state=%s", self->session_id_,
                   ws_data->op_code, static_cast<unsigned>(ws_data->data_len),
                   state_name_(self->session_state_.load()));
      }
      break;

    case WEBSOCKET_EVENT_ERROR:
      self->log_state_("ws_event_error", "websocket_error");
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
      if (this->session_state_.load() == SessionState::STREAMING_MIC) {
        this->log_state_("state_thinking_before", "proxy_msg");
        this->waiting_response_started_ms_ = millis();
        this->session_state_.store(SessionState::WAITING_RESPONSE);
        this->stop_mic_requested_.store(true);
        if (this->ring_buffer_) {
          this->ring_buffer_->reset();
        }
        g_set_phase.store(4);
        this->log_state_("state_thinking", "proxy_msg");
      } else {
        this->log_state_("state_thinking_ignored", "not_streaming");
      }
      break;

    case MSG_RESPONSE_START: {
      this->log_state_("response_start_before", "proxy_msg");
      this->session_state_.store(SessionState::RESPONDING);
      this->response_started_ms_ = millis();

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
      this->log_state_("response_start", http_url.c_str());
      break;
    }

    case MSG_RESPONSE_END: {
      SessionState state_before_reset = this->session_state_.load();
      this->log_state_("response_end", "proxy_msg");
      this->reset_session_(false, "response_end");
      this->disconnect_requested_.store(true);
      if (state_before_reset != SessionState::RESPONDING) {
        g_set_phase.store(1);
      }
      // For real responses, keep the replying LED phase until the media player
      // reports that playback ended. This avoids a dark gap while the HTTP
      // audio stream drains through the speaker pipeline.
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
