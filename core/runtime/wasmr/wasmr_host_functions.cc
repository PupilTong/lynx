// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "core/runtime/wasmr/wasmr_host_functions.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/include/thread/timed_task.h"
#include "base/include/value/array.h"
#include "base/include/value/base_value.h"
#include "base/include/vector.h"
#include "core/event/event.h"
#include "core/event/event_dispatcher.h"
#include "core/event/event_listener.h"
#include "core/renderer/dom/fiber/block_element.h"
#include "core/renderer/dom/fiber/fiber_element.h"
#include "core/renderer/dom/selector/fiber_element_selector.h"
#include "core/renderer/css/wasm/css_token_stream_view.h"
#include "core/renderer/css/wasm/wasm_stylesheet_parser.h"
#include "core/renderer/utils/base/tasm_constants.h"
#include "core/renderer/utils/value_utils.h"
#include "core/renderer/template_assembler.h"
#include "core/runtime/lepus/bindings/renderer.h"
#include "core/runtime/lepus/bindings/renderer_functions.h"
#include "core/runtime/lepus/bindings/style/shared_css_fragment_wrapper.h"
#include "core/runtime/mts_context.h"
#include "core/runtime/wasmr/wasmr_thread_env_guard.h"
#include "core/shell/runtime/mts/mts_runtime.h"

namespace lynx {
namespace runtime {
namespace wasmr {
namespace {

constexpr int32_t kNullHostRef = -1;
constexpr uint32_t kTimerCallbackStackSize = 64 * 1024;

constexpr int32_t kEventFlagCapture = 1 << 0;
constexpr int32_t kEventFlagBubbles = 1 << 1;
constexpr int32_t kEventFlagCancelable = 1 << 2;
constexpr int32_t kEventFlagComposed = 1 << 3;
constexpr const char kStyleAttribute[] = "style";

std::shared_ptr<tasm::PipelineOptions>& CurrentEngineHostPipelineOptions() {
  static thread_local std::shared_ptr<tasm::PipelineOptions> pipeline_options;
  return pipeline_options;
}

struct WasmI32 {};
struct WasmI64 {};
struct WasmBool {};
struct WasmString {};
struct WasmElementRef {};
struct WasmVoid {};

struct HiddenCreateNone {};
struct HiddenCreateElement {};
struct HiddenCreatePage {};
struct HiddenCreateParent {};

using ElementRef = fml::RefPtr<tasm::FiberElement>;
using EventRef = fml::RefPtr<event::Event>;

struct WasmModuleTimerState {
  WasmModuleTimerState(wasm_module_t module,
                       wasm_module_inst_t module_inst,
                       MTSContext* context)
      : module(module), module_inst(module_inst), context(context) {}

  WasmModuleTimerState(const WasmModuleTimerState&) = delete;
  WasmModuleTimerState& operator=(const WasmModuleTimerState&) = delete;

  ~WasmModuleTimerState() { Shutdown(); }

  void Shutdown() {
    if (is_shutdown) {
      return;
    }
    is_shutdown = true;
    if (timer_manager) {
      timer_manager->StopAllTasks();
      timer_manager.reset();
    }
    timers.clear();
    element_refs.clear();
    event_refs.clear();
    if (module_inst != nullptr) {
      wasm_runtime_deinstantiate(module_inst);
      module_inst = nullptr;
    }
    if (module != nullptr) {
      wasm_runtime_unload(module);
      module = nullptr;
    }
    context = nullptr;
  }

  wasm_module_t module = nullptr;
  wasm_module_inst_t module_inst = nullptr;
  MTSContext* context = nullptr;
  bool is_shutdown = false;
  bool entry_finished = false;
  bool cleanup_scheduled = false;
  int32_t next_element_ref = 0;
  int32_t next_event_ref = 0;
  uint32_t event_listener_count = 0;
  std::unique_ptr<base::TimedTaskManager> timer_manager;
  std::unordered_set<uint32_t> timers;
  std::unordered_map<int32_t, ElementRef> element_refs;
  std::unordered_map<int32_t, EventRef> event_refs;
};

std::mutex& WasmModuleTimerStatesMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::unordered_map<wasm_module_inst_t, std::shared_ptr<WasmModuleTimerState>>&
WasmModuleTimerStates() {
  static auto* states =
      new std::unordered_map<wasm_module_inst_t,
                             std::shared_ptr<WasmModuleTimerState>>();
  return *states;
}

std::shared_ptr<WasmModuleTimerState> GetWasmModuleTimerState(
    wasm_module_inst_t module_inst) {
  std::lock_guard<std::mutex> lock(WasmModuleTimerStatesMutex());
  auto& states = WasmModuleTimerStates();
  auto iter = states.find(module_inst);
  if (iter == states.end()) {
    return nullptr;
  }
  return iter->second;
}

void MaybeEraseFinishedWasmModuleTimerState(
    const std::shared_ptr<WasmModuleTimerState>& state) {
  if (!state || !state->entry_finished || !state->timers.empty() ||
      state->event_listener_count != 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(WasmModuleTimerStatesMutex());
  auto& states = WasmModuleTimerStates();
  auto iter = states.find(state->module_inst);
  if (iter != states.end() && iter->second == state) {
    states.erase(iter);
  }
}

void DeferEraseFinishedWasmModuleTimerState(
    const std::shared_ptr<WasmModuleTimerState>& state);

void SetException(wasm_exec_env_t exec_env, const char* exception) {
  wasm_runtime_set_exception(wasm_runtime_get_module_inst(exec_env), exception);
}

void SetException(wasm_exec_env_t exec_env, const std::string& exception) {
  SetException(exec_env, exception.c_str());
}

std::string ExceptionPrefix(const char* function_name) {
  return std::string(function_name) + ": ";
}

std::string BuildTimerException(const char* function_name,
                                const char* detail) {
  std::string message = ExceptionPrefix(function_name);
  message.append(detail != nullptr && detail[0] != '\0' ? detail
                                                        : "unknown exception");
  return message;
}

void ReportTimerException(const std::shared_ptr<WasmModuleTimerState>& state,
                          const char* function_name, const char* detail) {
  if (state && state->context != nullptr) {
    state->context->ReportError(BuildTimerException(function_name, detail));
  }
}

std::shared_ptr<WasmModuleTimerState> GetWasmModuleState(
    wasm_exec_env_t exec_env, const char* function_name) {
  auto* module_inst = wasm_runtime_get_module_inst(exec_env);
  auto state = GetWasmModuleTimerState(module_inst);
  if (!state) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " missing WAMR module host state");
  }
  return state;
}

int32_t AllocateArenaId(int32_t* next_id, const char* arena_name,
                        wasm_exec_env_t exec_env, const char* function_name) {
  if (*next_id == std::numeric_limits<int32_t>::max()) {
    SetException(exec_env, ExceptionPrefix(function_name) + arena_name +
                               " arena is exhausted");
    return kNullHostRef;
  }
  return (*next_id)++;
}

int32_t StoreElementRef(const std::shared_ptr<WasmModuleTimerState>& state,
                        const ElementRef& element, wasm_exec_env_t exec_env,
                        const char* function_name) {
  if (!element) {
    return kNullHostRef;
  }
  const int32_t id =
      AllocateArenaId(&state->next_element_ref, " element", exec_env,
                      function_name);
  if (id < 0) {
    return kNullHostRef;
  }
  state->element_refs.emplace(id, element);
  return id;
}

int32_t StoreElementRef(wasm_exec_env_t exec_env, const ElementRef& element,
                        const char* function_name) {
  auto state = GetWasmModuleState(exec_env, function_name);
  if (!state) {
    return kNullHostRef;
  }
  return StoreElementRef(state, element, exec_env, function_name);
}

int32_t StoreElementValue(wasm_exec_env_t exec_env, lepus::Value&& value,
                          const char* function_name) {
  if (value.IsEmpty() || value.IsNil() || value.IsUndefined()) {
    return kNullHostRef;
  }
  if (!value.IsRefCounted() ||
      value.RefCounted()->GetRefType() != lepus::RefType::kElement) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " return value is not a FiberElement");
    return kNullHostRef;
  }
  return StoreElementRef(
      exec_env, fml::static_ref_ptr_cast<tasm::FiberElement>(
                    value.RefCounted()),
      function_name);
}

ElementRef GetElementRef(wasm_exec_env_t exec_env, int32_t element_id,
                         const char* function_name, bool* ok) {
  if (element_id < 0) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " element arena id is absent");
    *ok = false;
    return nullptr;
  }
  auto state = GetWasmModuleState(exec_env, function_name);
  if (!state) {
    *ok = false;
    return nullptr;
  }
  auto iter = state->element_refs.find(element_id);
  if (iter == state->element_refs.end()) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " element arena id is invalid");
    *ok = false;
    return nullptr;
  }
  return iter->second;
}

ElementRef GetOptionalElementRef(wasm_exec_env_t exec_env, int32_t element_id,
                                 const char* function_name, bool* ok) {
  if (element_id < 0) {
    return nullptr;
  }
  return GetElementRef(exec_env, element_id, function_name, ok);
}

void DropElementRef(wasm_exec_env_t exec_env, int32_t element_id,
                    const char* function_name) {
  if (element_id < 0) {
    return;
  }
  auto state = GetWasmModuleState(exec_env, function_name);
  if (!state) {
    return;
  }
  if (state->element_refs.erase(element_id) == 0) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " element arena id is invalid");
  }
}

