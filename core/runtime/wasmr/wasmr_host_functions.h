// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef CORE_RUNTIME_WASMR_WASMR_HOST_FUNCTIONS_H_
#define CORE_RUNTIME_WASMR_WASMR_HOST_FUNCTIONS_H_

#include <cstdint>
#include <memory>

#include "wasm_export.h"

namespace lynx {
namespace tasm {
struct PipelineOptions;
}  // namespace tasm

namespace runtime {

class MTSContext;

namespace wasmr {

constexpr const char kEngineHostModuleName[] = "env";

// Registers Lynx engine APIs imported by wasm modules under the `env` module.
// Elements and events use separate typed i32 arenas. Strings, primitive values,
// element-id arrays, and string arrays are copied through wasm linear memory.
// A negative arena id means `Optional<T>::None`.
bool RegisterEngineHostFunctions();

// Keeps a loaded WAMR module instance alive for callbacks that can re-enter the
// guest after the entry function returns, such as setTimeout / setInterval.
void RegisterEngineHostModule(wasm_module_t module,
                              wasm_module_inst_t module_inst,
                              MTSContext* context);

// Marks the initial entry call as finished. If no async host callbacks are
// pending, the module instance is released immediately; otherwise it is kept
// alive until the pending callbacks are cleared or fired.
void FinishEngineHostModule(wasm_module_inst_t module_inst);

// Cancels pending callbacks and releases the module instance.
void DestroyEngineHostModule(wasm_module_inst_t module_inst);

// Cancels all pending callbacks that belong to the runtime context.
void DestroyEngineHostModulesForContext(MTSContext* context);

// Stores the runtime context used by Lynx engine host functions invoked from
// this WAMR execution environment.
void SetEngineHostContext(wasm_exec_env_t exec_env, MTSContext* context);

MTSContext* GetEngineHostContext(wasm_exec_env_t exec_env);

void SetEngineHostPipelineOptions(
    std::shared_ptr<tasm::PipelineOptions> pipeline_options);

}  // namespace wasmr
}  // namespace runtime
}  // namespace lynx

#endif  // CORE_RUNTIME_WASMR_WASMR_HOST_FUNCTIONS_H_
