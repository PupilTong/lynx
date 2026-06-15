// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef CORE_RENDERER_CSS_WASM_WASM_STYLESHEET_PARSER_H_
#define CORE_RENDERER_CSS_WASM_WASM_STYLESHEET_PARSER_H_

#include <memory>
#include <string>

#include "core/renderer/css/parser/css_parser_configs.h"
#include "core/renderer/css/shared_css_fragment.h"
#include "core/renderer/css/wasm/css_token_stream_view.h"

namespace lynx {
namespace tasm {
namespace css_wasm {

struct WasmStyleSheetParseResult {
  std::unique_ptr<SharedCSSFragment> fragment;
  std::string error;
};

WasmStyleSheetParseResult ParseStyleSheetTokenStream(
    const CSSTokenStreamView& stream, const CSSParserConfigs& configs,
    bool enable_css_invalidation);

}  // namespace css_wasm
}  // namespace tasm
}  // namespace lynx

#endif  // CORE_RENDERER_CSS_WASM_WASM_STYLESHEET_PARSER_H_