int32_t StoreEventRef(const std::shared_ptr<WasmModuleTimerState>& state,
                      const EventRef& event, wasm_exec_env_t exec_env,
                      const char* function_name) {
  if (!event) {
    return kNullHostRef;
  }
  const int32_t id =
      AllocateArenaId(&state->next_event_ref, " event", exec_env,
                      function_name);
  if (id < 0) {
    return kNullHostRef;
  }
  state->event_refs.emplace(id, event);
  return id;
}

int32_t StoreEventRef(wasm_exec_env_t exec_env, const EventRef& event,
                      const char* function_name) {
  auto state = GetWasmModuleState(exec_env, function_name);
  if (!state) {
    return kNullHostRef;
  }
  return StoreEventRef(state, event, exec_env, function_name);
}

EventRef GetEventRef(wasm_exec_env_t exec_env, int32_t event_id,
                     const char* function_name, bool* ok) {
  if (event_id < 0) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " event arena id is absent");
    *ok = false;
    return nullptr;
  }
  auto state = GetWasmModuleState(exec_env, function_name);
  if (!state) {
    *ok = false;
    return nullptr;
  }
  auto iter = state->event_refs.find(event_id);
  if (iter == state->event_refs.end()) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " event arena id is invalid");
    *ok = false;
    return nullptr;
  }
  return iter->second;
}

void DropEventRef(wasm_exec_env_t exec_env, int32_t event_id,
                  const char* function_name) {
  if (event_id < 0) {
    return;
  }
  auto state = GetWasmModuleState(exec_env, function_name);
  if (!state) {
    return;
  }
  if (state->event_refs.erase(event_id) == 0) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " event arena id is invalid");
  }
}

void InvokeWasmTimerCallback(
    const std::shared_ptr<WasmModuleTimerState>& state,
    uint32_t callback_index, uint32_t timer_id, const char* function_name) {
  if (!state || state->module_inst == nullptr || state->context == nullptr) {
    return;
  }

  wasm_exec_env_t callback_env =
      wasm_runtime_create_exec_env(state->module_inst, kTimerCallbackStackSize);
  if (callback_env == nullptr) {
    ReportTimerException(state, function_name,
                         "failed to create callback exec env");
    return;
  }

  SetEngineHostContext(callback_env, state->context);
  WamrThreadEnvGuard thread_env;
  if (!thread_env.ok()) {
    ReportTimerException(state, function_name,
                         "failed to initialize WAMR thread environment");
    wasm_runtime_destroy_exec_env(callback_env);
    return;
  }

  uint32_t argv[] = {timer_id};
  if (!wasm_runtime_call_indirect(callback_env, callback_index,
                                  static_cast<uint32_t>(std::size(argv)),
                                  argv)) {
    ReportTimerException(
        state, function_name,
        wasm_runtime_get_exception(state->module_inst));
  }
  wasm_runtime_destroy_exec_env(callback_env);
}

void InvokeWasmEventCallback(
    const std::shared_ptr<WasmModuleTimerState>& state,
    uint32_t callback_index, const EventRef& event,
    const char* function_name) {
  if (!state || state->module_inst == nullptr || state->context == nullptr ||
      callback_index == 0) {
    return;
  }

  wasm_exec_env_t callback_env =
      wasm_runtime_create_exec_env(state->module_inst, kTimerCallbackStackSize);
  if (callback_env == nullptr) {
    ReportTimerException(state, function_name,
                         "failed to create event callback exec env");
    return;
  }

  SetEngineHostContext(callback_env, state->context);
  WamrThreadEnvGuard thread_env;
  if (!thread_env.ok()) {
    ReportTimerException(state, function_name,
                         "failed to initialize WAMR thread environment");
    wasm_runtime_destroy_exec_env(callback_env);
    return;
  }

  int32_t event_id = StoreEventRef(state, event, callback_env, function_name);
  if (event_id >= 0) {
    uint32_t argv[] = {static_cast<uint32_t>(event_id)};
    if (!wasm_runtime_call_indirect(callback_env, callback_index,
                                    static_cast<uint32_t>(std::size(argv)),
                                    argv)) {
      ReportTimerException(
          state, function_name,
          wasm_runtime_get_exception(state->module_inst));
    }
  }
  wasm_runtime_destroy_exec_env(callback_env);
}

std::unique_ptr<base::TimedTaskManager>& EnsureTimerManager(
    const std::shared_ptr<WasmModuleTimerState>& state) {
  if (!state->timer_manager) {
    state->timer_manager = std::make_unique<base::TimedTaskManager>();
  }
  return state->timer_manager;
}

void DeferEraseFinishedWasmModuleTimerState(
    const std::shared_ptr<WasmModuleTimerState>& state) {
  if (!state || !state->entry_finished || !state->timers.empty() ||
      state->cleanup_scheduled) {
    return;
  }
  state->cleanup_scheduled = true;
  EnsureTimerManager(state)->SetTimeout(
      [state]() {
        state->cleanup_scheduled = false;
        MaybeEraseFinishedWasmModuleTimerState(state);
      },
      0);
}

uint32_t ScheduleWasmTimer(wasm_exec_env_t exec_env, uint32_t callback_index,
                           int64_t delay_ms, bool is_interval,
                           const char* function_name) {
  if (callback_index == 0) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " callback function pointer is null");
    return 0;
  }

  auto* module_inst = wasm_runtime_get_module_inst(exec_env);
  auto state = GetWasmModuleTimerState(module_inst);
  if (!state) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " missing WAMR module timer state");
    return 0;
  }

  auto timer_id_holder = std::make_shared<uint32_t>(0);
  auto task = [state, callback_index, timer_id_holder, is_interval,
               function_name]() {
    const uint32_t timer_id = *timer_id_holder;
    InvokeWasmTimerCallback(state, callback_index, timer_id, function_name);
    if (!is_interval) {
      state->timers.erase(timer_id);
      MaybeEraseFinishedWasmModuleTimerState(state);
    }
  };

  uint32_t timer_id = 0;
  const int64_t normalized_delay = delay_ms < 0 ? 0 : delay_ms;
  if (is_interval) {
    timer_id = EnsureTimerManager(state)->SetInterval(std::move(task),
                                                      normalized_delay);
  } else {
    timer_id = EnsureTimerManager(state)->SetTimeout(std::move(task),
                                                     normalized_delay);
  }
  *timer_id_holder = timer_id;
  state->timers.emplace(timer_id);
  return timer_id;
}

void ClearWasmTimer(wasm_exec_env_t exec_env, uint32_t timer_id,
                    const char* function_name) {
  if (timer_id == 0) {
    return;
  }
  auto* module_inst = wasm_runtime_get_module_inst(exec_env);
  auto state = GetWasmModuleTimerState(module_inst);
  if (!state) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " missing WAMR module timer state");
    return;
  }
  if (state->timer_manager) {
    state->timer_manager->StopTask(timer_id);
  }
  state->timers.erase(timer_id);
  DeferEraseFinishedWasmModuleTimerState(state);
}

uint32_t DecodeTimerId(uint64_t raw_timer_id) {
  int64_t timer_id = static_cast<int64_t>(raw_timer_id);
  if (timer_id <= 0 ||
      timer_id > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
    return 0;
  }
  return static_cast<uint32_t>(timer_id);
}

void SetTimeoutHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  const uint32_t callback_index = static_cast<uint32_t>(raw_args[0]);
  const int64_t delay_ms = static_cast<int64_t>(raw_args[1]);
  raw_args[0] = ScheduleWasmTimer(exec_env, callback_index, delay_ms, false,
                                  tasm::kSetTimeout);
}

void SetIntervalHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  const uint32_t callback_index = static_cast<uint32_t>(raw_args[0]);
  const int64_t delay_ms = static_cast<int64_t>(raw_args[1]);
  raw_args[0] = ScheduleWasmTimer(exec_env, callback_index, delay_ms, true,
                                  tasm::kSetInterval);
}

void ClearTimeoutHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  ClearWasmTimer(exec_env, DecodeTimerId(raw_args[0]), tasm::kClearTimeout);
}

void ClearIntervalHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  ClearWasmTimer(exec_env, DecodeTimerId(raw_args[0]),
                 tasm::kClearTimeInterval);
}

bool CheckedByteLength(int32_t count, int32_t item_size, int32_t* out) {
  if (count < 0 || item_size <= 0) {
    return false;
  }
  if (count > std::numeric_limits<int32_t>::max() / item_size) {
    return false;
  }
  *out = count * item_size;
  return true;
}

bool ValidateAppMemory(wasm_exec_env_t exec_env, int32_t app_offset,
                       int32_t byte_length, const char* function_name,
                       const char* role) {
  if (byte_length < 0) {
    SetException(exec_env, ExceptionPrefix(function_name) + role +
                               " length must not be negative");
    return false;
  }
  if (byte_length == 0) {
    return true;
  }
  if (app_offset <= 0) {
    SetException(exec_env, ExceptionPrefix(function_name) + role +
                               " pointer is invalid");
    return false;
  }
  auto* module_inst = wasm_runtime_get_module_inst(exec_env);
  if (!wasm_runtime_validate_app_addr(
          module_inst, static_cast<uint64_t>(app_offset),
          static_cast<uint64_t>(byte_length))) {
    SetException(exec_env, ExceptionPrefix(function_name) + role +
                               " memory range is invalid");
    return false;
  }
  return true;
}

void* AppAddrToNative(wasm_exec_env_t exec_env, int32_t app_offset) {
  return wasm_runtime_addr_app_to_native(
      wasm_runtime_get_module_inst(exec_env), static_cast<uint64_t>(app_offset));
}

