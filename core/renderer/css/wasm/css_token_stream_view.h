// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef CORE_RENDERER_CSS_WASM_CSS_TOKEN_STREAM_VIEW_H_
#define CORE_RENDERER_CSS_WASM_CSS_TOKEN_STREAM_VIEW_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lynx {
namespace tasm {
namespace css_wasm {

enum class CSSTokenType : uint8_t {
  kEof = 0,
  kIdent = 1,
  kFunction = 2,
  kAtKeyword = 3,
  kHash = 4,
  kString = 5,
  kBadString = 6,
  kUrl = 7,
  kBadUrl = 8,
  kDelim = 9,
  kNumber = 10,
  kPercentage = 11,
  kDimension = 12,
  kWhitespace = 13,
  kCdo = 14,
  kCdc = 15,
  kColon = 16,
  kSemicolon = 17,
  kComma = 18,
  kLeftSquareBracket = 19,
  kRightSquareBracket = 20,
  kLeftParentheses = 21,
  kRightParentheses = 22,
  kLeftCurlyBracket = 23,
  kRightCurlyBracket = 24,
  kComment = 25,
};

struct CSSTokenRef {
  CSSTokenType type = CSSTokenType::kEof;
  uint8_t flags = 0;
  std::string_view payload;
};

class CSSTokenStreamView {
 public:
  CSSTokenStreamView() = default;
  CSSTokenStreamView(const uint8_t* data, size_t size);

  bool IsValid() const { return valid_; }
  const char* Error() const { return error_; }
  uint32_t TokenCount() const { return token_count_; }

  CSSTokenRef TokenAt(uint32_t index) const;

 private:
  static constexpr uint32_t kMagic = 0x5453434c;  // "LCST", little-endian.
  static constexpr uint8_t kVersion = 1;
  static constexpr uint8_t kHeaderLen = 16;
  static constexpr uint8_t kRecordLen = 12;

  uint32_t ReadU32(size_t offset) const;
  void SetError(const char* error);

  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  uint32_t token_count_ = 0;
  uint32_t payload_len_ = 0;
  size_t records_offset_ = 0;
  size_t payload_offset_ = 0;
  bool valid_ = false;
  const char* error_ = nullptr;
};

}  // namespace css_wasm
}  // namespace tasm
}  // namespace lynx

#endif  // CORE_RENDERER_CSS_WASM_CSS_TOKEN_STREAM_VIEW_H_
