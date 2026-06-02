// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef CORE_RUNTIME_WASMR_WASMR_RUNNER_H_
#define CORE_RUNTIME_WASMR_WASMR_RUNNER_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace lynx {
namespace runtime {

class MTSContext;

namespace wasmr {

bool ExecuteWasmModule(const uint8_t* data, size_t size, MTSContext* context,
                       const std::string& url, std::string* error_msg);

}  // namespace wasmr
}  // namespace runtime
}  // namespace lynx

#endif  // CORE_RUNTIME_WASMR_WASMR_RUNNER_H_