std::string ReadUtf8String(wasm_exec_env_t exec_env, int32_t app_offset,
                           int32_t byte_length, const char* function_name,
                           bool* ok) {
  if (!ValidateAppMemory(exec_env, app_offset, byte_length, function_name,
                         " string")) {
    *ok = false;
    return {};
  }
  if (byte_length == 0) {
    return {};
  }
  auto* data = static_cast<const char*>(AppAddrToNative(exec_env, app_offset));
  return std::string(data, static_cast<size_t>(byte_length));
}

int32_t WriteUtf8String(wasm_exec_env_t exec_env, std::string_view value,
                        int32_t out_offset, int32_t out_max_length,
                        const char* function_name, bool* ok) {
  if (out_max_length < 0) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " output string max length must not be negative");
    *ok = false;
    return -1;
  }
  int32_t required_length =
      value.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())
          ? std::numeric_limits<int32_t>::max()
          : static_cast<int32_t>(value.size());
  if (required_length > out_max_length) {
    return required_length;
  }
  if (required_length == 0) {
    return required_length;
  }
  if (!ValidateAppMemory(exec_env, out_offset, required_length, function_name,
                         " output string")) {
    *ok = false;
    return required_length;
  }
  auto* out_data = static_cast<char*>(AppAddrToNative(exec_env, out_offset));
  std::memcpy(out_data, value.data(), static_cast<size_t>(required_length));
  return required_length;
}

class RawArgReader {
 public:
  explicit RawArgReader(uint64_t* raw_args) : current_(raw_args) {}

  int32_t ReadI32() { return static_cast<int32_t>(*current_++); }

  int64_t ReadI64() { return static_cast<int64_t>(*current_++); }

 private:
  uint64_t* current_;
};

lepus::Value ReadHostArgument(wasm_exec_env_t, RawArgReader* reader, WasmI32,
                              const char*, bool*) {
  return lepus::Value(reader->ReadI32());
}

lepus::Value ReadHostArgument(wasm_exec_env_t, RawArgReader* reader, WasmI64,
                              const char*, bool*) {
  return lepus::Value(reader->ReadI64());
}

lepus::Value ReadHostArgument(wasm_exec_env_t exec_env, RawArgReader* reader,
                              WasmString, const char* function_name,
                              bool* ok) {
  return lepus::Value(
      ReadUtf8String(exec_env, reader->ReadI32(), reader->ReadI32(),
                     function_name, ok));
}

lepus::Value ReadHostArgument(wasm_exec_env_t exec_env, RawArgReader* reader,
                              WasmElementRef, const char* function_name,
                              bool* ok) {
  auto element = GetElementRef(exec_env, reader->ReadI32(), function_name, ok);
  if (!*ok || !element) {
    return lepus::Value();
  }
  return lepus::Value(element);
}

bool ValueAsI32(const lepus::Value& value, const char* function_name,
                wasm_exec_env_t exec_env, int32_t* out) {
  if (value.IsNumber()) {
    *out = static_cast<int32_t>(value.Number());
    return true;
  }
  if (value.IsBool()) {
    *out = value.Bool() ? 1 : 0;
    return true;
  }
  SetException(exec_env, ExceptionPrefix(function_name) +
                             " return value is not an i32-compatible value");
  return false;
}

bool ValueAsI64(const lepus::Value& value, const char* function_name,
                wasm_exec_env_t exec_env, int64_t* out) {
  if (value.IsNumber()) {
    *out = static_cast<int64_t>(value.Number());
    return true;
  }
  if (value.IsBool()) {
    *out = value.Bool() ? 1 : 0;
    return true;
  }
  SetException(exec_env, ExceptionPrefix(function_name) +
                             " return value is not an i64-compatible value");
  return false;
}

bool ValueAsString(const lepus::Value& value, const char* function_name,
                   wasm_exec_env_t exec_env, std::string* out) {
  if (value.IsEmpty() || value.IsUndefined() || value.IsNil()) {
    out->clear();
    return true;
  }
  if (value.IsString()) {
    *out = value.StdString();
    return true;
  }
  SetException(exec_env, ExceptionPrefix(function_name) +
                             " return value is not a string");
  return false;
}

void WriteHostReturn(wasm_exec_env_t, uint64_t*, RawArgReader*, WasmVoid,
                     lepus::Value&&, const char*, bool*) {}

void WriteHostReturn(wasm_exec_env_t exec_env, uint64_t* raw_args,
                     RawArgReader*, WasmI32, lepus::Value&& result,
                     const char* function_name, bool* ok) {
  int32_t value = 0;
  if (!ValueAsI32(result, function_name, exec_env, &value)) {
    *ok = false;
    return;
  }
  raw_args[0] = static_cast<uint32_t>(value);
}

void WriteHostReturn(wasm_exec_env_t exec_env, uint64_t* raw_args,
                     RawArgReader*, WasmI64, lepus::Value&& result,
                     const char* function_name, bool* ok) {
  int64_t value = 0;
  if (!ValueAsI64(result, function_name, exec_env, &value)) {
    *ok = false;
    return;
  }
  raw_args[0] = static_cast<uint64_t>(value);
}

void WriteHostReturn(wasm_exec_env_t, uint64_t* raw_args, RawArgReader*,
                     WasmBool, lepus::Value&& result, const char*, bool*) {
  raw_args[0] = result.Bool() ? 1 : 0;
}

void WriteHostReturn(wasm_exec_env_t exec_env, uint64_t* raw_args,
                     RawArgReader* reader, WasmString, lepus::Value&& result,
                     const char* function_name, bool* ok) {
  int32_t out_offset = reader->ReadI32();
  int32_t out_max_length = reader->ReadI32();
  if (result.IsEmpty() || result.IsUndefined() || result.IsNil()) {
    raw_args[0] = static_cast<uint32_t>(-1);
    return;
  }
  std::string string_value;
  if (!ValueAsString(result, function_name, exec_env, &string_value)) {
    *ok = false;
    raw_args[0] = static_cast<uint32_t>(-1);
    return;
  }
  int32_t required_length =
      WriteUtf8String(exec_env, string_value, out_offset, out_max_length,
                      function_name, ok);
  raw_args[0] = static_cast<uint32_t>(required_length);
}

void WriteHostReturn(wasm_exec_env_t exec_env, uint64_t* raw_args,
                     RawArgReader*, WasmElementRef, lepus::Value&& result,
                     const char* function_name, bool*) {
  raw_args[0] = static_cast<uint32_t>(
      StoreElementValue(exec_env, std::move(result), function_name));
}

void InjectHiddenCreateValues(std::vector<lepus::Value>*, HiddenCreateNone) {}

void InjectHiddenCreateValues(std::vector<lepus::Value>* args,
                              HiddenCreateElement) {
  args->insert(args->begin() + 1, lepus::Value(0.0));
}

void InjectHiddenCreateValues(std::vector<lepus::Value>* args,
                              HiddenCreatePage) {
  args->insert(args->begin(), lepus::Value(std::string("0")));
  args->insert(args->begin() + 1, lepus::Value(0.0));
}

void InjectHiddenCreateValues(std::vector<lepus::Value>* args,
                              HiddenCreateParent) {
  args->insert(args->begin(), lepus::Value(0.0));
}

template <typename ArgTag>
void ReadAndAppendHostArgument(wasm_exec_env_t exec_env, RawArgReader* reader,
                               const char* function_name, bool* ok,
                               std::vector<lepus::Value>* args) {
  if (!*ok) {
    return;
  }
  args->emplace_back(
      ReadHostArgument(exec_env, reader, ArgTag{}, function_name, ok));
}

template <typename ReturnTag, typename HiddenCreateTag, typename... ArgTags>
void InvokeEngineHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args,
                              lepus::CFunction function,
                              const char* function_name) {
  auto* context = GetEngineHostContext(exec_env);
  if (context == nullptr) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " missing Lynx runtime context");
    return;
  }

  bool ok = true;
  RawArgReader reader(raw_args);
  std::vector<lepus::Value> args;
  args.reserve(sizeof...(ArgTags) + 2);
  int unused[] = {0, (ReadAndAppendHostArgument<ArgTags>(
                          exec_env, &reader, function_name, &ok, &args),
                      0)...};
  (void)unused;
  if (!ok) {
    return;
  }

  InjectHiddenCreateValues(&args, HiddenCreateTag{});

  lepus::Value result = function(context, args.data(),
                                 static_cast<int>(args.size()));
  WriteHostReturn(exec_env, raw_args, &reader, ReturnTag{}, std::move(result),
                  function_name, &ok);
}

#define BINDING(id, symbol, function, return_tag, ...)                     \
  constexpr const char* id##Symbol = symbol;                               \
  void id##HostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {    \
    InvokeEngineHostFunction<return_tag, HiddenCreateNone, __VA_ARGS__>(   \
        exec_env, raw_args, &tasm::RendererFunctions::function,            \
        id##Symbol);                                                       \
  }

#define BINDING_WITH_CREATE_ARGS(id, symbol, function, return_tag,       \
                                 hidden_create_tag, ...)                 \
  constexpr const char* id##Symbol = symbol;                             \
  void id##HostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {  \
    InvokeEngineHostFunction<return_tag, hidden_create_tag, __VA_ARGS__>( \
        exec_env, raw_args, &tasm::RendererFunctions::function,          \
        id##Symbol);                                                     \
  }

#define BINDING0(id, symbol, function, return_tag)                         \
  constexpr const char* id##Symbol = symbol;                               \
  void id##HostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {    \
    InvokeEngineHostFunction<return_tag, HiddenCreateNone>(                \
        exec_env, raw_args, &tasm::RendererFunctions::function,            \
        id##Symbol);                                                       \
  }

