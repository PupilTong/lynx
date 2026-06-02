// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "core/runtime/wasmr/wasmr_runner.h"

#include <array>
#include <mutex>
#include <string>

#include "base/include/log/logging.h"
#include "core/runtime/mts_context.h"
#include "core/runtime/wasmr/wasmr_host_functions.h"

namespace lynx {
namespace runtime {
namespace wasmr {
namespace {

constexpr uint32_t kWasmStackSize = 64 * 1024;
constexpr uint32_t kWasmHeapSize = 64 * 1024;
constexpr uint32_t kWasmErrorBufferSize = 512;

void SetError(std::string* error_msg, const std::string& message) {
  if (error_msg != nullptr) {
    *error_msg = message;
  }
}

std::string BuildWamrError(const char* prefix, const char* detail) {
  std::string message(prefix);
  if (detail != nullptr && detail[0] != '\0') {
    message.append(": ");
    message.append(detail);
  }
  return message;
}

bool EnsureWamrInitialized(std::string* error_msg) {
  static std::once_flag once;
  static bool initialized = false;
  static std::string init_error;

  std::call_once(once, []() {
    if (!wasm_runtime_init()) {
      init_error = "failed to initialize WAMR runtime";
      return;
    }
    if (!RegisterEngineHostFunctions()) {
      init_error = "failed to register Lynx WAMR host functions";
      return;
    }
    initialized = true;
  });

  if (!initialized) {
    SetError(error_msg, init_error);
  }
  return initialized;
}

bool CallWasmFunction(wasm_module_inst_t module_inst, wasm_exec_env_t exec_env,
                      wasm_function_inst_t function,
                      const char* function_name, std::string* error_msg) {
  if (wasm_runtime_call_wasm(exec_env, function, 0, nullptr)) {
    return true;
  }

  const char* exception = wasm_runtime_get_exception(module_inst);
  const std::string prefix =
      std::string("failed to execute WASM function ") + function_name;
  SetError(error_msg, BuildWamrError(prefix.c_str(), exception));
  return false;
}

bool ExecuteExportedEntry(wasm_module_inst_t module_inst,
                          wasm_exec_env_t exec_env, std::string* error_msg) {
  wasm_function_inst_t wasi_start =
      wasm_runtime_lookup_function(module_inst, "_start");
  if (wasi_start != nullptr) {
    return CallWasmFunction(module_inst, exec_env, wasi_start, "_start",
                            error_msg);
  }

  wasm_function_inst_t ctor =
      wasm_runtime_lookup_function(module_inst, "__wasm_call_ctors");
  if (ctor != nullptr &&
      !CallWasmFunction(module_inst, exec_env, ctor, "__wasm_call_ctors",
                        error_msg)) {
    return false;
  }

  constexpr std::array<const char*, 2> kEntryNames = {"__start", "start"};
  for (const char* entry_name : kEntryNames) {
    wasm_function_inst_t entry =
        wasm_runtime_lookup_function(module_inst, entry_name);
    if (entry == nullptr) {
      continue;
    }
    return CallWasmFunction(module_inst, exec_env, entry, entry_name,
                            error_msg);
  }

  if (wasm_application_execute_main(module_inst, 0, nullptr)) {
    return true;
  }

  const char* exception = wasm_runtime_get_exception(module_inst);
  if (exception != nullptr && exception[0] != '\0') {
    SetError(error_msg,
             BuildWamrError("failed to execute WASM main", exception));
    return false;
  }

  SetError(error_msg,
           "failed to execute WASM template: no supported entry function found");
  return false;
}

}  // namespace

bool ExecuteWasmModule(const uint8_t* data, size_t size, MTSContext* context,
                       const std::string& url, std::string* error_msg) {
  if (data == nullptr || size == 0) {
    SetError(error_msg, "failed to execute WASM template: empty module");
    return false;
  }
  if (context == nullptr) {
    SetError(error_msg, "failed to execute WASM template: missing MTS context");
    return false;
  }
  if (!EnsureWamrInitialized(error_msg)) {
    return false;
  }

  char error_buf[kWasmErrorBufferSize] = {};
  wasm_module_t module =
      wasm_runtime_load(const_cast<uint8_t*>(data),
                        static_cast<uint32_t>(size), error_buf,
                        static_cast<uint32_t>(sizeof(error_buf)));
  if (module == nullptr) {
    SetError(error_msg, BuildWamrError("failed to load WASM template",
                                       error_buf));
    return false;
  }

  wasm_module_inst_t module_inst =
      wasm_runtime_instantiate(module, kWasmStackSize, kWasmHeapSize,
                               error_buf,
                               static_cast<uint32_t>(sizeof(error_buf)));
  if (module_inst == nullptr) {
    SetError(error_msg, BuildWamrError("failed to instantiate WASM template",
                                       error_buf));
    wasm_runtime_unload(module);
    return false;
  }

  wasm_runtime_set_custom_data(module_inst, context);

  wasm_exec_env_t exec_env =
      wasm_runtime_create_exec_env(module_inst, kWasmStackSize);
  if (exec_env == nullptr) {
    SetError(error_msg,
             "failed to execute WASM template: cannot create exec env");
    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
    return false;
  }
  SetEngineHostContext(exec_env, context);

  const bool success = ExecuteExportedEntry(module_inst, exec_env, error_msg);
  if (success) {
    LOGI("executed WASM template: " << url);
  }

  wasm_runtime_destroy_exec_env(exec_env);
  wasm_runtime_deinstantiate(module_inst);
  wasm_runtime_unload(module);
  return success;
}

}  // namespace wasmr
}  // namespace runtime
}  // namespace lynx
