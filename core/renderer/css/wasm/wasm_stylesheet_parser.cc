// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "core/renderer/css/wasm/wasm_stylesheet_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/include/fml/memory/ref_counted.h"
#include "base/include/value/base_value.h"
#include "core/renderer/css/css_color.h"
#include "core/renderer/css/css_keyframes_token.h"
#include "core/renderer/css/css_parser_token.h"
#include "core/renderer/css/css_property.h"
#include "core/renderer/css/css_value.h"
#include "core/renderer/css/ng/selector/lynx_css_selector_list.h"
#include "core/renderer/css/ng/style/style_rule.h"
#include "core/renderer/css/unit_handler.h"
#include "core/renderer/starlight/style/css_type.h"

namespace lynx {
namespace tasm {
namespace css_wasm {

namespace {

using css::LynxCSSSelector;

bool IsWhitespace(CSSTokenType type) {
  return type == CSSTokenType::kWhitespace || type == CSSTokenType::kComment;
}

std::string ToLower(std::string_view value) {
  std::string lower(value);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return lower;
}

class Cursor {
 public:
  explicit Cursor(const CSSTokenStreamView& stream) : stream_(stream) {}

  bool AtEnd() const { return index_ >= stream_.TokenCount(); }
  uint32_t Index() const { return index_; }
  void SetIndex(uint32_t index) { index_ = index; }

  CSSTokenRef Peek(uint32_t offset = 0) const {
    const uint32_t index = index_ + offset;
    return index < stream_.TokenCount() ? stream_.TokenAt(index)
                                        : CSSTokenRef{};
  }

  CSSTokenRef Consume() {
    CSSTokenRef token = Peek();
    if (!AtEnd()) {
      ++index_;
    }
    return token;
  }

  void ConsumeWhitespace() {
    while (!AtEnd() && IsWhitespace(Peek().type)) {
      ++index_;
    }
  }

