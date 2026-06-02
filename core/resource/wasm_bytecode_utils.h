// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef CORE_RESOURCE_WASM_BYTECODE_UTILS_H_
#define CORE_RESOURCE_WASM_BYTECODE_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lynx {
namespace tasm {

inline bool HasWamrWasmBytecodeMagic(const uint8_t* data, size_t size) {
  return data != nullptr && size >= 4 && data[0] == '\0' && data[1] == 'a' &&
         data[2] == 's' && data[3] == 'm';
}

inline bool HasWamrWasmBytecodeMagic(const std::vector<uint8_t>& data) {
  return HasWamrWasmBytecodeMagic(data.data(), data.size());
}

}  // namespace tasm
}  // namespace lynx

#endif  // CORE_RESOURCE_WASM_BYTECODE_UTILS_H_