#define BINDING0_WITH_CREATE_ARGS(id, symbol, function, return_tag,       \
                                  hidden_create_tag)                      \
  constexpr const char* id##Symbol = symbol;                              \
  void id##HostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {   \
    InvokeEngineHostFunction<return_tag, hidden_create_tag>(              \
        exec_env, raw_args, &tasm::RendererFunctions::function,           \
        id##Symbol);                                                      \
  }

BINDING_WITH_CREATE_ARGS(
    kCreateElementBinding, tasm::kCFunctionCreateElement, FiberCreateElement,
    WasmElementRef, HiddenCreateElement, WasmString);
BINDING0_WITH_CREATE_ARGS(kCreatePageBinding, tasm::kCFunctionCreatePage,
                          FiberCreatePage, WasmElementRef, HiddenCreatePage);
BINDING0_WITH_CREATE_ARGS(kCreateViewBinding, tasm::kCFunctionCreateView,
                          FiberCreateView, WasmElementRef, HiddenCreateParent);
BINDING0_WITH_CREATE_ARGS(
    kCreateScrollViewBinding, tasm::kCFunctionCreateScrollView,
    FiberCreateScrollView, WasmElementRef, HiddenCreateParent);
BINDING0_WITH_CREATE_ARGS(kCreateTextBinding, tasm::kCFunctionCreateText,
                          FiberCreateText, WasmElementRef, HiddenCreateParent);
BINDING0_WITH_CREATE_ARGS(kCreateImageBinding, tasm::kCFunctionCreateImage,
                          FiberCreateImage, WasmElementRef, HiddenCreateParent);
BINDING(kCreateRawTextBinding, tasm::kCFunctionCreateRawText,
        FiberCreateRawText, WasmElementRef, WasmString);
BINDING0_WITH_CREATE_ARGS(kCreateNonElementBinding,
                          tasm::kCFunctionCreateNonElement,
                          FiberCreateNonElement, WasmElementRef,
                          HiddenCreateParent);
BINDING0_WITH_CREATE_ARGS(kCreateWrapperElementBinding,
                          tasm::kCFunctionCreateWrapperElement,
                          FiberCreateWrapperElement, WasmElementRef,
                          HiddenCreateParent);

BINDING(kAppendElementBinding, tasm::kCFunctionAppendElement,
        FiberAppendElement, WasmElementRef, WasmElementRef, WasmElementRef);
BINDING(kRemoveElementBinding, tasm::kCFunctionRemoveElement,
        FiberRemoveElement, WasmElementRef, WasmElementRef, WasmElementRef);
BINDING(kFirstElementBinding, tasm::kCFunctionFirstElement, FiberFirstElement,
        WasmElementRef, WasmElementRef);
BINDING(kLastElementBinding, tasm::kCFunctionLastElement, FiberLastElement,
        WasmElementRef, WasmElementRef);
BINDING(kNextElementBinding, tasm::kCFunctionNextElement, FiberNextElement,
        WasmElementRef, WasmElementRef);
BINDING(kReplaceElementBinding, tasm::kCFunctionReplaceElement,
        FiberReplaceElement, WasmVoid, WasmElementRef, WasmElementRef);
BINDING(kSwapElementBinding, tasm::kCFunctionSwapElement, FiberSwapElement,
        WasmVoid, WasmElementRef, WasmElementRef);
BINDING(kGetParentBinding, tasm::kCFunctionGetParent, FiberGetParent,
        WasmElementRef, WasmElementRef);
BINDING(kElementIsEqualBinding, tasm::kCFunctionElementIsEqual,
        FiberElementIsEqual, WasmBool, WasmElementRef, WasmElementRef);
BINDING(kGetElementUniqueIDBinding, tasm::kCFunctionGetElementUniqueID,
        FiberGetElementUniqueID, WasmI64, WasmElementRef);
BINDING(kGetTagBinding, tasm::kCFunctionGetTag, FiberGetTag,
        WasmString, WasmElementRef);
BINDING(kAddClassBinding, tasm::kCFunctionAddClass, FiberAddClass,
        WasmVoid, WasmElementRef, WasmString);
BINDING(kSetClassesBinding, tasm::kCFunctionSetClasses, FiberSetClasses,
        WasmVoid, WasmElementRef, WasmString);
BINDING(kGetIDBinding, tasm::kCFunctionGetID, FiberGetID, WasmString,
        WasmElementRef);
BINDING0(kGetPageElementBinding, tasm::kCFunctionGetPageElement,
         FiberGetPageElement, WasmElementRef);
BINDING(kGetElementByUniqueIDBinding, tasm::kCFunctionGetElementByUniqueID,
        FiberGetElementByUniqueID, WasmElementRef, WasmI64);

#undef BINDING0_WITH_CREATE_ARGS
#undef BINDING0
#undef BINDING_WITH_CREATE_ARGS
#undef BINDING

constexpr const char kDropElementBindingSymbol[] = "binding__DropElement";
constexpr const char kDropEventBindingSymbol[] = "binding__DropEvent";
constexpr const char kSetStringAttributeSymbol[] = "__SetStringAttribute";
constexpr const char kRemoveAttributeSymbol[] = "__RemoveAttribute";
constexpr const char kAdoptStyleSheetTokensSymbol[] = "__AdoptStyleSheetTokens";
constexpr const char kReplaceStyleSheetsTokensSymbol[] =
    "__ReplaceStyleSheetsTokens";
constexpr const char kSetBackgroundColorRgbSymbol[] =
    "__SetBackgroundColorRgb";
constexpr const char kCreateViewWithClassAndBackgroundColorRgbSymbol[] =
    "__CreateViewWithClassAndBackgroundColorRgb";
constexpr const char kGetStringAttributeByNameSymbol[] =
    "__GetStringAttributeByName";
constexpr const char kGetEventTypeSymbol[] = "__GetEventType";
constexpr const char kGetEventCurrentTargetUniqueIDSymbol[] =
    "__GetEventCurrentTargetUniqueID";

void DropElementHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  DropElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                 kDropElementBindingSymbol);
}

void DropEventHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  DropEventRef(exec_env, static_cast<int32_t>(raw_args[0]),
               kDropEventBindingSymbol);
}

std::vector<int32_t> ReadI32Array(wasm_exec_env_t exec_env, int32_t data_offset,
                                  int32_t count,
                                  const char* function_name, bool* ok) {
  int32_t byte_length = 0;
  if (!CheckedByteLength(count, static_cast<int32_t>(sizeof(int32_t)),
                         &byte_length)) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " array length is invalid");
    *ok = false;
    return {};
  }
  if (!ValidateAppMemory(exec_env, data_offset, byte_length, function_name,
                         " input array")) {
    *ok = false;
    return {};
  }
  std::vector<int32_t> values;
  values.reserve(static_cast<size_t>(count));
  if (count == 0) {
    return values;
  }
  auto* raw = static_cast<const int32_t*>(AppAddrToNative(exec_env,
                                                          data_offset));
  for (int32_t i = 0; i < count; ++i) {
    values.emplace_back(raw[i]);
  }
  return values;
}

base::Vector<ElementRef> ReadElementArray(wasm_exec_env_t exec_env,
                                          int32_t data_offset, int32_t count,
                                          const char* function_name,
                                          bool* ok) {
  auto ids = ReadI32Array(exec_env, data_offset, count, function_name, ok);
  base::Vector<ElementRef> elements;
  if (!*ok) {
    return elements;
  }
  elements.reserve(ids.size());
  for (int32_t id : ids) {
    auto element = GetElementRef(exec_env, id, function_name, ok);
    if (!*ok) {
      elements.clear();
      return elements;
    }
    elements.emplace_back(std::move(element));
  }
  return elements;
}

int32_t WriteI32Array(wasm_exec_env_t exec_env,
                      const std::vector<int32_t>& values, int32_t out_offset,
                      int32_t out_capacity, const char* function_name,
                      bool* ok) {
  const int32_t required_count =
      values.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())
          ? std::numeric_limits<int32_t>::max()
          : static_cast<int32_t>(values.size());
  if (out_capacity < 0) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " output array capacity must not be negative");
    *ok = false;
    return required_count;
  }
  if (required_count > out_capacity || required_count == 0) {
    return required_count;
  }
  int32_t byte_length = 0;
  if (!CheckedByteLength(required_count, static_cast<int32_t>(sizeof(int32_t)),
                         &byte_length) ||
      !ValidateAppMemory(exec_env, out_offset, byte_length, function_name,
                         " output array")) {
    *ok = false;
    return required_count;
  }
  auto* out = static_cast<int32_t*>(AppAddrToNative(exec_env, out_offset));
  std::memcpy(out, values.data(), static_cast<size_t>(byte_length));
  return required_count;
}

int32_t WriteElementArray(wasm_exec_env_t exec_env,
                          const std::vector<ElementRef>& elements,
                          int32_t out_offset, int32_t out_capacity,
                          const char* function_name) {
  bool ok = true;
  if (out_capacity >= 0 &&
      elements.size() <= static_cast<size_t>(out_capacity)) {
    auto state = GetWasmModuleState(exec_env, function_name);
    if (!state) {
      return 0;
    }
    std::vector<int32_t> ids;
    ids.reserve(elements.size());
    for (const auto& element : elements) {
      ids.emplace_back(
          StoreElementRef(state, element, exec_env, function_name));
    }
    return WriteI32Array(exec_env, ids, out_offset, out_capacity,
                         function_name, &ok);
  }
  return elements.size() >
                 static_cast<size_t>(std::numeric_limits<int32_t>::max())
             ? std::numeric_limits<int32_t>::max()
             : static_cast<int32_t>(elements.size());
}