 private:
  const CSSTokenStreamView& stream_;
  uint32_t index_ = 0;
};

bool IsCloseTokenFor(CSSTokenType open, CSSTokenType close) {
  return (open == CSSTokenType::kFunction &&
          close == CSSTokenType::kRightParentheses) ||
         (open == CSSTokenType::kLeftParentheses &&
          close == CSSTokenType::kRightParentheses) ||
         (open == CSSTokenType::kLeftSquareBracket &&
          close == CSSTokenType::kRightSquareBracket) ||
         (open == CSSTokenType::kLeftCurlyBracket &&
          close == CSSTokenType::kRightCurlyBracket);
}

bool IsOpenBlock(CSSTokenType type) {
  return type == CSSTokenType::kFunction ||
         type == CSSTokenType::kLeftParentheses ||
         type == CSSTokenType::kLeftSquareBracket ||
         type == CSSTokenType::kLeftCurlyBracket;
}

uint32_t SkipComponentValue(const CSSTokenStreamView& stream, uint32_t index,
                            uint32_t end) {
  if (index >= end) {
    return index;
  }
  const CSSTokenType open = stream.TokenAt(index).type;
  if (!IsOpenBlock(open)) {
    return index + 1;
  }
  uint32_t depth = 1;
  ++index;
  while (index < end && depth > 0) {
    const CSSTokenType type = stream.TokenAt(index).type;
    if (IsOpenBlock(type)) {
      ++depth;
    } else if (IsCloseTokenFor(open, type) ||
               type == CSSTokenType::kRightParentheses ||
               type == CSSTokenType::kRightSquareBracket ||
               type == CSSTokenType::kRightCurlyBracket) {
      --depth;
    }
    ++index;
  }
  return index;
}

uint32_t FindTopLevelToken(const CSSTokenStreamView& stream, uint32_t start,
                           uint32_t end, CSSTokenType target) {
  uint32_t index = start;
  while (index < end) {
    const auto token = stream.TokenAt(index);
    if (token.type == target) {
      return index;
    }
    if (IsOpenBlock(token.type)) {
      index = SkipComponentValue(stream, index, end);
      continue;
    }
    ++index;
  }
  return end;
}

bool ConsumeNumber(std::string_view payload, double* out) {
  std::string value(payload);
  char* end = nullptr;
  const double number = std::strtod(value.c_str(), &end);
  if (end == value.c_str() || !end || *end != '\0') {
    return false;
  }
  *out = number;
  return true;
}

struct Dimension {
  double value = 0;
  std::string unit;
};

bool ConsumeDimension(std::string_view payload, Dimension* out) {
  std::string value(payload);
  char* end = nullptr;
  const double number = std::strtod(value.c_str(), &end);
  if (end == value.c_str() || !end || *end == '\0') {
    return false;
  }
  out->value = number;
  out->unit = ToLower(end);
  return true;
}

bool LengthPatternForUnit(std::string_view unit, CSSValuePattern* pattern) {
  if (unit == "px") {
    *pattern = CSSValuePattern::PX;
  } else if (unit == "rpx") {
    *pattern = CSSValuePattern::RPX;
  } else if (unit == "em") {
    *pattern = CSSValuePattern::EM;
  } else if (unit == "rem") {
    *pattern = CSSValuePattern::REM;
  } else if (unit == "vh") {
    *pattern = CSSValuePattern::VH;
  } else if (unit == "vw") {
    *pattern = CSSValuePattern::VW;
  } else if (unit == "ppx") {
    *pattern = CSSValuePattern::PPX;
  } else {
    return false;
  }
  return true;
}

bool IsLengthProperty(CSSPropertyID id) {
  switch (id) {
    case kPropertyIDTop:
    case kPropertyIDLeft:
    case kPropertyIDRight:
    case kPropertyIDBottom:
    case kPropertyIDWidth:
    case kPropertyIDHeight:
    case kPropertyIDMaxWidth:
    case kPropertyIDMinWidth:
    case kPropertyIDMaxHeight:
    case kPropertyIDMinHeight:
    case kPropertyIDPaddingLeft:
    case kPropertyIDPaddingRight:
    case kPropertyIDPaddingTop:
    case kPropertyIDPaddingBottom:
    case kPropertyIDMarginLeft:
    case kPropertyIDMarginRight:
    case kPropertyIDMarginTop:
    case kPropertyIDMarginBottom:
    case kPropertyIDFontSize:
    case kPropertyIDLineHeight:
    case kPropertyIDFlexBasis:
      return true;
    default:
      return false;
  }
}

bool ParseLengthToken(CSSTokenRef token, CSSValue* out) {
  if (token.type == CSSTokenType::kIdent) {
    const std::string ident = ToLower(token.payload);
    if (ident == "auto") {
      *out = CSSValue(starlight::LengthValueType::kAuto);
      return true;
    }
    if (ident == "max-content") {
      *out = CSSValue(starlight::LengthValueType::kMaxContent);
      return true;
    }
    if (ident == "fit-content") {
      *out = CSSValue(starlight::LengthValueType::kFitContent);
      return true;
    }
    return false;
  }

  double number = 0;
  if (token.type == CSSTokenType::kNumber) {
    if (!ConsumeNumber(token.payload, &number)) {
      return false;
    }
    *out = CSSValue(number, CSSValuePattern::NUMBER);
    return true;
  }
  if (token.type == CSSTokenType::kPercentage) {
    if (!ConsumeNumber(token.payload, &number)) {
      return false;
    }
    *out = CSSValue(number, CSSValuePattern::PERCENT);
    return true;
  }
  if (token.type == CSSTokenType::kDimension) {
    Dimension dimension;
    CSSValuePattern pattern = CSSValuePattern::EMPTY;
    if (!ConsumeDimension(token.payload, &dimension) ||
        !LengthPatternForUnit(dimension.unit, &pattern)) {
      return false;
    }
    *out = CSSValue(dimension.value, pattern);
    return true;
  }
  return false;
}

bool ParseColorToken(CSSTokenRef token, CSSValue* out) {
  CSSColor color;
  if (token.type == CSSTokenType::kIdent) {
    if (!CSSColor::ParseNamedColor(std::string(token.payload), color)) {
      return false;
    }
  } else if (token.type == CSSTokenType::kHash) {
    std::string hex("#");
    hex.append(token.payload.data(), token.payload.size());
    if (!CSSColor::Parse(hex, color)) {
      return false;
    }
  } else {
    return false;
  }
  *out = CSSValue(static_cast<uint32_t>(color.Cast()), CSSValuePattern::NUMBER);
  return true;
}

bool SetEnumValue(CSSPropertyID id, std::string_view ident, CSSValue* out) {
  const std::string value = ToLower(ident);
  switch (id) {
    case kPropertyIDDisplay:
      if (value == "none") {
        *out = CSSValue(starlight::DisplayType::kNone);
      } else if (value == "flex") {
        *out = CSSValue(starlight::DisplayType::kFlex);
      } else if (value == "grid") {
        *out = CSSValue(starlight::DisplayType::kGrid);
      } else if (value == "linear") {
        *out = CSSValue(starlight::DisplayType::kLinear);
      } else if (value == "relative") {
        *out = CSSValue(starlight::DisplayType::kRelative);
      } else if (value == "block") {
        *out = CSSValue(starlight::DisplayType::kBlock);
      } else if (value == "auto") {
        *out = CSSValue(starlight::DisplayType::kAuto);
      } else {
        return false;
      }
      return true;
    case kPropertyIDPosition:
      if (value == "absolute") {
        *out = CSSValue(starlight::PositionType::kAbsolute);
      } else if (value == "relative") {
        *out = CSSValue(starlight::PositionType::kRelative);
      } else if (value == "fixed") {
        *out = CSSValue(starlight::PositionType::kFixed);
      } else if (value == "sticky") {
        *out = CSSValue(starlight::PositionType::kSticky);
      } else {
        return false;
      }
      return true;
    case kPropertyIDBoxSizing:
      if (value == "border-box") {
        *out = CSSValue(starlight::BoxSizingType::kBorderBox);
      } else if (value == "content-box") {
        *out = CSSValue(starlight::BoxSizingType::kContentBox);
      } else if (value == "auto") {
        *out = CSSValue(starlight::BoxSizingType::kAuto);
      } else {
        return false;
      }
      return true;
    case kPropertyIDVisibility:
      if (value == "hidden") {
        *out = CSSValue(starlight::VisibilityType::kHidden);
      } else if (value == "visible") {
        *out = CSSValue(starlight::VisibilityType::kVisible);
      } else if (value == "none") {
        *out = CSSValue(starlight::VisibilityType::kNone);
      } else if (value == "collapse") {
        *out = CSSValue(starlight::VisibilityType::kCollapse);
      } else {
        return false;
      }
      return true;
    case kPropertyIDFlexDirection:
      if (value == "column") {
        *out = CSSValue(starlight::FlexDirectionType::kColumn);
      } else if (value == "row") {
        *out = CSSValue(starlight::FlexDirectionType::kRow);
      } else if (value == "row-reverse") {
        *out = CSSValue(starlight::FlexDirectionType::kRowReverse);
      } else if (value == "column-reverse") {
        *out = CSSValue(starlight::FlexDirectionType::kColumnReverse);
      } else {
        return false;
      }
      return true;
    case kPropertyIDFlexWrap:
      if (value == "wrap") {
        *out = CSSValue(starlight::FlexWrapType::kWrap);
      } else if (value == "nowrap") {
        *out = CSSValue(starlight::FlexWrapType::kNowrap);
      } else if (value == "wrap-reverse") {
        *out = CSSValue(starlight::FlexWrapType::kWrapReverse);
      } else {
        return false;
      }
      return true;
    case kPropertyIDAlignItems:
    case kPropertyIDAlignSelf:
      if (value == "auto") {
        *out = CSSValue(starlight::FlexAlignType::kAuto);
      } else if (value == "stretch") {
        *out = CSSValue(starlight::FlexAlignType::kStretch);
      } else if (value == "flex-start") {
        *out = CSSValue(starlight::FlexAlignType::kFlexStart);
      } else if (value == "flex-end") {
        *out = CSSValue(starlight::FlexAlignType::kFlexEnd);
      } else if (value == "center") {
        *out = CSSValue(starlight::FlexAlignType::kCenter);
      } else if (value == "baseline") {
        *out = CSSValue(starlight::FlexAlignType::kBaseline);
      } else if (value == "start") {
        *out = CSSValue(starlight::FlexAlignType::kStart);
      } else if (value == "end") {
        *out = CSSValue(starlight::FlexAlignType::kEnd);
      } else {
        return false;
      }
      return true;
    case kPropertyIDJustifyContent:
      if (value == "flex-start" || value == "start") {
        *out = CSSValue(starlight::JustifyContentType::kFlexStart);
      } else if (value == "center") {
        *out = CSSValue(starlight::JustifyContentType::kCenter);
      } else if (value == "flex-end" || value == "end") {
        *out = CSSValue(starlight::JustifyContentType::kFlexEnd);
      } else if (value == "space-between") {
        *out = CSSValue(starlight::JustifyContentType::kSpaceBetween);
      } else if (value == "space-around") {
        *out = CSSValue(starlight::JustifyContentType::kSpaceAround);
      } else if (value == "space-evenly") {
        *out = CSSValue(starlight::JustifyContentType::kSpaceEvenly);
      } else if (value == "stretch") {
        *out = CSSValue(starlight::JustifyContentType::kStretch);
      } else {
        return false;
      }
      return true;
    default:
      return false;
  }
}

bool ParseSingleValue(CSSPropertyID id, CSSTokenRef token, CSSValue* out) {
  if (token.type == CSSTokenType::kIdent && SetEnumValue(id, token.payload, out)) {
    return true;
  }
  if (id == kPropertyIDColor || id == kPropertyIDBackgroundColor ||
      id == kPropertyIDBorderLeftColor || id == kPropertyIDBorderRightColor ||
      id == kPropertyIDBorderTopColor || id == kPropertyIDBorderBottomColor) {
    return ParseColorToken(token, out);
  }
  if (IsLengthProperty(id)) {
    return ParseLengthToken(token, out);
  }
  if (id == kPropertyIDOpacity || id == kPropertyIDFlexGrow ||
      id == kPropertyIDFlexShrink || id == kPropertyIDOrder ||
      id == kPropertyIDZIndex) {
    double number = 0;
    if (token.type != CSSTokenType::kNumber ||
        !ConsumeNumber(token.payload, &number)) {
      return false;
    }
    *out = CSSValue(number, CSSValuePattern::NUMBER);
    return true;
  }
  return false;
}

std::vector<CSSTokenRef> SignificantTokens(const CSSTokenStreamView& stream,
                                           uint32_t start, uint32_t end) {
  std::vector<CSSTokenRef> tokens;
  for (uint32_t i = start; i < end; ++i) {
    auto token = stream.TokenAt(i);
    if (!IsWhitespace(token.type)) {
      tokens.push_back(token);
    }
  }
  return tokens;
}

void AppendStringView(std::string_view value, std::string* output) {
  output->append(value.data(), value.size());
}

void AppendSerializedToken(CSSTokenRef token, std::string* output) {
  switch (token.type) {
    case CSSTokenType::kAtKeyword:
      output->push_back('@');
      AppendStringView(token.payload, output);
      break;
    case CSSTokenType::kFunction:
      AppendStringView(token.payload, output);
      output->push_back('(');
      break;
    case CSSTokenType::kHash:
      output->push_back('#');
      AppendStringView(token.payload, output);
      break;
    case CSSTokenType::kString:
      output->push_back('"');
      AppendStringView(token.payload, output);
      output->push_back('"');
      break;
    case CSSTokenType::kUrl:
      output->append("url(");
      AppendStringView(token.payload, output);
      output->push_back(')');
      break;
    case CSSTokenType::kPercentage:
      AppendStringView(token.payload, output);
      output->push_back('%');
      break;
    case CSSTokenType::kWhitespace:
      if (token.payload.empty()) {
        output->push_back(' ');
      } else {
        AppendStringView(token.payload, output);
      }
      break;
    case CSSTokenType::kColon:
      output->push_back(':');
      break;
    case CSSTokenType::kSemicolon:
      output->push_back(';');
      break;
    case CSSTokenType::kComma:
      output->push_back(',');
      break;
    case CSSTokenType::kLeftSquareBracket:
      output->push_back('[');
      break;
    case CSSTokenType::kRightSquareBracket:
      output->push_back(']');
      break;
    case CSSTokenType::kLeftParentheses:
      output->push_back('(');
      break;
    case CSSTokenType::kRightParentheses:
      output->push_back(')');
      break;
    case CSSTokenType::kLeftCurlyBracket:
      output->push_back('{');
      break;
    case CSSTokenType::kRightCurlyBracket:
      output->push_back('}');
      break;
    case CSSTokenType::kComment:
      output->append("/*");
      AppendStringView(token.payload, output);
      output->append("*/");
      break;
    default:
      AppendStringView(token.payload, output);
      break;
  }
}

std::string SerializeTokenRange(const CSSTokenStreamView& stream,
                                uint32_t start, uint32_t end) {
  std::string output;
  for (uint32_t i = start; i < end; ++i) {
    AppendSerializedToken(stream.TokenAt(i), &output);
  }
  return output;
}

bool ProcessSerializedPropertyValue(const CSSTokenStreamView& stream,
                                    CSSPropertyID id, uint32_t start,
                                    uint32_t end, StyleMap* output,
                                    const CSSParserConfigs& configs) {
  std::string value = SerializeTokenRange(stream, start, end);
  return !value.empty() &&
         UnitHandler::Process(id, lepus::Value(std::move(value)), *output,
                              configs);
}

void ExpandBox(CSSPropertyID top, CSSPropertyID right, CSSPropertyID bottom,
               CSSPropertyID left, const std::vector<CSSValue>& values,
               StyleMap* output) {
  if (values.empty()) {
    return;
  }
  const CSSValue& v0 = values[0];
  const CSSValue& v1 = values.size() > 1 ? values[1] : values[0];
  const CSSValue& v2 = values.size() > 2 ? values[2] : values[0];
  const CSSValue& v3 = values.size() > 3 ? values[3] : v1;
  (*output)[top] = v0;
  (*output)[right] = v1;
  (*output)[bottom] = v2;
  (*output)[left] = v3;
}

bool ParsePropertyValue(const CSSTokenStreamView& stream, CSSPropertyID id,
                        uint32_t start, uint32_t end, StyleMap* output,
                        const CSSParserConfigs& configs) {
  auto tokens = SignificantTokens(stream, start, end);
  if (tokens.empty()) {
    return false;
  }

  if (id == kPropertyIDMargin || id == kPropertyIDPadding) {
    if (tokens.size() > 4) {
      return false;
    }
    std::vector<CSSValue> values;
    values.reserve(tokens.size());
    for (auto token : tokens) {
      CSSValue value;
      if (!ParseLengthToken(token, &value)) {
        return false;
      }
      values.emplace_back(std::move(value));
    }
    if (id == kPropertyIDMargin) {
      ExpandBox(kPropertyIDMarginTop, kPropertyIDMarginRight,
                kPropertyIDMarginBottom, kPropertyIDMarginLeft, values,
                output);
    } else {
      ExpandBox(kPropertyIDPaddingTop, kPropertyIDPaddingRight,
                kPropertyIDPaddingBottom, kPropertyIDPaddingLeft, values,
                output);
    }
    return true;
  }

  if (tokens.size() == 1) {
    CSSValue value;
    if (ParseSingleValue(id, tokens[0], &value)) {
      (*output)[id] = std::move(value);
      return true;
    }
  }

  return ProcessSerializedPropertyValue(stream, id, start, end, output,
                                        configs);
}

struct SimpleSelector {
  LynxCSSSelector::MatchType match = LynxCSSSelector::kUnknown;
  std::string value;
};

struct CompoundSelector {
  std::vector<SimpleSelector> simples;
  LynxCSSSelector::RelationType relation_to_left = LynxCSSSelector::kSubSelector;
};

bool ParseCompound(const CSSTokenStreamView& stream, uint32_t* index,
                   uint32_t end, CompoundSelector* out) {
  bool consumed = false;
  while (*index < end) {
    auto token = stream.TokenAt(*index);
    if (IsWhitespace(token.type) || token.type == CSSTokenType::kComma ||
        token.type == CSSTokenType::kLeftCurlyBracket) {
      break;
    }
    if (token.type == CSSTokenType::kIdent) {
      out->simples.push_back({LynxCSSSelector::kTag, ToLower(token.payload)});
      consumed = true;
      ++*index;
      continue;
    }
    if (token.type == CSSTokenType::kHash) {
      out->simples.push_back({LynxCSSSelector::kId, std::string(token.payload)});
      consumed = true;
      ++*index;
      continue;
    }
    if (token.type == CSSTokenType::kDelim && token.payload == ".") {
      ++*index;
      if (*index >= end || stream.TokenAt(*index).type != CSSTokenType::kIdent) {
        return false;
      }
      out->simples.push_back(
          {LynxCSSSelector::kClass, std::string(stream.TokenAt(*index).payload)});
      consumed = true;
      ++*index;
      continue;
    }
    if (token.type == CSSTokenType::kDelim && token.payload == "*") {
      out->simples.push_back({LynxCSSSelector::kTag, "*"});
      consumed = true;
      ++*index;
      continue;
    }
    return false;
  }
  return consumed;
}

std::unique_ptr<LynxCSSSelector[]> ParseSelectorList(
    const CSSTokenStreamView& stream, uint32_t start, uint32_t end,
    size_t* flattened_size) {
  std::vector<std::vector<CompoundSelector>> selector_list;
  uint32_t index = start;
  while (index < end) {
    while (index < end && IsWhitespace(stream.TokenAt(index).type)) {
      ++index;
    }
    std::vector<CompoundSelector> compounds;
    LynxCSSSelector::RelationType pending_relation =
        LynxCSSSelector::kSubSelector;
    while (index < end && stream.TokenAt(index).type != CSSTokenType::kComma) {
      CompoundSelector compound;
      compound.relation_to_left = pending_relation;
      if (!ParseCompound(stream, &index, end, &compound)) {
        return nullptr;
      }
      pending_relation = LynxCSSSelector::kSubSelector;
      bool has_whitespace = false;
      while (index < end && IsWhitespace(stream.TokenAt(index).type)) {
        ++index;
        has_whitespace = true;
      }
      if (index < end && stream.TokenAt(index).type == CSSTokenType::kDelim &&
          stream.TokenAt(index).payload == ">") {
        pending_relation = LynxCSSSelector::kChild;
        ++index;
        while (index < end && IsWhitespace(stream.TokenAt(index).type)) {
          ++index;
        }
      } else if (has_whitespace && index < end &&
                 stream.TokenAt(index).type != CSSTokenType::kComma) {
        pending_relation = LynxCSSSelector::kDescendant;
      }
      compounds.push_back(std::move(compound));
    }
    if (compounds.empty()) {
      return nullptr;
    }
    selector_list.push_back(std::move(compounds));
    if (index < end && stream.TokenAt(index).type == CSSTokenType::kComma) {
      ++index;
    }
  }

  size_t size = 0;
  for (const auto& selector : selector_list) {
    for (const auto& compound : selector) {
      size += compound.simples.size();
    }
  }
  if (size == 0) {
    return nullptr;
  }

  auto selector_array = std::make_unique<LynxCSSSelector[]>(size);
  size_t out = 0;
  for (size_t selector_index = 0; selector_index < selector_list.size();
       ++selector_index) {
    const auto& selector = selector_list[selector_index];
    const size_t selector_start = out;
    for (auto compound_it = selector.rbegin(); compound_it != selector.rend();
         ++compound_it) {
      for (size_t i = 0; i < compound_it->simples.size(); ++i) {
        const auto& simple = compound_it->simples[i];
        selector_array[out].SetMatch(simple.match);
        selector_array[out].SetValue(simple.value);
        if (i + 1 == compound_it->simples.size()) {
          selector_array[out].SetRelation(compound_it->relation_to_left);
        }
        ++out;
      }
    }
    selector_array[out - 1].SetLastInTagHistory(true);
    for (size_t i = selector_start; i + 1 < out; ++i) {
      selector_array[i].SetLastInTagHistory(false);
    }
  }
  selector_array[out - 1].SetLastInSelectorList(true);

  for (size_t selector_index = 0;;) {
    selector_array[selector_index].UpdateSpecificity(
        selector_array[selector_index].CalcSpecificity());
    const LynxCSSSelector* next =
        css::LynxCSSSelectorList::Next(selector_array[selector_index]);
    if (!next) {
      break;
    }
    selector_index = static_cast<size_t>(next - selector_array.get());
  }

  *flattened_size = size;
  return selector_array;
}

bool ParseDeclarations(const CSSTokenStreamView& stream, Cursor* cursor,
                       fml::RefPtr<CSSParseToken> parse_token) {
  StyleMap attributes;
  StyleMap important_attributes;

  while (!cursor->AtEnd()) {
    cursor->ConsumeWhitespace();
    if (cursor->Peek().type == CSSTokenType::kRightCurlyBracket) {
      cursor->Consume();
      break;
    }
    if (cursor->Peek().type == CSSTokenType::kSemicolon) {
      cursor->Consume();
      continue;
    }
    if (cursor->Peek().type != CSSTokenType::kIdent) {
      cursor->SetIndex(SkipComponentValue(stream, cursor->Index(),
                                          stream.TokenCount()));
      continue;
    }

    const auto property_token = cursor->Consume();
    const std::string property_name = ToLower(property_token.payload);
    cursor->ConsumeWhitespace();
    if (cursor->Peek().type != CSSTokenType::kColon) {
      continue;
    }
    cursor->Consume();
    const uint32_t value_start = cursor->Index();
    uint32_t value_end = FindTopLevelToken(stream, value_start,
                                           stream.TokenCount(),
                                           CSSTokenType::kSemicolon);
    const uint32_t block_end = FindTopLevelToken(stream, value_start,
                                                 stream.TokenCount(),
                                                 CSSTokenType::kRightCurlyBracket);
    if (block_end < value_end) {
      value_end = block_end;
    }

    uint32_t significant_end = value_end;
    while (significant_end > value_start &&
           IsWhitespace(stream.TokenAt(significant_end - 1).type)) {
      --significant_end;
    }
    bool important = false;
    if (significant_end >= value_start + 2) {
      const auto bang = stream.TokenAt(significant_end - 2);
      const auto ident = stream.TokenAt(significant_end - 1);
      if (bang.type == CSSTokenType::kDelim && bang.payload == "!" &&
          ident.type == CSSTokenType::kIdent &&
          ToLower(ident.payload) == "important") {
        important = true;
        significant_end -= 2;
      }
    }

    if (!CSSProperty::IsCustomProperty(property_name.c_str(),
                                       static_cast<uint32_t>(property_name.size()))) {
      CSSPropertyID property_id = CSSProperty::GetPropertyID(property_name);
      if (CSSProperty::IsPropertyValid(property_id)) {
        ParsePropertyValue(stream, property_id, value_start, significant_end,
                           important ? &important_attributes : &attributes,
                           parse_token->GetCSSParserConfigs());
      }
    }

    cursor->SetIndex(value_end);
    if (cursor->Peek().type == CSSTokenType::kSemicolon) {
      cursor->Consume();
    }
  }

  parse_token->SetAttributes(std::move(attributes));
  parse_token->SetImportantAttributes(std::move(important_attributes));
  return true;
}

bool ParseKeyframeSelector(CSSTokenRef token,
                           const CSSParserConfigs& configs, float* out) {
  if (token.type == CSSTokenType::kIdent) {
    const std::string value = ToLower(token.payload);
    if (value == "from" || value == "to") {
      *out = CSSKeyframesToken::ParseKeyStr(value, configs.enable_css_strict_mode);
      return true;
    }
    return false;
  }
  if (token.type != CSSTokenType::kPercentage) {
    return false;
  }
  double percentage = 0;
  if (!ConsumeNumber(token.payload, &percentage)) {
    return false;
  }
  if (percentage < 0 || percentage > 100) {
    return false;
  }
  *out = static_cast<float>(percentage / 100.0);
  return true;
}

std::vector<float> ParseKeyframeSelectorList(
    const CSSTokenStreamView& stream, uint32_t start, uint32_t end,
    const CSSParserConfigs& configs) {
  std::vector<float> selectors;
  uint32_t index = start;
  while (index < end) {
    while (index < end && IsWhitespace(stream.TokenAt(index).type)) {
      ++index;
    }
    if (index >= end) {
      break;
    }

    float selector = 0;
    if (!ParseKeyframeSelector(stream.TokenAt(index), configs, &selector)) {
      return {};
    }
    selectors.push_back(selector);
    ++index;

    while (index < end && IsWhitespace(stream.TokenAt(index).type)) {
      ++index;
    }
    if (index < end) {
      if (stream.TokenAt(index).type != CSSTokenType::kComma) {
        return {};
      }
      ++index;
    }
  }
  return selectors;
}

class Parser {
 public:
  Parser(const CSSTokenStreamView& stream, const CSSParserConfigs& configs,
         bool enable_css_invalidation)
      : stream_(stream),
        cursor_(stream),
        configs_(configs),
        enable_css_invalidation_(enable_css_invalidation) {}

