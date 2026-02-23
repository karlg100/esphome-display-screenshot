// display_capture -- implementation file.
//
// IMPORTANT: The #define protected public hack MUST be the very first thing
// in this file, before any #include. It makes DisplayBuffer::buffer_ accessible
// in this translation unit only. The .h file uses forward declarations so it
// compiles cleanly without this hack.
//
// This works because each .cpp is a separate translation unit with its own
// #pragma once state. The macro only affects headers included in THIS file.

// --- Step 1: Expose protected members for buffer access ---
#define protected public
#include "esphome/components/display/display_buffer.h"
#include "esphome/components/display/display.h"
#undef protected

#ifdef USE_RPI_DPI_RGB
#include "esphome/components/rpi_dpi_rgb/rpi_dpi_rgb.h"
#endif

// --- Step 2: Our own header (uses only forward declarations, no buffer access) ---
#include "display_capture.h"

// --- Step 3: Globals support (conditionally compiled) ---
// DISPLAY_CAPTURE_USE_GLOBALS is defined by __init__.py when page_global or
// sleep_global are configured. Without this guard, the build fails when globals
// aren't used because ESPHome doesn't copy globals headers to the build directory.
#ifdef DISPLAY_CAPTURE_USE_GLOBALS
#include "esphome/components/globals/globals_component.h"
#endif

#include <cstring>

