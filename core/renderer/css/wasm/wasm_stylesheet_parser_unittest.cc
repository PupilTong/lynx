// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "core/renderer/css/wasm/wasm_stylesheet_parser.h"

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "core/renderer/css/css_parser_token.h"
#include "core/renderer/css/css_property.h"
#include "core/renderer/css/css_value.h"
#include "core/renderer/css/ng/style/rule_data.h"
#include "core/renderer/css/ng/style/rule_set.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace lynx {
namespace tasm {
namespace css_wasm {
namespace {

struct TokenInput {
  CSSTokenType type;
  std::string_view payload;
};

void AppendU32(uint32_t value, std::vector<uint8_t>* bytes) {
  bytes->push_back(static_cast<uint8_t>(value));
  bytes->push_back(static_cast<uint8_t>(value >> 8));
  bytes->push_back(static_cast<uint8_t>(value >> 16));
  bytes->push_back(static_cast<uint8_t>(value >> 24));
}

std::vector<uint8_t> EncodeTokenStream(std::initializer_list<TokenInput> input) {
  std::vector<uint8_t> records;
  std::vector<uint8_t> payload;
  records.reserve(input.size() * 12);

  for (const auto& token : input) {
    const uint32_t payload_offset = static_cast<uint32_t>(payload.size());
    const uint32_t payload_len = static_cast<uint32_t>(token.payload.size());
    records.push_back(static_cast<uint8_t>(token.type));
    records.push_back(0);
    records.push_back(0);
    records.push_back(0);
    AppendU32(payload_offset, &records);
    AppendU32(payload_len, &records);
    payload.insert(payload.end(), token.payload.begin(), token.payload.end());
  }

  std::vector<uint8_t> bytes = {'L', 'C', 'S', 'T', 1, 16, 12, 0};
  AppendU32(static_cast<uint32_t>(input.size()), &bytes);
  AppendU32(static_cast<uint32_t>(payload.size()), &bytes);
  bytes.insert(bytes.end(), records.begin(), records.end());
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

TEST(CSSTokenStreamViewTest, ReadsRecordsAndPayload) {
  auto bytes = EncodeTokenStream({{CSSTokenType::kIdent, "view"}});
  CSSTokenStreamView stream(bytes.data(), bytes.size());

  ASSERT_TRUE(stream.IsValid()) << stream.Error();
  EXPECT_EQ(1u, stream.TokenCount());
  const auto token = stream.TokenAt(0);
  EXPECT_EQ(CSSTokenType::kIdent, token.type);
  EXPECT_EQ("view", token.payload);
}

TEST(WasmStyleSheetParserTest, ParsesStyleRuleIntoRuleSet) {
  auto bytes = EncodeTokenStream({
      {CSSTokenType::kDelim, "."},
      {CSSTokenType::kIdent, "foo"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kLeftCurlyBracket, "{"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kIdent, "width"},
      {CSSTokenType::kColon, ":"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kDimension, "10px"},
      {CSSTokenType::kSemicolon, ";"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kIdent, "margin"},
      {CSSTokenType::kColon, ":"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kDimension, "1px"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kDimension, "2px"},
      {CSSTokenType::kSemicolon, ";"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kRightCurlyBracket, "}"},
  });
  CSSTokenStreamView stream(bytes.data(), bytes.size());
  CSSParserConfigs configs;

  auto result = ParseStyleSheetTokenStream(stream, configs, true);

  ASSERT_TRUE(result.fragment) << result.error;
  ASSERT_TRUE(result.fragment->enable_css_selector());
  ASSERT_TRUE(result.fragment->enable_css_invalidation());
  const auto& rules = result.fragment->rule_set()->class_rules("foo");
  ASSERT_EQ(1u, rules.size());

  const auto& attributes = rules.front().Rule()->Token()->GetAttributes();
  const auto width = attributes.find(kPropertyIDWidth);
  ASSERT_NE(attributes.end(), width);
  EXPECT_TRUE(width->second.IsPx());
  EXPECT_DOUBLE_EQ(10, width->second.GetNumber());

  const auto margin_top = attributes.find(kPropertyIDMarginTop);
  const auto margin_right = attributes.find(kPropertyIDMarginRight);
  const auto margin_bottom = attributes.find(kPropertyIDMarginBottom);
  const auto margin_left = attributes.find(kPropertyIDMarginLeft);
  ASSERT_NE(attributes.end(), margin_top);
  ASSERT_NE(attributes.end(), margin_right);
  ASSERT_NE(attributes.end(), margin_bottom);
  ASSERT_NE(attributes.end(), margin_left);
  EXPECT_DOUBLE_EQ(1, margin_top->second.GetNumber());
  EXPECT_DOUBLE_EQ(2, margin_right->second.GetNumber());
  EXPECT_DOUBLE_EQ(1, margin_bottom->second.GetNumber());
  EXPECT_DOUBLE_EQ(2, margin_left->second.GetNumber());
}

TEST(WasmStyleSheetParserTest, ParsesAnimationAndKeyframes) {
  auto bytes = EncodeTokenStream({
      {CSSTokenType::kDelim, "."},
      {CSSTokenType::kIdent, "Logo--yew"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kLeftCurlyBracket, "{"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kIdent, "animation"},
      {CSSTokenType::kColon, ":"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kIdent, "Logo--spin"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kIdent, "infinite"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kDimension, "20s"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kIdent, "linear"},
      {CSSTokenType::kSemicolon, ";"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kRightCurlyBracket, "}"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kAtKeyword, "keyframes"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kIdent, "Logo--spin"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kLeftCurlyBracket, "{"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kIdent, "from"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kLeftCurlyBracket, "{"},
      {CSSTokenType::kIdent, "transform"},
      {CSSTokenType::kColon, ":"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kFunction, "rotate"},
      {CSSTokenType::kDimension, "0deg"},
      {CSSTokenType::kRightParentheses, ")"},
      {CSSTokenType::kSemicolon, ";"},
      {CSSTokenType::kRightCurlyBracket, "}"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kIdent, "to"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kLeftCurlyBracket, "{"},
      {CSSTokenType::kIdent, "transform"},
      {CSSTokenType::kColon, ":"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kFunction, "rotate"},
      {CSSTokenType::kDimension, "360deg"},
      {CSSTokenType::kRightParentheses, ")"},
      {CSSTokenType::kSemicolon, ";"},
      {CSSTokenType::kRightCurlyBracket, "}"},
      {CSSTokenType::kWhitespace, " "},
      {CSSTokenType::kRightCurlyBracket, "}"},
  });
  CSSTokenStreamView stream(bytes.data(), bytes.size());
  CSSParserConfigs configs;

  auto result = ParseStyleSheetTokenStream(stream, configs, true);

  ASSERT_TRUE(result.fragment) << result.error;
  const auto& rules = result.fragment->rule_set()->class_rules("Logo--yew");
  ASSERT_EQ(1u, rules.size());
  const auto& attributes = rules.front().Rule()->Token()->GetAttributes();
  EXPECT_NE(attributes.end(), attributes.find(kPropertyIDAnimationName));
  EXPECT_NE(attributes.end(), attributes.find(kPropertyIDAnimationDuration));
  EXPECT_NE(attributes.end(), attributes.find(kPropertyIDAnimationTimingFunction));
  EXPECT_NE(attributes.end(), attributes.find(kPropertyIDAnimationIterationCount));

  auto* keyframes = result.fragment->GetKeyframesRule(base::String("Logo--spin"));
  ASSERT_NE(nullptr, keyframes);
  auto& content = keyframes->GetKeyframesContent();
  ASSERT_EQ(2u, content.size());
  auto from = content.find(0);
  ASSERT_NE(content.end(), from);
  ASSERT_NE(nullptr, from->second);
  EXPECT_NE(from->second->end(), from->second->find(kPropertyIDTransform));
  auto to = content.find(1);
  ASSERT_NE(content.end(), to);
  ASSERT_NE(nullptr, to->second);
  EXPECT_NE(to->second->end(), to->second->find(kPropertyIDTransform));
}

}  // namespace
}  // namespace css_wasm
}  // namespace tasm
}  // namespace lynx