void WriteRequiredBytes(wasm_exec_env_t exec_env, int32_t required_bytes_offset,
                        int32_t required_bytes, const char* function_name,
                        bool* ok) {
  if (!ValidateAppMemory(exec_env, required_bytes_offset,
                         static_cast<int32_t>(sizeof(int32_t)), function_name,
                         " required byte count")) {
    *ok = false;
    return;
  }
  auto* out =
      static_cast<int32_t*>(AppAddrToNative(exec_env, required_bytes_offset));
  *out = required_bytes;
}

int32_t WriteStringArray(wasm_exec_env_t exec_env,
                         const std::vector<std::string>& values,
                         int32_t items_offset, int32_t item_capacity,
                         int32_t bytes_offset, int32_t byte_capacity,
                         int32_t required_bytes_offset,
                         const char* function_name) {
  bool ok = true;
  int32_t required_bytes = 0;
  for (const auto& value : values) {
    if (value.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max() -
                                           required_bytes)) {
      required_bytes = std::numeric_limits<int32_t>::max();
      break;
    }
    required_bytes += static_cast<int32_t>(value.size());
  }
  WriteRequiredBytes(exec_env, required_bytes_offset, required_bytes,
                     function_name, &ok);
  if (!ok) {
    return 0;
  }

  const int32_t required_items =
      values.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())
          ? std::numeric_limits<int32_t>::max()
          : static_cast<int32_t>(values.size());
  if (item_capacity < 0 || byte_capacity < 0) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " output capacity must not be negative");
    return required_items;
  }
  if (required_items > item_capacity || required_bytes > byte_capacity ||
      required_items == 0) {
    return required_items;
  }

  int32_t item_bytes = 0;
  if (!CheckedByteLength(required_items, 2 * static_cast<int32_t>(sizeof(int32_t)),
                         &item_bytes) ||
      !ValidateAppMemory(exec_env, items_offset, item_bytes, function_name,
                         " output string item array") ||
      !ValidateAppMemory(exec_env, bytes_offset, required_bytes, function_name,
                         " output string bytes")) {
    return required_items;
  }

  auto* items = static_cast<int32_t*>(AppAddrToNative(exec_env, items_offset));
  auto* bytes = static_cast<char*>(AppAddrToNative(exec_env, bytes_offset));
  int32_t cursor = 0;
  for (int32_t i = 0; i < required_items; ++i) {
    items[2 * i] = cursor;
    items[2 * i + 1] = static_cast<int32_t>(values[static_cast<size_t>(i)].size());
    if (!values[static_cast<size_t>(i)].empty()) {
      std::memcpy(bytes + cursor, values[static_cast<size_t>(i)].data(),
                  values[static_cast<size_t>(i)].size());
      cursor += static_cast<int32_t>(values[static_cast<size_t>(i)].size());
    }
  }
  return required_items;
}

void InsertElementBeforeHostFunction(wasm_exec_env_t exec_env,
                                     uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionInsertElementBefore;
  bool ok = true;
  auto parent = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                              kFunction, &ok);
  auto child = GetElementRef(exec_env, static_cast<int32_t>(raw_args[1]),
                             kFunction, &ok);
  auto before = GetOptionalElementRef(exec_env, static_cast<int32_t>(raw_args[2]),
                                      kFunction, &ok);
  if (!ok) {
    raw_args[0] = static_cast<uint32_t>(kNullHostRef);
    return;
  }
  if (before) {
    parent->InsertNodeBefore(child, before);
  } else {
    parent->InsertNode(child);
  }
  raw_args[0] = raw_args[1];
}

void GetChildrenHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionGetChildren;
  bool ok = true;
  auto parent = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                              kFunction, &ok);
  if (!ok) {
    raw_args[0] = 0;
    return;
  }
  std::vector<ElementRef> children;
  children.reserve(parent->children().size());
  for (const auto& child : parent->children()) {
    children.emplace_back(fml::static_ref_ptr_cast<tasm::FiberElement>(child));
  }
  raw_args[0] = static_cast<uint32_t>(WriteElementArray(
      exec_env, children, static_cast<int32_t>(raw_args[1]),
      static_cast<int32_t>(raw_args[2]), kFunction));
}

void SetStringAttributeHostFunction(wasm_exec_env_t exec_env,
                                    uint64_t* raw_args) {
  constexpr const char* kFunction = kSetStringAttributeSymbol;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  std::string key = ReadUtf8String(exec_env, static_cast<int32_t>(raw_args[1]),
                                   static_cast<int32_t>(raw_args[2]), kFunction,
                                   &ok);
  std::string value =
      ReadUtf8String(exec_env, static_cast<int32_t>(raw_args[3]),
                     static_cast<int32_t>(raw_args[4]), kFunction, &ok);
  if (!ok) {
    return;
  }
  if (key.empty()) {
    SetException(exec_env, ExceptionPrefix(kFunction) +
                               " attribute key must not be empty");
    return;
  }
  if (key == kStyleAttribute) {
    element->RemoveAllInlineStyles();
    element->SetRawInlineStyles(base::String(value));
    return;
  }
  element->SetAttribute(base::String(key), lepus::Value(value));
}

void RemoveAttributeHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  constexpr const char* kFunction = kRemoveAttributeSymbol;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  std::string key = ReadUtf8String(exec_env, static_cast<int32_t>(raw_args[1]),
                                   static_cast<int32_t>(raw_args[2]), kFunction,
                                   &ok);
  if (!ok) {
    return;
  }
  if (key.empty()) {
    SetException(exec_env, ExceptionPrefix(kFunction) +
                               " attribute key must not be empty");
    return;
  }
  if (key == kStyleAttribute) {
    element->RemoveAllInlineStyles();
    return;
  }
  element->SetAttribute(base::String(key), lepus::Value());
}

void MarkTreeStyleDirty(tasm::ElementManager* manager) {
  if (!manager) {
    return;
  }
  auto root = static_cast<tasm::FiberElement*>(manager->root());
  if (root) {
    root->ApplyFunctionRecursive(
        [](auto element) { element->MarkStyleDirty(false); });
  }
}

tasm::TemplateAssembler* GetTemplateAssembler(wasm_exec_env_t exec_env,
                                              const char* function_name) {
  auto* context = GetEngineHostContext(exec_env);
  if (context == nullptr) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " missing Lynx runtime context");
    return nullptr;
  }
  auto* runtime = MTSRuntime::ToContext(context);
  if (runtime == nullptr || runtime->GetDelegate() == nullptr) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " missing Lynx runtime delegate");
    return nullptr;
  }
  return static_cast<tasm::TemplateAssembler*>(runtime->GetDelegate());
}

void ApplyStyleSheetTokensHostFunction(wasm_exec_env_t exec_env,
                                       uint64_t* raw_args,
                                       const char* function_name,
                                       bool replace) {
  auto* tasm = GetTemplateAssembler(exec_env, function_name);
  if (!tasm || !tasm->page_proxy()) {
    return;
  }

  const int32_t bytes_offset = static_cast<int32_t>(raw_args[0]);
  const int32_t bytes_length = static_cast<int32_t>(raw_args[1]);
  if (!ValidateAppMemory(exec_env, bytes_offset, bytes_length, function_name,
                         " CSS token stream")) {
    return;
  }
  if (bytes_length <= 0) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " CSS token stream must not be empty");
    return;
  }

  auto* bytes =
      static_cast<const uint8_t*>(AppAddrToNative(exec_env, bytes_offset));
  tasm::css_wasm::CSSTokenStreamView stream(
      bytes, static_cast<size_t>(bytes_length));
  auto* manager = tasm->page_proxy()->element_manager().get();
  if (!manager) {
    SetException(exec_env,
                 ExceptionPrefix(function_name) + " missing ElementManager");
    return;
  }

  const bool enable_css_invalidation =
      tasm->GetPageConfig() && tasm->GetPageConfig()->GetEnableCSSInvalidation();
  auto result = tasm::css_wasm::ParseStyleSheetTokenStream(
      stream, manager->GetCSSParserConfigs(), enable_css_invalidation);
  if (!result.fragment) {
    SetException(exec_env,
                 ExceptionPrefix(function_name) + " " + result.error);
    return;
  }

  if (replace) {
    manager->ClearAdoptedStyleSheets();
  }
  auto wrapper = fml::MakeRefCounted<tasm::SharedCSSFragmentWrapper>(
      std::move(result.fragment));
  manager->AdoptStyleSheet(std::move(wrapper));
  MarkTreeStyleDirty(manager);
}

void AdoptStyleSheetTokensHostFunction(wasm_exec_env_t exec_env,
                                       uint64_t* raw_args) {
  ApplyStyleSheetTokensHostFunction(exec_env, raw_args,
                                    kAdoptStyleSheetTokensSymbol, false);
}

void ReplaceStyleSheetsTokensHostFunction(wasm_exec_env_t exec_env,
                                          uint64_t* raw_args) {
  ApplyStyleSheetTokensHostFunction(exec_env, raw_args,
                                    kReplaceStyleSheetsTokensSymbol, true);
}

void ApplyBackgroundColorRgb(const ElementRef& element, uint32_t rgb) {
  const uint32_t argb = 0xff000000u | (rgb & 0x00ffffffu);
  element->SetStyle(tasm::kPropertyIDBackgroundColor, lepus::Value(argb));
}