  WasmStyleSheetParseResult Parse() {
    auto fragment = std::make_unique<SharedCSSFragment>();
    if (enable_css_invalidation_) {
      fragment->SetEnableCSSInvalidation();
    }
    fragment->SetEnableCSSSelector();

    while (!cursor_.AtEnd()) {
      cursor_.ConsumeWhitespace();
      if (cursor_.AtEnd()) {
        break;
      }
      if (cursor_.Peek().type == CSSTokenType::kAtKeyword) {
        if (ToLower(cursor_.Peek().payload) == "keyframes") {
          ParseKeyframesRule(fragment.get());
          continue;
        }
        SkipAtRule();
        continue;
      }
      ParseStyleRule(fragment.get());
    }

    return {std::move(fragment), {}};
  }

 private:
  void SkipAtRule() {
    while (!cursor_.AtEnd()) {
      const auto token = cursor_.Peek();
      if (token.type == CSSTokenType::kSemicolon) {
        cursor_.Consume();
        return;
      }
      if (token.type == CSSTokenType::kLeftCurlyBracket ||
          token.type == CSSTokenType::kFunction) {
        cursor_.SetIndex(SkipComponentValue(stream_, cursor_.Index(),
                                            stream_.TokenCount()));
        return;
      }
      cursor_.Consume();
    }
  }

