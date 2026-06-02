// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "core/runtime/wasmr/wasmr_host_functions.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>

#include "base/include/value/base_value.h"
#include "core/runtime/lepus/bindings/renderer.h"
#include "core/runtime/lepus/bindings/renderer_functions.h"

namespace lynx {
namespace runtime {
namespace wasmr {
namespace {

constexpr uintptr_t kNullExternRef = static_cast<uintptr_t>(-1);

struct EngineHostObject {
  explicit EngineHostObject(lepus::Value&& value) : value(std::move(value)) {}

  lepus::Value value;
};

void DeleteEngineHostObject(void* object) {
  delete static_cast<EngineHostObject*>(object);
}

void SetException(wasm_exec_env_t exec_env, const char* exception) {
  wasm_runtime_set_exception(wasm_runtime_get_module_inst(exec_env), exception);
}

uintptr_t ToExternRef(wasm_exec_env_t exec_env, lepus::Value&& value) {
  if (!value.IsRefCounted()) {
    SetException(exec_env, "__CreateView did not return a ref-counted object");
    return kNullExternRef;
  }

  auto host_object = std::make_unique<EngineHostObject>(std::move(value));
  auto* module_inst = wasm_runtime_get_module_inst(exec_env);
  uint32_t externref_index = 0;
  if (!wasm_externref_obj2ref(module_inst, host_object.get(),
                              &externref_index)) {
    SetException(exec_env, "failed to create externref for __CreateView");
    return kNullExternRef;
  }

  if (!wasm_externref_set_cleanup(module_inst, host_object.get(),
                                  DeleteEngineHostObject)) {
    wasm_externref_objdel(module_inst, host_object.get());
    SetException(exec_env, "failed to set externref cleanup for __CreateView");
    return kNullExternRef;
  }

  return reinterpret_cast<uintptr_t>(host_object.release());
}

uintptr_t CreateView(wasm_exec_env_t exec_env,
                     int32_t parent_component_unique_id) {
  auto* context = GetEngineHostContext(exec_env);
  if (context == nullptr) {
    SetException(exec_env, "__CreateView missing Lynx runtime context");
    return kNullExternRef;
  }

  lepus::Value args[] = {lepus::Value(parent_component_unique_id)};
  return ToExternRef(exec_env,
                     tasm::RendererFunctions::FiberCreateView(
                         context, args, static_cast<int>(std::size(args))));
}

NativeSymbol g_engine_host_symbols[] = {
    {tasm::kCFunctionCreateView, reinterpret_cast<void*>(CreateView), "(i)r",
     nullptr},
};

}  // namespace

bool RegisterEngineHostFunctions() {
  static std::mutex mutex;

  std::lock_guard<std::mutex> lock(mutex);
  wasm_runtime_unregister_natives(kEngineHostModuleName, g_engine_host_symbols);
  return wasm_runtime_register_natives(
      kEngineHostModuleName, g_engine_host_symbols,
      static_cast<uint32_t>(std::size(g_engine_host_symbols)));
}

void SetEngineHostContext(wasm_exec_env_t exec_env, MTSContext* context) {
  wasm_runtime_set_user_data(exec_env, context);
}

MTSContext* GetEngineHostContext(wasm_exec_env_t exec_env) {
  auto* context = static_cast<MTSContext*>(wasm_runtime_get_user_data(exec_env));
  if (context != nullptr) {
    return context;
  }
  return static_cast<MTSContext*>(
      wasm_runtime_get_custom_data(wasm_runtime_get_module_inst(exec_env)));
}

}  // namespace wasmr
}  // namespace runtime
}  // namespace lynx