void SetBackgroundColorRgbHostFunction(wasm_exec_env_t exec_env,
                                       uint64_t* raw_args) {
  constexpr const char* kFunction = kSetBackgroundColorRgbSymbol;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  if (!ok) {
    return;
  }

  ApplyBackgroundColorRgb(element, static_cast<uint32_t>(raw_args[1]));
}

void CreateViewWithClassAndBackgroundColorRgbHostFunction(
    wasm_exec_env_t exec_env, uint64_t* raw_args) {
  constexpr const char* kFunction =
      kCreateViewWithClassAndBackgroundColorRgbSymbol;
  auto* context = GetEngineHostContext(exec_env);
  if (context == nullptr) {
    SetException(exec_env, ExceptionPrefix(kFunction) +
                               " missing Lynx runtime context");
    raw_args[0] = static_cast<uint32_t>(kNullHostRef);
    return;
  }

  bool ok = true;
  std::string class_name =
      ReadUtf8String(exec_env, static_cast<int32_t>(raw_args[0]),
                     static_cast<int32_t>(raw_args[1]), kFunction, &ok);
  if (!ok) {
    raw_args[0] = static_cast<uint32_t>(kNullHostRef);
    return;
  }

  lepus::Value args[] = {lepus::Value(0.0)};
  lepus::Value result =
      tasm::RendererFunctions::FiberCreateView(context, args, 1);
  if (result.IsEmpty() || result.IsNil() || result.IsUndefined() ||
      !result.IsRefCounted() ||
      result.RefCounted()->GetRefType() != lepus::RefType::kElement) {
    SetException(exec_env, ExceptionPrefix(kFunction) +
                               " return value is not a FiberElement");
    raw_args[0] = static_cast<uint32_t>(kNullHostRef);
    return;
  }

  auto element =
      fml::static_ref_ptr_cast<tasm::FiberElement>(result.RefCounted());
  if (!class_name.empty()) {
    tasm::ClassList old_classes = element->ReleaseClasses();
    element->RemoveAllClass();
    element->SetClass(base::String(std::move(class_name)));
    element->OnClassChanged(old_classes, element->classes());
  }
  ApplyBackgroundColorRgb(element, static_cast<uint32_t>(raw_args[2]));
  raw_args[0] =
      static_cast<uint32_t>(StoreElementRef(exec_env, element, kFunction));
}

void SetIDHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionSetID;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  int32_t value_offset = static_cast<int32_t>(raw_args[1]);
  int32_t value_length = static_cast<int32_t>(raw_args[2]);
  if (!ok) {
    return;
  }
  if (value_offset < 0 || value_length < 0) {
    element->SetIdSelector(base::String());
    return;
  }
  std::string value =
      ReadUtf8String(exec_env, value_offset, value_length, kFunction, &ok);
  if (!ok) {
    return;
  }
  element->SetIdSelector(base::String(value));
}

void FlushElementTreeHostFunction(wasm_exec_env_t exec_env,
                                  uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionFlushElementTree;
  auto* context = GetEngineHostContext(exec_env);
  if (context == nullptr) {
    SetException(exec_env, ExceptionPrefix(kFunction) +
                               " missing Lynx runtime context");
    return;
  }
  bool ok = true;
  int32_t root_id = static_cast<int32_t>(raw_args[0]);
  std::vector<lepus::Value> args;
  if (root_id >= 0) {
    auto root = GetElementRef(exec_env, root_id, kFunction, &ok);
    if (!ok) {
      return;
    }
    args.emplace_back(root);
  }
  auto pipeline_options = CurrentEngineHostPipelineOptions();
  if (pipeline_options != nullptr) {
    if (args.empty()) {
      args.emplace_back(lepus::Value());
    }
    lepus::Value options(lepus::Dictionary::Create());
    BASE_STATIC_STRING_DECL(kPipelineOptions, "pipelineOptions");
    options.SetProperty(kPipelineOptions,
                        tasm::PipelineOptionsToLepusValue(pipeline_options));
    args.emplace_back(std::move(options));
  }
  tasm::RendererFunctions::FiberFlushElementTree(
      context, args.empty() ? nullptr : args.data(),
      static_cast<int>(args.size()));
}

void ReplaceElementsHostFunction(wasm_exec_env_t exec_env,
                                 uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionReplaceElements;
  bool ok = true;
  auto parent = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                              kFunction, &ok);
  auto inserted =
      ReadElementArray(exec_env, static_cast<int32_t>(raw_args[1]),
                       static_cast<int32_t>(raw_args[2]), kFunction, &ok);
  auto removed =
      ReadElementArray(exec_env, static_cast<int32_t>(raw_args[3]),
                       static_cast<int32_t>(raw_args[4]), kFunction, &ok);
  auto ref = GetOptionalElementRef(exec_env, static_cast<int32_t>(raw_args[5]),
                                   kFunction, &ok);
  if (!ok) {
    return;
  }
  if (parent->is_block()) {
    static_cast<tasm::BlockElement*>(parent.get())
        ->ReplaceElements(inserted, removed);
    return;
  }
  parent->ReplaceElements(inserted, removed, ref.get());
}

tasm::NodeSelectOptions MakeCssSelectorOptions(std::string selector,
                                               int32_t only_current_component) {
  tasm::NodeSelectOptions options(
      tasm::NodeSelectOptions::IdentifierType::CSS_SELECTOR, selector);
  options.only_current_component = only_current_component != 0;
  return options;
}

void QuerySelectorHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionQuerySelector;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  std::string selector =
      ReadUtf8String(exec_env, static_cast<int32_t>(raw_args[1]),
                     static_cast<int32_t>(raw_args[2]), kFunction, &ok);
  if (!ok) {
    raw_args[0] = static_cast<uint32_t>(kNullHostRef);
    return;
  }
  auto options = MakeCssSelectorOptions(std::move(selector),
                                        static_cast<int32_t>(raw_args[3]));
  auto result = tasm::FiberElementSelector::Select(element.get(), options);
  if (!result.Success()) {
    raw_args[0] = static_cast<uint32_t>(kNullHostRef);
    return;
  }
  raw_args[0] = static_cast<uint32_t>(
      StoreElementRef(exec_env, ElementRef(result.GetOneNode()), kFunction));
}

void QuerySelectorAllHostFunction(wasm_exec_env_t exec_env,
                                  uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionQuerySelectorAll;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  std::string selector =
      ReadUtf8String(exec_env, static_cast<int32_t>(raw_args[1]),
                     static_cast<int32_t>(raw_args[2]), kFunction, &ok);
  if (!ok) {
    raw_args[0] = 0;
    return;
  }
  auto options = MakeCssSelectorOptions(std::move(selector),
                                        static_cast<int32_t>(raw_args[5]));
  options.first_only = false;
  auto result = tasm::FiberElementSelector::Select(element.get(), options);
  std::vector<ElementRef> elements;
  elements.reserve(result.nodes.size());
  for (auto* node : result.nodes) {
    elements.emplace_back(ElementRef(node));
  }
  raw_args[0] = static_cast<uint32_t>(WriteElementArray(
      exec_env, elements, static_cast<int32_t>(raw_args[3]),
      static_cast<int32_t>(raw_args[4]), kFunction));
}

void GetClassesHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionGetClasses;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  if (!ok) {
    raw_args[0] = 0;
    return;
  }
  std::vector<std::string> values;
  for (const auto& clazz : element->classes()) {
    values.emplace_back(clazz.str());
  }
  raw_args[0] = static_cast<uint32_t>(WriteStringArray(
      exec_env, values, static_cast<int32_t>(raw_args[1]),
      static_cast<int32_t>(raw_args[2]), static_cast<int32_t>(raw_args[3]),
      static_cast<int32_t>(raw_args[4]), static_cast<int32_t>(raw_args[5]),
      kFunction));
}

void GetAttributeNamesHostFunction(wasm_exec_env_t exec_env,
                                   uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionGetAttributeNames;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  if (!ok) {
    raw_args[0] = 0;
    return;
  }
  std::vector<std::string> values;
  for (const auto& pair : element->data_model()->attributes()) {
    values.emplace_back(pair.first.str());
  }
  const auto& builtin_attr_map = element->builtin_attr_map();
  if (builtin_attr_map.has_value()) {
    for (const auto& pair : *builtin_attr_map) {
      values.emplace_back(std::to_string(pair.first));
    }
  }
  raw_args[0] = static_cast<uint32_t>(WriteStringArray(
      exec_env, values, static_cast<int32_t>(raw_args[1]),
      static_cast<int32_t>(raw_args[2]), static_cast<int32_t>(raw_args[3]),
      static_cast<int32_t>(raw_args[4]), static_cast<int32_t>(raw_args[5]),
      kFunction));
}

void GetStringAttributeByNameHostFunction(wasm_exec_env_t exec_env,
                                          uint64_t* raw_args) {
  constexpr const char* kFunction = kGetStringAttributeByNameSymbol;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  std::string key = ReadUtf8String(exec_env, static_cast<int32_t>(raw_args[1]),
                                   static_cast<int32_t>(raw_args[2]), kFunction,
                                   &ok);
  if (!ok) {
    raw_args[0] = static_cast<uint32_t>(-1);
    return;
  }
  const auto& attr_map = element->data_model()->attributes();
  auto iter = attr_map.find(base::String(key));
  if (iter == attr_map.end() || !iter->second.IsString()) {
    raw_args[0] = static_cast<uint32_t>(-1);
    return;
  }
  raw_args[0] = static_cast<uint32_t>(
      WriteUtf8String(exec_env, iter->second.StdString(),
                      static_cast<int32_t>(raw_args[3]),
                      static_cast<int32_t>(raw_args[4]), kFunction, &ok));
}