  void ParseKeyframesRule(SharedCSSFragment* fragment) {
    cursor_.Consume();
    cursor_.ConsumeWhitespace();
    if (cursor_.Peek().type != CSSTokenType::kIdent &&
        cursor_.Peek().type != CSSTokenType::kString) {
      SkipAtRule();
      return;
    }
    const std::string name(cursor_.Consume().payload);
    cursor_.ConsumeWhitespace();
    if (cursor_.Peek().type != CSSTokenType::kLeftCurlyBracket) {
      SkipAtRule();
      return;
    }
    cursor_.Consume();

    CSSKeyframesContent content;
    while (!cursor_.AtEnd()) {
      cursor_.ConsumeWhitespace();
      if (cursor_.Peek().type == CSSTokenType::kRightCurlyBracket) {
        cursor_.Consume();
        break;
      }

      const uint32_t selector_start = cursor_.Index();
      const uint32_t block_start =
          FindTopLevelToken(stream_, selector_start, stream_.TokenCount(),
                            CSSTokenType::kLeftCurlyBracket);
      if (block_start >= stream_.TokenCount()) {
        cursor_.SetIndex(stream_.TokenCount());
        break;
      }

      auto selectors = ParseKeyframeSelectorList(stream_, selector_start,
                                                 block_start, configs_);
      cursor_.SetIndex(block_start + 1);
      auto parse_token = fml::MakeRefCounted<CSSParseToken>(configs_);
      ParseDeclarations(stream_, &cursor_, parse_token);
      if (selectors.empty()) {
        continue;
      }

      auto style_map =
          std::make_shared<StyleMap>(parse_token->GetAttributes());
      for (float selector : selectors) {
        content[selector] = style_map;
      }
    }

    if (!content.empty()) {
      CSSKeyframesTokenMap keyframes = fragment->GetKeyframesRuleMap();
      auto token = fml::MakeRefCounted<CSSKeyframesToken>(configs_);
      token->SetKeyframesContent(std::move(content));
      keyframes[base::String(name)] = std::move(token);
      fragment->SetKeyFramesRuleMap(std::move(keyframes));
    }
  }

