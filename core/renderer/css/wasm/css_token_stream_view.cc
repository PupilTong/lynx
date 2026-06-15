// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "core/renderer/css/wasm/css_token_stream_view.h"

#include <limits>

namespace lynx {
namespace tasm {
namespace css_wasm {

CSSTokenStreamView::CSSTokenStreamView(const uint8_t* data, size_t size)
    : data_(data), size_(size) {
  if (!data_) {
    SetError("CSS token stream data is null");
    return;
  }
  if (size_ < kHeaderLen) {
    SetError("CSS token stream header is truncated");
    return;
  }
  if (ReadU32(0) != kMagic) {
    SetError("CSS token stream magic mismatch");
    return;
  }
  if (data_[4] != kVersion) {
    SetError("CSS token stream version is unsupported");
    return;
  }
  if (data_[5] != kHeaderLen || data_[6] != kRecordLen) {
    SetError("CSS token stream layout is unsupported");
    return;
  }

  token_count_ = ReadU32(8);
  payload_len_ = ReadU32(12);
  records_offset_ = kHeaderLen;

  if (token_count_ >
      (std::numeric_limits<size_t>::max() - records_offset_) / kRecordLen) {
    SetError("CSS token stream record section is too large");
    return;
  }
  payload_offset_ = records_offset_ + token_count_ * kRecordLen;
  if (payload_len_ > std::numeric_limits<size_t>::max() - payload_offset_) {
    SetError("CSS token stream payload section is too large");
    return;
  }
  if (payload_offset_ + payload_len_ > size_) {
    SetError("CSS token stream payload is truncated");
    return;
  }
  valid_ = true;
}

CSSTokenRef CSSTokenStreamView::TokenAt(uint32_t index) const {
  if (!valid_ || index >= token_count_) {
    return {};
  }

  const size_t offset = records_offset_ + index * kRecordLen;
  const uint32_t payload_offset = ReadU32(offset + 4);
  const uint32_t payload_len = ReadU32(offset + 8);
  if (payload_offset > payload_len_ || payload_len > payload_len_ ||
      payload_offset > payload_len_ - payload_len) {
    return {};
  }

  return {
      static_cast<CSSTokenType>(data_[offset]),
      data_[offset + 1],
      std::string_view(
          reinterpret_cast<const char*>(data_ + payload_offset_ +
                                        payload_offset),
          payload_len),
  };
}

uint32_t CSSTokenStreamView::ReadU32(size_t offset) const {
  return static_cast<uint32_t>(data_[offset]) |
         (static_cast<uint32_t>(data_[offset + 1]) << 8) |
         (static_cast<uint32_t>(data_[offset + 2]) << 16) |
         (static_cast<uint32_t>(data_[offset + 3]) << 24);
}

void CSSTokenStreamView::SetError(const char* error) {
  error_ = error;
  valid_ = false;
}

}  // namespace css_wasm
}  // namespace tasm
}  // namespace lynx