event::EventListener::Options DecodeListenerOptions(int32_t flags) {
  return event::EventListener::Options(
      (flags & event::EventListener::Options::kCaptureBit) != 0,
      (flags & event::EventListener::Options::kOnceBit) != 0,
      (flags & event::EventListener::Options::kPassiveBit) != 0,
      (flags & event::EventListener::Options::kSignalBit) != 0,
      (flags & event::EventListener::Options::kCatchBit) != 0,
      (flags & event::EventListener::Options::kGlobalBit) != 0);
}

class WasmEventListener : public event::EventListener {
 public:
  WasmEventListener(std::shared_ptr<WasmModuleTimerState> state,
                    uint32_t handler_id,
                    const event::EventListener::Options& options)
      : event::EventListener(
            event::EventListener::Type::kClosureEventListener, options),
        state_(std::move(state)),
        handler_id_(handler_id) {}

  void Invoke(EventRef event) override {
    InvokeWasmEventCallback(state_, handler_id_, event,
                            tasm::kCFunctionAddEventListener);
  }

  bool Matches(event::EventListener* listener) override {
    if (listener->type() != type()) {
      return false;
    }
    auto* other = static_cast<WasmEventListener*>(listener);
    return handler_id_ == other->handler_id_ &&
           options_.flags == other->GetOptions().flags;
  }

 private:
  std::shared_ptr<WasmModuleTimerState> state_;
  uint32_t handler_id_;
};

void AddEventListenerHostFunction(wasm_exec_env_t exec_env,
                                  uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionAddEventListener;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  std::string event_type =
      ReadUtf8String(exec_env, static_cast<int32_t>(raw_args[1]),
                     static_cast<int32_t>(raw_args[2]), kFunction, &ok);
  if (!ok) {
    return;
  }
  auto state = GetWasmModuleState(exec_env, kFunction);
  if (!state) {
    return;
  }
  const uint32_t handler_id = static_cast<uint32_t>(raw_args[3]);
  const auto options = DecodeListenerOptions(static_cast<int32_t>(raw_args[4]));
  state->event_listener_count++;
  element->SetJSEventHandler(base::String(event_type), base::String(),
                             base::String());
  element->AddEventListener(
      event_type,
      std::make_shared<WasmEventListener>(std::move(state), handler_id,
                                          options));
}

void RemoveEventListenerHostFunction(wasm_exec_env_t exec_env,
                                     uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionFiberRemoveEventListener;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  std::string event_type =
      ReadUtf8String(exec_env, static_cast<int32_t>(raw_args[1]),
                     static_cast<int32_t>(raw_args[2]), kFunction, &ok);
  if (!ok) {
    return;
  }
  const uint32_t handler_id = static_cast<uint32_t>(raw_args[3]);
  const auto options = DecodeListenerOptions(static_cast<int32_t>(raw_args[4]));
  auto state = GetWasmModuleState(exec_env, kFunction);
  if (state && state->event_listener_count > 0) {
    state->event_listener_count--;
    DeferEraseFinishedWasmModuleTimerState(state);
  }
  element->RemoveEvent(base::String(event_type), base::String());
  element->RemoveEventListener(
      event_type,
      std::make_shared<WasmEventListener>(nullptr, handler_id, options));
}

void CreateEventHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionCreateEvent;
  bool ok = true;
  int32_t type = static_cast<int32_t>(raw_args[0]);
  std::string name = ReadUtf8String(exec_env, static_cast<int32_t>(raw_args[1]),
                                    static_cast<int32_t>(raw_args[2]),
                                    kFunction, &ok);
  int32_t flags = static_cast<int32_t>(raw_args[3]);
  if (!ok) {
    raw_args[0] = static_cast<uint32_t>(kNullHostRef);
    return;
  }
  int64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  lepus::Value detail(lepus::Dictionary::Create());
  auto event = fml::MakeRefCounted<event::Event>(
      name, timestamp, static_cast<event::Event::EventType>(type),
      (flags & kEventFlagCapture) != 0 ? event::Event::Capture::kYes
                                       : event::Event::Capture::kNo,
      (flags & kEventFlagBubbles) != 0 ? event::Event::Bubbles::kYes
                                       : event::Event::Bubbles::kNo,
      (flags & kEventFlagCancelable) != 0
          ? event::Event::Cancelable::kYes
          : event::Event::Cancelable::kNo,
      (flags & kEventFlagComposed) != 0 ? event::Event::ComposedMode::kScoped
                                        : event::Event::ComposedMode::kComposed,
      event::Event::PhaseType::kNone, detail);
  raw_args[0] =
      static_cast<uint32_t>(StoreEventRef(exec_env, event, kFunction));
}

void DispatchEventHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionDispatchEvent;
  bool ok = true;
  auto element = GetElementRef(exec_env, static_cast<int32_t>(raw_args[0]),
                               kFunction, &ok);
  auto event = GetEventRef(exec_env, static_cast<int32_t>(raw_args[1]),
                           kFunction, &ok);
  if (!ok) {
    raw_args[0] = 0;
    return;
  }
  bool result =
      event::EventDispatcher::DispatchEvent(*element.get(), event).cancel_type ==
      event::EventCancelType::kNotCanceled;
  raw_args[0] = result ? 1 : 0;
}

void StopPropagationHostFunction(wasm_exec_env_t exec_env,
                                 uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionStopPropagation;
  bool ok = true;
  auto event = GetEventRef(exec_env, static_cast<int32_t>(raw_args[0]),
                           kFunction, &ok);
  if (ok) {
    event->set_is_stop_propagation(true);
  }
}

void StopImmediatePropagationHostFunction(wasm_exec_env_t exec_env,
                                          uint64_t* raw_args) {
  constexpr const char* kFunction = tasm::kCFunctionStopImmediatePropagation;
  bool ok = true;
  auto event = GetEventRef(exec_env, static_cast<int32_t>(raw_args[0]),
                           kFunction, &ok);
  if (ok) {
    event->set_is_stop_immediate_propagation(true);
  }
}

void GetEventTypeHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  bool ok = true;
  auto event = GetEventRef(exec_env, static_cast<int32_t>(raw_args[0]),
                           kGetEventTypeSymbol, &ok);
  if (!ok) {
    raw_args[0] = static_cast<uint32_t>(kNullHostRef);
    return;
  }
  raw_args[0] = static_cast<uint32_t>(WriteUtf8String(
      exec_env, event->type(), static_cast<int32_t>(raw_args[1]),
      static_cast<int32_t>(raw_args[2]), kGetEventTypeSymbol, &ok));
}

void GetEventCurrentTargetUniqueIDHostFunction(wasm_exec_env_t exec_env,
                                               uint64_t* raw_args) {
  bool ok = true;
  auto event = GetEventRef(exec_env, static_cast<int32_t>(raw_args[0]),
                           kGetEventCurrentTargetUniqueIDSymbol, &ok);
  if (!ok) {
    raw_args[0] = static_cast<uint64_t>(-1);
    return;
  }
  auto current_target = event->current_target();
  if (!current_target) {
    raw_args[0] = static_cast<uint64_t>(-1);
    return;
  }
  raw_args[0] =
      static_cast<uint64_t>(std::stoll(current_target->GetUniqueID()));
}

#define WASM_I32 "i"
#define WASM_I64 "I"
#define WASM_BOOL "i"
#define WASM_STRING "ii"
#define WASM_REF "i"
#define WASM_FUNC_REF "i"
#define WASM_STRING_OUT "ii"
#define SIG(args, result) "(" args ")" result

#define SYMBOL(binding, signature)                                         \
  { binding##Symbol, reinterpret_cast<void*>(binding##HostFunction),        \
    signature, nullptr }

#define CUSTOM_SYMBOL(symbol, function, signature) \
  { symbol, reinterpret_cast<void*>(function), signature, nullptr }