namespace esphome {
namespace display_capture {

// ============================================================================
// Component lifecycle
// ============================================================================

void DisplayCaptureHandler::setup() {
  // Binary semaphore for HTTP task <-> main loop synchronization.
  // The HTTP handler takes it (blocks), the main loop gives it (unblocks).
  this->semaphore_ = xSemaphoreCreateBinary();
  this->stream_chunk_done_ = xSemaphoreCreateBinary();
  this->base_->init();
  this->base_->add_handler(this);

  const char *mode_str = "single";
  if (this->page_mode_ == NATIVE_PAGES)
    mode_str = "native_pages";
  else if (this->page_mode_ == GLOBAL_PAGES)
    mode_str = "global_pages";

  const char *backend_str = "display_buffer";
  if (this->backend_ == BACKEND_RPI_DPI_RGB)
    backend_str = "rpi_dpi_rgb";

  int pages = this->get_page_count();
  if (pages >= 0) {
    ESP_LOGI(TAG, "Display capture registered at /screenshot (mode: %s, backend: %s, pages: %d)", mode_str, backend_str, pages);
  } else {
    ESP_LOGI(TAG, "Display capture registered at /screenshot (mode: %s, backend: %s, pages: unknown)", mode_str, backend_str);
  }
}

int DisplayCaptureHandler::get_page_count() const {
  switch (this->page_mode_) {
    case NATIVE_PAGES:
      return this->pages_.size();
    case GLOBAL_PAGES:
      // Global mode doesn't inherently know the page count -- use page_names
      // as the source of truth if provided, otherwise return -1 (unknown).
      // The /info endpoint omits the "pages" field when the count is unknown,
      // so clients can distinguish "unknown" from "zero pages".
      return this->page_names_.empty() ? -1 : this->page_names_.size();
    default:
      return 1;
  }
}

// ============================================================================
// Main loop -- runs on the ESPHome main task
// ============================================================================
//
// This is where all display buffer access happens. The HTTP task sets
// request_pending_ and blocks on the semaphore. We do the work here
// (where it's safe to touch display state) and signal when done.
//
// Sequence:
//   1. Prepare stream snapshot state on main loop (safe display access)
//   2. HTTP task requests chunk ranges via shared state
//   3. Main loop fills chunks from framebuffer and signals completion
//   4. After stream ends, main loop restores page/sleep state

void DisplayCaptureHandler::loop() {
  if (this->request_pending_) {
    this->request_pending_ = false;
    this->prepare_stream_capture_();
    xSemaphoreGive(this->semaphore_);
  }

  if (this->stream_chunk_requested_) {
    this->stream_chunk_requested_ = false;
    this->fill_stream_chunk_();
    xSemaphoreGive(this->stream_chunk_done_);
  }

  if (this->stream_restore_pending_) {
    this->finish_stream_capture_();
    this->stream_restore_pending_ = false;
  }
}

// ============================================================================
// HTTP handlers -- run on the web server's FreeRTOS task
// ============================================================================

void DisplayCaptureHandler::handle_screenshot_(AsyncWebServerRequest *req) {
  int requested_page = -1;
  if (req->hasParam("page")) {
    requested_page = atoi(req->arg("page").c_str());
  }

  if (this->stream_in_progress_) {
    req->send(429, "text/plain", "Screenshot already in progress");
    return;
  }

  this->stream_in_progress_ = true;
  this->requested_page_ = requested_page;
  this->stream_failed_ = false;
  this->stream_ready_ = false;
  this->stream_restore_pending_ = false;
  this->request_pending_ = true;

  if (xSemaphoreTake(this->semaphore_, pdMS_TO_TICKS(5000)) == pdTRUE) {
    if (this->stream_ready_ && !this->stream_failed_) {
      auto *response = req->beginChunkedResponse(
          "image/bmp",
          [this](uint8_t *buffer, size_t max_len, size_t index) -> size_t {
            if (!this->stream_ready_ || this->stream_failed_)
              return 0;
            if (index >= this->stream_file_size_) {
              this->stream_restore_pending_ = true;
              return 0;
            }

            size_t len = this->stream_file_size_ - index;
            if (len > max_len)
              len = max_len;
            if (len > STREAM_CHUNK_SIZE)
              len = STREAM_CHUNK_SIZE;

            // Clear any stale signal from a previous chunk/request.
            (void) xSemaphoreTake(this->stream_chunk_done_, 0);
            this->stream_chunk_index_ = index;
            this->stream_chunk_len_ = len;
            this->stream_chunk_requested_ = true;

            if (xSemaphoreTake(this->stream_chunk_done_, pdMS_TO_TICKS(5000)) != pdTRUE) {
              this->stream_failed_ = true;
              this->stream_restore_pending_ = true;
              return 0;
            }

            if (this->stream_chunk_filled_ == 0) {
              this->stream_failed_ = true;
              this->stream_restore_pending_ = true;
              return 0;
            }

            memcpy(buffer, this->stream_chunk_buf_, this->stream_chunk_filled_);
            return this->stream_chunk_filled_;
          });
      response->addHeader("Cache-Control", "no-cache");
      req->send(response);
    } else {
      this->stream_restore_pending_ = true;
      req->send(500, "text/plain", "Failed to capture screenshot");
    }
  } else {
    this->request_pending_ = false;
    this->stream_in_progress_ = false;
    this->stream_restore_pending_ = true;
    req->send(504, "text/plain", "Screenshot capture timed out");
  }
}

/// Info handler: returns JSON metadata about the display and page configuration.
/// Runs synchronously on the HTTP task -- all data is immutable after setup.
///
/// Response format:
///   {"pages":3,"width":320,"height":240,"mode":"native_pages","page_names":["Main","Graph","Settings"]}
void DisplayCaptureHandler::handle_info_(AsyncWebServerRequest *req) {
  int screen_w = this->display_->get_width();
  int screen_h = this->display_->get_height();
  int page_count = this->get_page_count();

  const char *mode_str = "single";
  if (this->page_mode_ == NATIVE_PAGES)
    mode_str = "native_pages";
  else if (this->page_mode_ == GLOBAL_PAGES)
    mode_str = "global_pages";

  std::string json = "{";
  json += "\"width\":" + std::to_string(screen_w);
  json += ",\"height\":" + std::to_string(screen_h);
  // Only include "pages" when the count is known (>= 0).
  // In global_pages mode without page_names, the count is unknown (-1)
  // and we omit the field so clients can distinguish "unknown" from "zero".
  if (page_count >= 0) {
    json += ",\"pages\":" + std::to_string(page_count);
  }
  json += ",\"mode\":\"";
  json += mode_str;
  json += "\"";

  if (!this->page_names_.empty()) {
    json += ",\"page_names\":[";
    for (size_t i = 0; i < this->page_names_.size(); i++) {
      if (i > 0)
        json += ",";
      json += "\"";
      for (char c : this->page_names_[i]) {
        switch (c) {
          case '"':  json += "\\\""; break;
          case '\\': json += "\\\\"; break;
          case '\n': json += "\\n"; break;
          case '\r': json += "\\r"; break;
          case '\t': json += "\\t"; break;
          case '\b': json += "\\b"; break;
          case '\f': json += "\\f"; break;
          default:
            // Escape remaining control characters (U+0000 through U+001F)
            if (static_cast<unsigned char>(c) < 0x20) {
              char buf[8];
              snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
              json += buf;
            } else {
              json += c;
            }
            break;
        }
      }
      json += "\"";
    }
    json += "]";
  }

  json += "}";

  req->send(200, "application/json", json.c_str());
}

// ============================================================================
// Streaming capture helpers -- all framebuffer access on main loop
// ============================================================================

void DisplayCaptureHandler::prepare_stream_capture_() {
  this->stream_ready_ = false;
  this->stream_failed_ = false;
  this->stream_fb_ = nullptr;
  this->stream_chunk_filled_ = 0;
  this->stream_was_sleeping_ = false;
  this->stream_page_switched_ = false;

#ifdef DISPLAY_CAPTURE_USE_GLOBALS
  if (this->sleep_global_ != nullptr && this->sleep_global_->value()) {
    this->stream_was_sleeping_ = true;
    this->sleep_global_->value() = false;
  }
#endif

  if (this->requested_page_ >= 0) {
    switch (this->page_mode_) {
      case NATIVE_PAGES: {
        int idx = this->requested_page_;
        if (idx >= 0 && idx < (int) this->pages_.size()) {
          this->saved_native_page_ = this->display_->get_active_page();
          this->display_->show_page(this->pages_[idx]);
          this->stream_page_switched_ = true;
        }
        break;
      }
#ifdef DISPLAY_CAPTURE_USE_GLOBALS
      case GLOBAL_PAGES: {
        this->saved_global_page_ = this->page_global_->value();
        if (this->saved_global_page_ != this->requested_page_) {
          this->page_global_->value() = this->requested_page_;
          this->stream_page_switched_ = true;
        }
        break;
      }
#endif
      default:
        break;
    }
  }

  // Render requested state before streaming from framebuffer.
  this->display_->update();

  this->stream_screen_w_ = this->display_->get_width();
  this->stream_screen_h_ = this->display_->get_height();
  this->stream_native_w_ = this->display_->get_native_width();
  this->stream_native_h_ = this->display_->get_native_height();
  this->stream_rotation_ = (int) this->display_->get_rotation();
  this->stream_row_stride_ = ((this->stream_screen_w_ * 3 + 3) / 4) * 4;
  this->stream_file_size_ = 54 + (size_t) this->stream_row_stride_ * this->stream_screen_h_;

  memset(this->stream_header_, 0, sizeof(this->stream_header_));
  this->stream_header_[0] = 'B';
  this->stream_header_[1] = 'M';
  write_le32_(this->stream_header_ + 2, this->stream_file_size_);
  write_le32_(this->stream_header_ + 10, 54);
  write_le32_(this->stream_header_ + 14, 40);
  write_le32_(this->stream_header_ + 18, this->stream_screen_w_);
  write_le32_(this->stream_header_ + 22, this->stream_screen_h_);
  write_le16_(this->stream_header_ + 26, 1);
  write_le16_(this->stream_header_ + 28, 24);
  write_le32_(this->stream_header_ + 34, (uint32_t) this->stream_row_stride_ * this->stream_screen_h_);

  if (this->backend_ == BACKEND_RPI_DPI_RGB) {
#ifdef USE_RPI_DPI_RGB
    auto *rgb_display = static_cast<rpi_dpi_rgb::RpiDpiRgb *>(this->display_);
    if (rgb_display->handle_ == nullptr) {
      ESP_LOGE(TAG, "rpi_dpi_rgb handle is null");
      this->stream_failed_ = true;
      return;
    }
    void *fb = nullptr;
    esp_err_t err = esp_lcd_rgb_panel_get_frame_buffer(rgb_display->handle_, 1, &fb);
    if (err != ESP_OK || fb == nullptr) {
      ESP_LOGE(TAG, "Failed to get rpi_dpi_rgb frame buffer (%d)", err);
      this->stream_failed_ = true;
      return;
    }
    this->stream_fb_ = static_cast<uint8_t *>(fb);
#else
    ESP_LOGE(TAG, "rpi_dpi_rgb backend requested but USE_RPI_DPI_RGB is not enabled in this build");
    this->stream_failed_ = true;
    return;
#endif
  } else {
    auto *display_buffer = static_cast<display::DisplayBuffer *>(this->display_);
    this->stream_fb_ = display_buffer->buffer_;
  }

  if (this->stream_fb_ == nullptr) {
    ESP_LOGE(TAG, "Frame buffer pointer is null");
    this->stream_failed_ = true;
    return;
  }

  this->stream_ready_ = true;
}

void DisplayCaptureHandler::fill_stream_chunk_() {
  if (!this->stream_ready_ || this->stream_failed_ || this->stream_fb_ == nullptr) {
    this->stream_chunk_filled_ = 0;
    return;
  }

  size_t index = this->stream_chunk_index_;
  size_t len = this->stream_chunk_len_;
  if (index >= this->stream_file_size_) {
    this->stream_chunk_filled_ = 0;
    return;
  }
  if (index + len > this->stream_file_size_) {
    len = this->stream_file_size_ - index;
  }

  for (size_t i = 0; i < len; i++) {
    size_t file_pos = index + i;
    uint8_t out = 0;

    if (file_pos < 54) {
      out = this->stream_header_[file_pos];
    } else {
      size_t pixel_pos = file_pos - 54;
      int row = pixel_pos / this->stream_row_stride_;
      int row_off = pixel_pos % this->stream_row_stride_;

      if (row_off < this->stream_screen_w_ * 3) {
        int sx = row_off / 3;
        int ch = row_off % 3;  // 0=B,1=G,2=R
        int sy = this->stream_screen_h_ - 1 - row;

        int bx = sx;
        int by = sy;
        switch (this->stream_rotation_) {
          case display::DISPLAY_ROTATION_90_DEGREES:
            bx = this->stream_native_w_ - 1 - sy;
            by = sx;
            break;
          case display::DISPLAY_ROTATION_180_DEGREES:
            bx = this->stream_native_w_ - 1 - sx;
            by = this->stream_native_h_ - 1 - sy;
            break;
          case display::DISPLAY_ROTATION_270_DEGREES:
            bx = sy;
            by = this->stream_native_h_ - 1 - sx;
            break;
          default:
            break;
        }

        uint32_t pos = (by * this->stream_native_w_ + bx) * 2;
        uint8_t high = this->stream_fb_[pos];
        uint8_t low = this->stream_fb_[pos + 1];
        uint8_t r5 = high >> 3;
        uint8_t g6 = ((high & 0x07) << 3) | (low >> 5);
        uint8_t b5 = low & 0x1F;
        uint8_t r = (r5 * 255) / 31;
        uint8_t g = (g6 * 255) / 63;
        uint8_t b = (b5 * 255) / 31;
        out = ch == 0 ? b : (ch == 1 ? g : r);
      }
    }

    this->stream_chunk_buf_[i] = out;
  }

  this->stream_chunk_filled_ = len;
}

void DisplayCaptureHandler::finish_stream_capture_() {
  if (this->stream_page_switched_) {
    switch (this->page_mode_) {
      case NATIVE_PAGES:
        if (this->saved_native_page_ != nullptr) {
          this->display_->show_page(const_cast<display::DisplayPage *>(this->saved_native_page_));
        }
        break;
#ifdef DISPLAY_CAPTURE_USE_GLOBALS
      case GLOBAL_PAGES:
        this->page_global_->value() = this->saved_global_page_;
        break;
#endif
      default:
        break;
    }
  }

#ifdef DISPLAY_CAPTURE_USE_GLOBALS
  if (this->stream_was_sleeping_) {
    this->sleep_global_->value() = true;
  }
#endif

  if (this->stream_page_switched_ || this->stream_was_sleeping_) {
    this->display_->update();
  }

  this->stream_ready_ = false;
  this->stream_in_progress_ = false;
  this->stream_fb_ = nullptr;
}

}  // namespace display_capture
}  // namespace esphome
