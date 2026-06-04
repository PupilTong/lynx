// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef CORE_RUNTIME_WASMR_WASMR_HOST_FUNCTIONS_H_
#define CORE_RUNTIME_WASMR_WASMR_HOST_FUNCTIONS_H_

#include <cstdint>

#include "wasm_export.h"

namespace lynx {
namespace runtime {

class MTSContext;

namespace wasmr {

constexpr const char kEngineHostModuleName[] = "env";

// Dynamic `any` input slots use five wasm ABI values:
//   i32 kind, f64 number_payload, i32 string_ptr, i32 string_len, i32 ref.
// `null` and `undefined` are represented by the kind tag. String payloads use
// wasm linear memory. Non-copyable values use the WAMR externref index payload.
enum class WasmHostValueKind : int32_t {
  kUndefined = 0,
  kNull = 1,
  kBool = 2,
  kNumber = 3,
  kString = 4,
  kExternRef = 5,
};

// Dynamic `any` return slots append three wasm ABI values:
//   i32 out_value_ptr, i32 out_string_ptr, i32 out_string_max_len.
// The native function fills this descriptor at out_value_ptr and returns an
// WAMR externref index only when kind is kExternRef.
struct WasmHostValueOut {
  int32_t kind;
  int32_t bool_value;
  double number_value;
  int32_t string_required_length;
  int32_t string_written_length;
};

// Registers Lynx engine APIs imported by wasm modules under the `env` module.
// Primitive number, bool, string, and null values are copied across the ABI;
// objects, elements, callbacks, arrays, maps, and other non-copyable values use
// WAMR externref handles owned by the WAMR runtime.
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

}  // namespace wasmr
}  // namespace runtime
}  // namespace lynx

#endif  // CORE_RUNTIME_WASMR_WASMR_HOST_FUNCTIONS_H_
