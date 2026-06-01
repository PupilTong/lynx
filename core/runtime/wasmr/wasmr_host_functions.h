// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef CORE_RUNTIME_WASMR_WASMR_HOST_FUNCTIONS_H_
#define CORE_RUNTIME_WASMR_WASMR_HOST_FUNCTIONS_H_

#include "wasm_export.h"

namespace lynx {
namespace runtime {

class MTSContext;

namespace wasmr {

constexpr const char kEngineHostModuleName[] = "env";

// Registers Lynx engine APIs imported by wasm modules. Currently exports:
//   env.__CreateView(i32 parent_component_unique_id) -> externref
bool RegisterEngineHostFunctions();

// Stores the runtime context used by Lynx engine host functions invoked from
// this WAMR execution environment.
void SetEngineHostContext(wasm_exec_env_t exec_env, MTSContext* context);

MTSContext* GetEngineHostContext(wasm_exec_env_t exec_env);

}  // namespace wasmr
}  // namespace runtime
}  // namespace lynx

#endif  // CORE_RUNTIME_WASMR_WASMR_HOST_FUNCTIONS_H_