  void ParseStyleRule(SharedCSSFragment* fragment) {
    const uint32_t selector_start = cursor_.Index();
    const uint32_t block_start = FindTopLevelToken(
        stream_, selector_start, stream_.TokenCount(),
        CSSTokenType::kLeftCurlyBracket);
    if (block_start >= stream_.TokenCount()) {
      cursor_.SetIndex(stream_.TokenCount());
      return;
    }

    size_t flattened_size = 0;
    auto selector_array =
        ParseSelectorList(stream_, selector_start, block_start, &flattened_size);
    cursor_.SetIndex(block_start + 1);
    auto parse_token = fml::MakeRefCounted<CSSParseToken>(configs_);
    ParseDeclarations(stream_, &cursor_, parse_token);
    if (selector_array && flattened_size > 0) {
      fragment->AddStyleRule(std::move(selector_array), std::move(parse_token));
    }
  }

  const CSSTokenStreamView& stream_;
  Cursor cursor_;
  const CSSParserConfigs& configs_;
  bool enable_css_invalidation_;
};

}  // namespace

WasmStyleSheetParseResult ParseStyleSheetTokenStream(
    const CSSTokenStreamView& stream, const CSSParserConfigs& configs,
    bool enable_css_invalidation) {
  if (!stream.IsValid()) {
    return {nullptr, stream.Error() ? stream.Error()
                                    : "CSS token stream is invalid"};
  }
  Parser parser(stream, configs, enable_css_invalidation);
  return parser.Parse();
}

}  // namespace css_wasm
}  // namespace tasm
}  // namespace lynx