NativeSymbol g_engine_host_symbols[] = {
    SYMBOL(kCreateElementBinding, SIG(WASM_STRING, WASM_REF)),
    SYMBOL(kCreatePageBinding, SIG("", WASM_REF)),
    SYMBOL(kCreateViewBinding, SIG("", WASM_REF)),
    SYMBOL(kCreateScrollViewBinding, SIG("", WASM_REF)),
    SYMBOL(kCreateTextBinding, SIG("", WASM_REF)),
    SYMBOL(kCreateImageBinding, SIG("", WASM_REF)),
    SYMBOL(kCreateRawTextBinding, SIG(WASM_STRING, WASM_REF)),
    SYMBOL(kCreateNonElementBinding, SIG("", WASM_REF)),
    SYMBOL(kCreateWrapperElementBinding, SIG("", WASM_REF)),
    CUSTOM_SYMBOL(kDropElementBindingSymbol, DropElementHostFunction,
                  SIG(WASM_REF, "")),
    CUSTOM_SYMBOL(kDropEventBindingSymbol, DropEventHostFunction,
                  SIG(WASM_REF, "")),
    SYMBOL(kAppendElementBinding, SIG(WASM_REF WASM_REF, WASM_REF)),
    SYMBOL(kRemoveElementBinding, SIG(WASM_REF WASM_REF, WASM_REF)),
    CUSTOM_SYMBOL(tasm::kCFunctionInsertElementBefore,
                  InsertElementBeforeHostFunction,
                  SIG(WASM_REF WASM_REF WASM_REF, WASM_REF)),
    SYMBOL(kFirstElementBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kLastElementBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kNextElementBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kReplaceElementBinding, SIG(WASM_REF WASM_REF, "")),
    SYMBOL(kSwapElementBinding, SIG(WASM_REF WASM_REF, "")),
    SYMBOL(kGetParentBinding, SIG(WASM_REF, WASM_REF)),
    CUSTOM_SYMBOL(tasm::kCFunctionGetChildren, GetChildrenHostFunction,
                  SIG(WASM_REF WASM_I32 WASM_I32, WASM_I32)),
    SYMBOL(kElementIsEqualBinding, SIG(WASM_REF WASM_REF, WASM_BOOL)),
    SYMBOL(kGetElementUniqueIDBinding, SIG(WASM_REF, WASM_I64)),
    SYMBOL(kGetTagBinding, SIG(WASM_REF WASM_STRING_OUT, WASM_I32)),
    CUSTOM_SYMBOL(kSetStringAttributeSymbol, SetStringAttributeHostFunction,
                  SIG(WASM_REF WASM_STRING WASM_STRING, "")),
    CUSTOM_SYMBOL(kRemoveAttributeSymbol, RemoveAttributeHostFunction,
                  SIG(WASM_REF WASM_STRING, "")),
    CUSTOM_SYMBOL(kAdoptStyleSheetTokensSymbol, AdoptStyleSheetTokensHostFunction,
                  SIG(WASM_STRING, "")),
    CUSTOM_SYMBOL(kReplaceStyleSheetsTokensSymbol,
                  ReplaceStyleSheetsTokensHostFunction, SIG(WASM_STRING, "")),
    CUSTOM_SYMBOL(kSetBackgroundColorRgbSymbol, SetBackgroundColorRgbHostFunction,
                  SIG(WASM_REF WASM_I32, "")),
    CUSTOM_SYMBOL(kCreateViewWithClassAndBackgroundColorRgbSymbol,
                  CreateViewWithClassAndBackgroundColorRgbHostFunction,
                  SIG(WASM_STRING WASM_I32, WASM_REF)),
    SYMBOL(kAddClassBinding, SIG(WASM_REF WASM_STRING, "")),
    SYMBOL(kSetClassesBinding, SIG(WASM_REF WASM_STRING, "")),
    CUSTOM_SYMBOL(tasm::kCFunctionGetClasses, GetClassesHostFunction,
                  SIG(WASM_REF WASM_I32 WASM_I32 WASM_I32 WASM_I32 WASM_I32,
                      WASM_I32)),
    CUSTOM_SYMBOL(tasm::kCFunctionSetID, SetIDHostFunction,
                  SIG(WASM_REF WASM_STRING, "")),
    SYMBOL(kGetIDBinding, SIG(WASM_REF WASM_STRING_OUT, WASM_I32)),
    CUSTOM_SYMBOL(tasm::kCFunctionFlushElementTree, FlushElementTreeHostFunction,
                  SIG(WASM_REF, "")),
    CUSTOM_SYMBOL(tasm::kCFunctionReplaceElements, ReplaceElementsHostFunction,
                  SIG(WASM_REF WASM_I32 WASM_I32 WASM_I32 WASM_I32 WASM_REF,
                      "")),
    CUSTOM_SYMBOL(tasm::kCFunctionQuerySelector, QuerySelectorHostFunction,
                  SIG(WASM_REF WASM_STRING WASM_I32, WASM_REF)),
    CUSTOM_SYMBOL(tasm::kCFunctionQuerySelectorAll, QuerySelectorAllHostFunction,
                  SIG(WASM_REF WASM_STRING WASM_I32 WASM_I32 WASM_I32,
                      WASM_I32)),
    CUSTOM_SYMBOL(kGetStringAttributeByNameSymbol,
                  GetStringAttributeByNameHostFunction,
                  SIG(WASM_REF WASM_STRING WASM_STRING_OUT, WASM_I32)),
    CUSTOM_SYMBOL(tasm::kCFunctionGetAttributeNames,
                  GetAttributeNamesHostFunction,
                  SIG(WASM_REF WASM_I32 WASM_I32 WASM_I32 WASM_I32 WASM_I32,
                      WASM_I32)),
    SYMBOL(kGetPageElementBinding, SIG("", WASM_REF)),
    SYMBOL(kGetElementByUniqueIDBinding, SIG(WASM_I64, WASM_REF)),
    CUSTOM_SYMBOL(tasm::kCFunctionAddEventListener,
                  AddEventListenerHostFunction,
                  SIG(WASM_REF WASM_STRING WASM_I32 WASM_I32, "")),
    CUSTOM_SYMBOL(tasm::kCFunctionFiberRemoveEventListener,
                  RemoveEventListenerHostFunction,
                  SIG(WASM_REF WASM_STRING WASM_I32 WASM_I32, "")),
    CUSTOM_SYMBOL(tasm::kCFunctionCreateEvent, CreateEventHostFunction,
                  SIG(WASM_I32 WASM_STRING WASM_I32, WASM_REF)),
    CUSTOM_SYMBOL(tasm::kCFunctionDispatchEvent, DispatchEventHostFunction,
                  SIG(WASM_REF WASM_REF, WASM_BOOL)),
    CUSTOM_SYMBOL(tasm::kCFunctionStopPropagation, StopPropagationHostFunction,
                  SIG(WASM_REF, "")),
    CUSTOM_SYMBOL(tasm::kCFunctionStopImmediatePropagation,
                  StopImmediatePropagationHostFunction, SIG(WASM_REF, "")),
    CUSTOM_SYMBOL(kGetEventTypeSymbol, GetEventTypeHostFunction,
                  SIG(WASM_REF WASM_STRING_OUT, WASM_I32)),
    CUSTOM_SYMBOL(kGetEventCurrentTargetUniqueIDSymbol,
                  GetEventCurrentTargetUniqueIDHostFunction,
                  SIG(WASM_REF, WASM_I64)),
    CUSTOM_SYMBOL(tasm::kSetTimeout, SetTimeoutHostFunction,
                  SIG(WASM_FUNC_REF WASM_I64, WASM_I64)),
    CUSTOM_SYMBOL(tasm::kClearTimeout, ClearTimeoutHostFunction,
                  SIG(WASM_I64, "")),
    CUSTOM_SYMBOL(tasm::kSetInterval, SetIntervalHostFunction,
                  SIG(WASM_FUNC_REF WASM_I64, WASM_I64)),
    CUSTOM_SYMBOL(tasm::kClearTimeInterval, ClearIntervalHostFunction,
                  SIG(WASM_I64, "")),
};

#undef CUSTOM_SYMBOL
#undef SYMBOL
#undef SIG
#undef WASM_STRING_OUT
#undef WASM_FUNC_REF
#undef WASM_REF
#undef WASM_STRING
#undef WASM_BOOL
#undef WASM_I64
#undef WASM_I32

}  // namespace

bool RegisterEngineHostFunctions() {
  static std::mutex mutex;

  std::lock_guard<std::mutex> lock(mutex);
  wasm_runtime_unregister_natives(kEngineHostModuleName, g_engine_host_symbols);
  return wasm_runtime_register_natives_raw(
      kEngineHostModuleName, g_engine_host_symbols,
      static_cast<uint32_t>(std::size(g_engine_host_symbols)));
}

void RegisterEngineHostModule(wasm_module_t module,
                              wasm_module_inst_t module_inst,
                              MTSContext* context) {
  auto state =
      std::make_shared<WasmModuleTimerState>(module, module_inst, context);
  std::lock_guard<std::mutex> lock(WasmModuleTimerStatesMutex());
  auto& states = WasmModuleTimerStates();
  auto old_state = states.find(module_inst);
  if (old_state != states.end() && old_state->second) {
    old_state->second->Shutdown();
  }
  states[module_inst] = std::move(state);
}

void FinishEngineHostModule(wasm_module_inst_t module_inst) {
  auto state = GetWasmModuleTimerState(module_inst);
  if (!state) {
    return;
  }
  state->entry_finished = true;
  MaybeEraseFinishedWasmModuleTimerState(state);
}

void DestroyEngineHostModule(wasm_module_inst_t module_inst) {
  std::shared_ptr<WasmModuleTimerState> state;
  {
    std::lock_guard<std::mutex> lock(WasmModuleTimerStatesMutex());
    auto& states = WasmModuleTimerStates();
    auto iter = states.find(module_inst);
    if (iter == states.end()) {
      return;
    }
    state = std::move(iter->second);
    states.erase(iter);
  }
  if (state) {
    state->Shutdown();
  }
  state.reset();
}

void DestroyEngineHostModulesForContext(MTSContext* context) {
  std::vector<std::shared_ptr<WasmModuleTimerState>> states_to_destroy;
  {
    std::lock_guard<std::mutex> lock(WasmModuleTimerStatesMutex());
    auto& states = WasmModuleTimerStates();
    for (auto iter = states.begin(); iter != states.end();) {
      if (iter->second && iter->second->context == context) {
        states_to_destroy.emplace_back(std::move(iter->second));
        iter = states.erase(iter);
      } else {
        ++iter;
      }
    }
  }
  for (auto& state : states_to_destroy) {
    if (state) {
      state->Shutdown();
    }
  }
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

void SetEngineHostPipelineOptions(
    std::shared_ptr<tasm::PipelineOptions> pipeline_options) {
  CurrentEngineHostPipelineOptions() = std::move(pipeline_options);
}

}  // namespace wasmr
}  // namespace runtime
}  // namespace lynx
