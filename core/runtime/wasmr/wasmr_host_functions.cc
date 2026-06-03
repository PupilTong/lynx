// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "core/runtime/wasmr/wasmr_host_functions.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/include/value/base_value.h"
#include "core/runtime/lepus/bindings/renderer.h"
#include "core/runtime/lepus/bindings/renderer_functions.h"
#include "core/renderer/utils/base/tasm_constants.h"

namespace lynx {
namespace runtime {
namespace wasmr {
namespace {

constexpr uintptr_t kNullExternRef = 0;
constexpr uintptr_t kInvalidExternRef = static_cast<uintptr_t>(-1);

enum class WasmArgKind {
  kI32,
  kI64,
  kF64,
  kBool,
  kString,
  kExternRef,
  kAny,
};

enum class WasmReturnKind {
  kVoid,
  kI32,
  kI64,
  kBool,
  kString,
  kExternRef,
  kAny,
};

enum class HiddenComponentId {
  kNone,
  kPrependNumberZero,
  kPrependStringZero,
  kInsertNumberZeroAfterFirstArg,
};

struct BindingDescriptor {
  const char* name;
  lepus::CFunction function;
  const WasmArgKind* args;
  size_t argc;
  WasmReturnKind return_kind;
  HiddenComponentId hidden_component_id;
};

struct EngineHostObject {
  explicit EngineHostObject(lepus::Value&& value) : value(std::move(value)) {}

  lepus::Value value;
};

std::mutex& ExternRefRegistryMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::unordered_set<EngineHostObject*>& ExternRefRegistry() {
  static auto* registry = new std::unordered_set<EngineHostObject*>();
  return *registry;
}

void RegisterExternRefObject(EngineHostObject* object) {
  std::lock_guard<std::mutex> lock(ExternRefRegistryMutex());
  ExternRefRegistry().insert(object);
}

void UnregisterExternRefObject(EngineHostObject* object) {
  std::lock_guard<std::mutex> lock(ExternRefRegistryMutex());
  ExternRefRegistry().erase(object);
}

bool IsRegisteredExternRefObject(EngineHostObject* object) {
  std::lock_guard<std::mutex> lock(ExternRefRegistryMutex());
  return ExternRefRegistry().find(object) != ExternRefRegistry().end();
}

void DeleteEngineHostObject(void* object) {
  auto* host_object = static_cast<EngineHostObject*>(object);
  UnregisterExternRefObject(host_object);
  delete host_object;
}

void SetException(wasm_exec_env_t exec_env, const char* exception) {
  wasm_runtime_set_exception(wasm_runtime_get_module_inst(exec_env), exception);
}

void SetException(wasm_exec_env_t exec_env, const std::string& exception) {
  SetException(exec_env, exception.c_str());
}

std::string ExceptionPrefix(const char* function_name) {
  return std::string(function_name) + ": ";
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

lepus::Value ReadUtf8String(wasm_exec_env_t exec_env, int32_t app_offset,
                            int32_t byte_length, const char* function_name,
                            bool* ok) {
  if (!ValidateAppMemory(exec_env, app_offset, byte_length, function_name,
                         "string")) {
    *ok = false;
    return lepus::Value();
  }
  if (byte_length == 0) {
    return lepus::Value(std::string());
  }
  auto* data = static_cast<const char*>(AppAddrToNative(exec_env, app_offset));
  return lepus::Value(std::string(data, static_cast<size_t>(byte_length)));
}

int32_t RequiredStringLength(const std::string& value) {
  if (value.size() >
      static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    return std::numeric_limits<int32_t>::max();
  }
  return static_cast<int32_t>(value.size());
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
  int32_t required_length = value.size() >
                                    static_cast<size_t>(
                                        std::numeric_limits<int32_t>::max())
                                ? std::numeric_limits<int32_t>::max()
                                : static_cast<int32_t>(value.size());
  if (required_length > out_max_length) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " output string buffer is too small");
    *ok = false;
    return required_length;
  }
  if (!ValidateAppMemory(exec_env, out_offset, out_max_length, function_name,
                         "output string")) {
    *ok = false;
    return required_length;
  }
  if (required_length == 0) {
    return required_length;
  }
  auto* out_data = static_cast<char*>(AppAddrToNative(exec_env, out_offset));
  std::memcpy(out_data, value.data(), static_cast<size_t>(required_length));
  return required_length;
}

uintptr_t ToExternRef(wasm_exec_env_t exec_env, lepus::Value&& value,
                      const char* function_name) {
  if (value.IsEmpty()) {
    return kNullExternRef;
  }

  auto host_object = std::make_unique<EngineHostObject>(std::move(value));
  auto* module_inst = wasm_runtime_get_module_inst(exec_env);
  uint32_t externref_index = 0;
  if (!wasm_externref_obj2ref(module_inst, host_object.get(),
                              &externref_index)) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " failed to create externref");
    return kInvalidExternRef;
  }

  if (!wasm_externref_set_cleanup(module_inst, host_object.get(),
                                  DeleteEngineHostObject)) {
    wasm_externref_objdel(module_inst, host_object.get());
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " failed to set externref cleanup");
    return kInvalidExternRef;
  }

  RegisterExternRefObject(host_object.get());
  host_object.release();
  return externref_index;
}

lepus::Value FromExternRef(wasm_exec_env_t exec_env, uintptr_t externref,
                           const char* function_name, bool* ok) {
  if (externref == kNullExternRef) {
    return lepus::Value();
  }
  if (externref == kInvalidExternRef) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " externref is invalid");
    *ok = false;
    return lepus::Value();
  }

  void* extern_object = nullptr;
  if (!wasm_externref_ref2obj(static_cast<uint32_t>(externref),
                              &extern_object)) {
    SetException(exec_env, ExceptionPrefix(function_name) +
                               " failed to resolve externref");
    *ok = false;
    return lepus::Value();
  }

  auto* host_object = static_cast<EngineHostObject*>(extern_object);
  if (!IsRegisteredExternRefObject(host_object)) {
    return lepus::Value(extern_object);
  }
  return host_object->value;
}

class RawArgReader {
 public:
  explicit RawArgReader(uint64_t* raw_args) : current_(raw_args) {}

  int32_t ReadI32() { return static_cast<int32_t>(*current_++); }

  int64_t ReadI64() { return static_cast<int64_t>(*current_++); }

  double ReadF64() {
    uint64_t bits = *current_++;
    double value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  uintptr_t ReadExternRef() { return static_cast<uintptr_t>(*current_++); }

 private:
  uint64_t* current_;
};

lepus::Value ReadAnyValue(wasm_exec_env_t exec_env, RawArgReader* reader,
                          const char* function_name, bool* ok) {
  auto kind = static_cast<WasmHostValueKind>(reader->ReadI32());
  double number_payload = reader->ReadF64();
  int32_t string_offset = reader->ReadI32();
  int32_t string_length = reader->ReadI32();
  uintptr_t externref = reader->ReadExternRef();

  switch (kind) {
    case WasmHostValueKind::kUndefined:
      return lepus::Value(lepus::Value::kCreateAsUndefinedTag);
    case WasmHostValueKind::kNull:
      return lepus::Value();
    case WasmHostValueKind::kBool:
      return lepus::Value(number_payload != 0);
    case WasmHostValueKind::kNumber:
      return lepus::Value(number_payload);
    case WasmHostValueKind::kString:
      return ReadUtf8String(exec_env, string_offset, string_length,
                            function_name, ok);
    case WasmHostValueKind::kExternRef:
      return FromExternRef(exec_env, externref, function_name, ok);
  }

  SetException(exec_env, ExceptionPrefix(function_name) +
                             " unknown dynamic value kind");
  *ok = false;
  return lepus::Value();
}

lepus::Value ReadArgument(wasm_exec_env_t exec_env, RawArgReader* reader,
                          WasmArgKind kind, const char* function_name,
                          bool* ok) {
  switch (kind) {
    case WasmArgKind::kI32:
      return lepus::Value(reader->ReadI32());
    case WasmArgKind::kI64:
      return lepus::Value(reader->ReadI64());
    case WasmArgKind::kF64:
      return lepus::Value(reader->ReadF64());
    case WasmArgKind::kBool:
      return lepus::Value(reader->ReadI32() != 0);
    case WasmArgKind::kString: {
      int32_t string_offset = reader->ReadI32();
      int32_t string_length = reader->ReadI32();
      return ReadUtf8String(exec_env, string_offset, string_length,
                            function_name, ok);
    }
    case WasmArgKind::kExternRef:
      return FromExternRef(exec_env, reader->ReadExternRef(), function_name, ok);
    case WasmArgKind::kAny:
      return ReadAnyValue(exec_env, reader, function_name, ok);
  }

  SetException(exec_env, ExceptionPrefix(function_name) +
                             " unknown argument ABI kind");
  *ok = false;
  return lepus::Value();
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
  if (value.IsEmpty()) {
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

bool WriteAnyValueDescriptor(wasm_exec_env_t exec_env, int32_t out_offset,
                             WasmHostValueOut* out_value,
                             const char* function_name) {
  if (!ValidateAppMemory(exec_env, out_offset,
                         static_cast<int32_t>(sizeof(WasmHostValueOut)),
                         function_name, "output value")) {
    return false;
  }
  auto* value_out = AppAddrToNative(exec_env, out_offset);
  std::memcpy(value_out, out_value, sizeof(WasmHostValueOut));
  return true;
}

uintptr_t WriteAnyReturn(wasm_exec_env_t exec_env, RawArgReader* reader,
                         lepus::Value&& result, const char* function_name,
                         bool* ok) {
  int32_t out_value_offset = reader->ReadI32();
  int32_t out_string_offset = reader->ReadI32();
  int32_t out_string_max_length = reader->ReadI32();

  WasmHostValueOut value_out = {};
  if (result.IsUndefined()) {
    value_out.kind = static_cast<int32_t>(WasmHostValueKind::kUndefined);
    *ok = WriteAnyValueDescriptor(exec_env, out_value_offset, &value_out,
                                  function_name);
    return kNullExternRef;
  }
  if (result.IsNil()) {
    value_out.kind = static_cast<int32_t>(WasmHostValueKind::kNull);
    *ok = WriteAnyValueDescriptor(exec_env, out_value_offset, &value_out,
                                  function_name);
    return kNullExternRef;
  }
  if (result.IsBool()) {
    value_out.kind = static_cast<int32_t>(WasmHostValueKind::kBool);
    value_out.bool_value = result.Bool() ? 1 : 0;
    *ok = WriteAnyValueDescriptor(exec_env, out_value_offset, &value_out,
                                  function_name);
    return kNullExternRef;
  }
  if (result.IsNumber()) {
    value_out.kind = static_cast<int32_t>(WasmHostValueKind::kNumber);
    value_out.number_value = result.Number();
    *ok = WriteAnyValueDescriptor(exec_env, out_value_offset, &value_out,
                                  function_name);
    return kNullExternRef;
  }
  if (result.IsString()) {
    value_out.kind = static_cast<int32_t>(WasmHostValueKind::kString);
    const std::string& string_value = result.StdString();
    value_out.string_required_length = RequiredStringLength(string_value);
    int32_t written_length =
        WriteUtf8String(exec_env, string_value, out_string_offset,
                        out_string_max_length, function_name, ok);
    value_out.string_written_length = written_length >= 0 ? written_length : 0;
    if (!WriteAnyValueDescriptor(exec_env, out_value_offset, &value_out,
                                 function_name)) {
      *ok = false;
    }
    return kNullExternRef;
  }

  value_out.kind = static_cast<int32_t>(WasmHostValueKind::kExternRef);
  if (!WriteAnyValueDescriptor(exec_env, out_value_offset, &value_out,
                               function_name)) {
    *ok = false;
    return kInvalidExternRef;
  }
  return ToExternRef(exec_env, std::move(result), function_name);
}

void WriteReturnValue(wasm_exec_env_t exec_env, uint64_t* raw_args,
                      RawArgReader* reader, WasmReturnKind kind,
                      lepus::Value&& result, const char* function_name,
                      bool* ok) {
  switch (kind) {
    case WasmReturnKind::kVoid:
      return;
    case WasmReturnKind::kI32: {
      int32_t value = 0;
      if (!ValueAsI32(result, function_name, exec_env, &value)) {
        *ok = false;
        return;
      }
      raw_args[0] = static_cast<uint32_t>(value);
      return;
    }
    case WasmReturnKind::kI64: {
      int64_t value = 0;
      if (!ValueAsI64(result, function_name, exec_env, &value)) {
        *ok = false;
        return;
      }
      raw_args[0] = static_cast<uint64_t>(value);
      return;
    }
    case WasmReturnKind::kBool:
      raw_args[0] = result.Bool() ? 1 : 0;
      return;
    case WasmReturnKind::kString: {
      int32_t out_offset = reader->ReadI32();
      int32_t out_max_length = reader->ReadI32();
      if (result.IsEmpty()) {
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
      return;
    }
    case WasmReturnKind::kExternRef:
      raw_args[0] = ToExternRef(exec_env, std::move(result), function_name);
      return;
    case WasmReturnKind::kAny:
      raw_args[0] = WriteAnyReturn(exec_env, reader, std::move(result),
                                   function_name, ok);
      return;
  }

  SetException(exec_env, ExceptionPrefix(function_name) +
                             " unknown return ABI kind");
  *ok = false;
}

void EngineHostFunction(wasm_exec_env_t exec_env, uint64_t* raw_args) {
  auto* descriptor = static_cast<const BindingDescriptor*>(
      wasm_runtime_get_function_attachment(exec_env));
  if (descriptor == nullptr) {
    SetException(exec_env, "WAMR host function missing binding descriptor");
    return;
  }

  auto* context = GetEngineHostContext(exec_env);
  if (context == nullptr) {
    SetException(exec_env, ExceptionPrefix(descriptor->name) +
                               " missing Lynx runtime context");
    return;
  }

  bool ok = true;
  RawArgReader reader(raw_args);
  std::vector<lepus::Value> args;
  args.reserve(descriptor->argc);
  for (size_t i = 0; i < descriptor->argc; ++i) {
    args.emplace_back(
        ReadArgument(exec_env, &reader, descriptor->args[i], descriptor->name,
                     &ok));
    if (!ok) {
      return;
    }
  }

  switch (descriptor->hidden_component_id) {
    case HiddenComponentId::kNone:
      break;
    case HiddenComponentId::kPrependNumberZero:
      args.insert(args.begin(), lepus::Value(0.0));
      break;
    case HiddenComponentId::kPrependStringZero:
      args.insert(args.begin(), lepus::Value(std::string("0")));
      break;
    case HiddenComponentId::kInsertNumberZeroAfterFirstArg:
      args.insert(args.begin() + 1, lepus::Value(0.0));
      break;
  }

  lepus::Value result =
      descriptor->function(context, args.data(), static_cast<int>(args.size()));
  WriteReturnValue(exec_env, raw_args, &reader, descriptor->return_kind,
                   std::move(result), descriptor->name, &ok);
}

#define ARG_LIST(name, ...) \
  constexpr WasmArgKind name[] = {__VA_ARGS__}

#define BINDING(id, symbol, function, return_kind, args)                    \
  constexpr BindingDescriptor id = {symbol,                                \
                                    &tasm::RendererFunctions::function,     \
                                    args, std::size(args), return_kind,     \
                                    HiddenComponentId::kNone}

#define BINDING_WITH_COMPONENT_ID(id, symbol, function, return_kind, args, \
                                  hidden_component_id)                    \
  constexpr BindingDescriptor id = {symbol,                               \
                                    &tasm::RendererFunctions::function,    \
                                    args, std::size(args), return_kind,    \
                                    hidden_component_id}

#define BINDING0(id, symbol, function, return_kind)               \
  constexpr BindingDescriptor id = {symbol,                       \
                                    &tasm::RendererFunctions::function, \
                                    nullptr, 0, return_kind,      \
                                    HiddenComponentId::kNone}

#define BINDING0_WITH_COMPONENT_ID(id, symbol, function, return_kind, \
                                   hidden_component_id)              \
  constexpr BindingDescriptor id = {symbol,                          \
                                    &tasm::RendererFunctions::function, \
                                    nullptr, 0, return_kind,         \
                                    hidden_component_id}

ARG_LIST(kCreateElementArgs, WasmArgKind::kString, WasmArgKind::kAny);
BINDING_WITH_COMPONENT_ID(
    kCreateElementBinding, tasm::kCFunctionCreateElement, FiberCreateElement,
    WasmReturnKind::kExternRef, kCreateElementArgs,
    HiddenComponentId::kInsertNumberZeroAfterFirstArg);
ARG_LIST(kCreatePageArgs, WasmArgKind::kI32, WasmArgKind::kAny);
BINDING_WITH_COMPONENT_ID(kCreatePageBinding, tasm::kCFunctionCreatePage,
                          FiberCreatePage, WasmReturnKind::kExternRef,
                          kCreatePageArgs,
                          HiddenComponentId::kPrependStringZero);
ARG_LIST(kCreateParentWithInfoArgs, WasmArgKind::kAny);
BINDING_WITH_COMPONENT_ID(kCreateViewBinding, tasm::kCFunctionCreateView,
                          FiberCreateView, WasmReturnKind::kExternRef,
                          kCreateParentWithInfoArgs,
                          HiddenComponentId::kPrependNumberZero);
ARG_LIST(kCreateListArgs, WasmArgKind::kExternRef, WasmArgKind::kExternRef,
         WasmArgKind::kAny, WasmArgKind::kExternRef);
BINDING_WITH_COMPONENT_ID(kCreateListBinding, tasm::kCFunctionCreateList,
                          FiberCreateList, WasmReturnKind::kExternRef,
                          kCreateListArgs,
                          HiddenComponentId::kPrependNumberZero);
BINDING_WITH_COMPONENT_ID(
    kCreateScrollViewBinding, tasm::kCFunctionCreateScrollView,
    FiberCreateScrollView, WasmReturnKind::kExternRef,
    kCreateParentWithInfoArgs, HiddenComponentId::kPrependNumberZero);
BINDING_WITH_COMPONENT_ID(kCreateTextBinding, tasm::kCFunctionCreateText,
                          FiberCreateText, WasmReturnKind::kExternRef,
                          kCreateParentWithInfoArgs,
                          HiddenComponentId::kPrependNumberZero);
BINDING_WITH_COMPONENT_ID(kCreateImageBinding, tasm::kCFunctionCreateImage,
                          FiberCreateImage, WasmReturnKind::kExternRef,
                          kCreateParentWithInfoArgs,
                          HiddenComponentId::kPrependNumberZero);
ARG_LIST(kCreateRawTextArgs, WasmArgKind::kString, WasmArgKind::kAny);
BINDING(kCreateRawTextBinding, tasm::kCFunctionCreateRawText,
        FiberCreateRawText, WasmReturnKind::kExternRef, kCreateRawTextArgs);
BINDING0_WITH_COMPONENT_ID(kCreateNonElementBinding,
                           tasm::kCFunctionCreateNonElement,
                           FiberCreateNonElement, WasmReturnKind::kExternRef,
                           HiddenComponentId::kPrependNumberZero);
BINDING0_WITH_COMPONENT_ID(kCreateWrapperElementBinding,
                           tasm::kCFunctionCreateWrapperElement,
                           FiberCreateWrapperElement,
                           WasmReturnKind::kExternRef,
                           HiddenComponentId::kPrependNumberZero);

ARG_LIST(kTwoRefsArgs, WasmArgKind::kExternRef, WasmArgKind::kExternRef);
BINDING(kAppendElementBinding, tasm::kCFunctionAppendElement,
        FiberAppendElement, WasmReturnKind::kExternRef, kTwoRefsArgs);
BINDING(kRemoveElementBinding, tasm::kCFunctionRemoveElement,
        FiberRemoveElement, WasmReturnKind::kExternRef, kTwoRefsArgs);
ARG_LIST(kInsertElementBeforeArgs, WasmArgKind::kExternRef,
         WasmArgKind::kExternRef, WasmArgKind::kAny);
BINDING(kInsertElementBeforeBinding, tasm::kCFunctionInsertElementBefore,
        FiberInsertElementBefore, WasmReturnKind::kExternRef,
        kInsertElementBeforeArgs);
ARG_LIST(kOneRefArgs, WasmArgKind::kExternRef);
BINDING(kFirstElementBinding, tasm::kCFunctionFirstElement, FiberFirstElement,
        WasmReturnKind::kExternRef, kOneRefArgs);
BINDING(kLastElementBinding, tasm::kCFunctionLastElement, FiberLastElement,
        WasmReturnKind::kExternRef, kOneRefArgs);
BINDING(kNextElementBinding, tasm::kCFunctionNextElement, FiberNextElement,
        WasmReturnKind::kExternRef, kOneRefArgs);
BINDING(kReplaceElementBinding, tasm::kCFunctionReplaceElement,
        FiberReplaceElement, WasmReturnKind::kVoid, kTwoRefsArgs);
BINDING(kSwapElementBinding, tasm::kCFunctionSwapElement, FiberSwapElement,
        WasmReturnKind::kVoid, kTwoRefsArgs);
BINDING(kGetParentBinding, tasm::kCFunctionGetParent, FiberGetParent,
        WasmReturnKind::kExternRef, kOneRefArgs);
BINDING(kGetChildrenBinding, tasm::kCFunctionGetChildren, FiberGetChildren,
        WasmReturnKind::kExternRef, kOneRefArgs);
ARG_LIST(kCloneElementArgs, WasmArgKind::kExternRef, WasmArgKind::kAny);
BINDING(kCloneElementBinding, tasm::kCFunctionCloneElement, FiberCloneElement,
        WasmReturnKind::kExternRef, kCloneElementArgs);
BINDING(kElementIsEqualBinding, tasm::kCFunctionElementIsEqual,
        FiberElementIsEqual, WasmReturnKind::kBool, kTwoRefsArgs);
BINDING(kGetElementUniqueIDBinding, tasm::kCFunctionGetElementUniqueID,
        FiberGetElementUniqueID, WasmReturnKind::kI64, kOneRefArgs);
BINDING(kGetTagBinding, tasm::kCFunctionGetTag, FiberGetTag,
        WasmReturnKind::kString, kOneRefArgs);

ARG_LIST(kSetAttributeArgs, WasmArgKind::kExternRef, WasmArgKind::kAny,
         WasmArgKind::kAny);
BINDING(kSetAttributeBinding, tasm::kCFunctionSetAttribute,
        FiberSetAttribute, WasmReturnKind::kVoid, kSetAttributeArgs);
BINDING(kGetAttributesBinding, tasm::kCFunctionGetAttributes,
        FiberGetAttributes, WasmReturnKind::kExternRef, kOneRefArgs);
ARG_LIST(kRefStringArgs, WasmArgKind::kExternRef, WasmArgKind::kString);
ARG_LIST(kRefAnyArgs, WasmArgKind::kExternRef, WasmArgKind::kAny);
BINDING(kAddClassBinding, tasm::kCFunctionAddClass, FiberAddClass,
        WasmReturnKind::kVoid, kRefStringArgs);
BINDING(kSetClassesBinding, tasm::kCFunctionSetClasses, FiberSetClasses,
        WasmReturnKind::kVoid, kRefStringArgs);
BINDING(kGetClassesBinding, tasm::kCFunctionGetClasses, FiberGetClasses,
        WasmReturnKind::kExternRef, kOneRefArgs);
ARG_LIST(kAddInlineStyleArgs, WasmArgKind::kExternRef, WasmArgKind::kAny,
         WasmArgKind::kAny);
BINDING(kAddInlineStyleBinding, tasm::kCFunctionAddInlineStyle,
        FiberAddInlineStyle, WasmReturnKind::kVoid, kAddInlineStyleArgs);
BINDING(kSetInlineStylesBinding, tasm::kCFunctionSetInlineStyles,
        FiberSetInlineStyles, WasmReturnKind::kVoid, kRefAnyArgs);
BINDING(kGetInlineStylesBinding, tasm::kCFunctionGetInlineStyles,
        FiberGetInlineStyles, WasmReturnKind::kString, kOneRefArgs);
ARG_LIST(kSetParsedStylesArgs, WasmArgKind::kExternRef, WasmArgKind::kString,
         WasmArgKind::kAny);
BINDING(kSetParsedStylesBinding, tasm::kCFunctionSetParsedStyles,
        FiberSetParsedStyles, WasmReturnKind::kVoid, kSetParsedStylesArgs);
BINDING(kGetComputedStylesBinding, tasm::kCFunctionGetComputedStyles,
        FiberGetComputedStyles, WasmReturnKind::kAny, kOneRefArgs);
ARG_LIST(kAddEventArgs, WasmArgKind::kExternRef, WasmArgKind::kString,
         WasmArgKind::kString, WasmArgKind::kAny);
BINDING(kAddEventBinding, tasm::kCFunctionAddEvent, FiberAddEvent,
        WasmReturnKind::kVoid, kAddEventArgs);
ARG_LIST(kRefRefArgs, WasmArgKind::kExternRef, WasmArgKind::kExternRef);
BINDING(kSetEventsBinding, tasm::kCFunctionSetEvents, FiberSetEvents,
        WasmReturnKind::kVoid, kRefAnyArgs);
ARG_LIST(kGetEventArgs, WasmArgKind::kExternRef, WasmArgKind::kString,
         WasmArgKind::kString);
BINDING(kGetEventBinding, tasm::kCFunctionGetEvent, FiberGetEvent,
        WasmReturnKind::kExternRef, kGetEventArgs);
BINDING(kGetEventsBinding, tasm::kCFunctionGetEvents, FiberGetEvents,
        WasmReturnKind::kExternRef, kOneRefArgs);
BINDING(kSetIDBinding, tasm::kCFunctionSetID, FiberSetID,
        WasmReturnKind::kVoid, kRefAnyArgs);
BINDING(kGetIDBinding, tasm::kCFunctionGetID, FiberGetID,
        WasmReturnKind::kString, kOneRefArgs);
ARG_LIST(kAddDatasetArgs, WasmArgKind::kExternRef, WasmArgKind::kString,
         WasmArgKind::kAny);
BINDING(kAddDatasetBinding, tasm::kCFunctionAddDataset, FiberAddDataset,
        WasmReturnKind::kVoid, kAddDatasetArgs);
BINDING(kSetDatasetBinding, tasm::kCFunctionSetDataset, FiberSetDataset,
        WasmReturnKind::kVoid, kRefAnyArgs);
BINDING(kGetDatasetBinding, tasm::kCFunctionGetDataset, FiberGetDataset,
        WasmReturnKind::kExternRef, kOneRefArgs);
ARG_LIST(kTwoAnyArgs, WasmArgKind::kAny, WasmArgKind::kAny);
BINDING(kFlushElementTreeBinding, tasm::kCFunctionFlushElementTree,
        FiberFlushElementTree, WasmReturnKind::kVoid, kTwoAnyArgs);
ARG_LIST(kReportErrorArgs, WasmArgKind::kAny, WasmArgKind::kAny);
BINDING(kReportErrorBinding, tasm::kCFunctionReportError, ReportError,
        WasmReturnKind::kVoid, kReportErrorArgs);
BINDING(kGetDataByKeyBinding, tasm::kCFunctionGetDataByKey,
        FiberGetDataByKey, WasmReturnKind::kAny, kRefStringArgs);
ARG_LIST(kReplaceElementsArgs, WasmArgKind::kExternRef, WasmArgKind::kAny,
         WasmArgKind::kAny);
BINDING(kReplaceElementsBinding, tasm::kCFunctionReplaceElements,
        FiberReplaceElements, WasmReturnKind::kVoid, kReplaceElementsArgs);
ARG_LIST(kQuerySelectorArgs, WasmArgKind::kExternRef, WasmArgKind::kString,
         WasmArgKind::kAny);
BINDING(kQuerySelectorBinding, tasm::kCFunctionQuerySelector,
        FiberQuerySelector, WasmReturnKind::kExternRef, kQuerySelectorArgs);
BINDING(kQuerySelectorAllBinding, tasm::kCFunctionQuerySelectorAll,
        FiberQuerySelectorAll, WasmReturnKind::kExternRef, kQuerySelectorArgs);
BINDING(kAddConfigBinding, tasm::kCFunctionAddConfig, FiberAddConfig,
        WasmReturnKind::kVoid, kAddDatasetArgs);
BINDING(kSetConfigBinding, tasm::kCFunctionSetConfig, FiberSetConfig,
        WasmReturnKind::kVoid, kRefAnyArgs);
BINDING(kGetConfigBinding, tasm::kCFunctionGetConfig, FiberGetElementConfig,
        WasmReturnKind::kExternRef, kOneRefArgs);
ARG_LIST(kRefI32Args, WasmArgKind::kExternRef, WasmArgKind::kI32);
BINDING(kGetInlineStyleBinding, tasm::kCFunctionGetInlineStyle,
        FiberGetInlineStyle, WasmReturnKind::kAny, kRefI32Args);
BINDING(kGetAttributeByNameBinding, tasm::kCFunctionGetAttributeByName,
        FiberGetAttributeByName, WasmReturnKind::kAny, kRefStringArgs);
BINDING(kGetAttributeNamesBinding, tasm::kCFunctionGetAttributeNames,
        FiberGetAttributeNames, WasmReturnKind::kExternRef, kOneRefArgs);
BINDING0(kGetPageElementBinding, tasm::kCFunctionGetPageElement,
         FiberGetPageElement, WasmReturnKind::kExternRef);
ARG_LIST(kUniqueIDArgs, WasmArgKind::kI64);
BINDING(kGetElementByUniqueIDBinding, tasm::kCFunctionGetElementByUniqueID,
        FiberGetElementByUniqueID, WasmReturnKind::kExternRef, kUniqueIDArgs);
ARG_LIST(kAddEventListenerArgs, WasmArgKind::kExternRef, WasmArgKind::kString,
         WasmArgKind::kExternRef, WasmArgKind::kExternRef);
BINDING(kAddEventListenerBinding, tasm::kCFunctionAddEventListener,
        FiberAddEventListener, WasmReturnKind::kVoid, kAddEventListenerArgs);
BINDING(kRemoveEventListenerBinding, tasm::kCFunctionFiberRemoveEventListener,
        FiberRemoveEventListener, WasmReturnKind::kVoid, kAddEventListenerArgs);
ARG_LIST(kCreateEventArgs, WasmArgKind::kI32, WasmArgKind::kString,
         WasmArgKind::kExternRef, WasmArgKind::kExternRef);
BINDING(kCreateEventBinding, tasm::kCFunctionCreateEvent, FiberCreateEvent,
        WasmReturnKind::kExternRef, kCreateEventArgs);
BINDING(kDispatchEventBinding, tasm::kCFunctionDispatchEvent,
        FiberDispatchEvent, WasmReturnKind::kBool, kTwoRefsArgs);
BINDING(kStopPropagationBinding, tasm::kCFunctionStopPropagation,
        FiberStopPropagation, WasmReturnKind::kVoid, kOneRefArgs);
BINDING(kStopImmediatePropagationBinding,
        tasm::kCFunctionStopImmediatePropagation, FiberStopImmediatePropagation,
        WasmReturnKind::kVoid, kOneRefArgs);
ARG_LIST(kInvokeUIMethodArgs, WasmArgKind::kExternRef, WasmArgKind::kString,
         WasmArgKind::kExternRef, WasmArgKind::kExternRef);
BINDING(kInvokeUIMethodBinding, tasm::kCFunctionInvokeUIMethod,
        InvokeUIMethod, WasmReturnKind::kVoid, kInvokeUIMethodArgs);
BINDING(kGetComputedStyleByKeyBinding,
        tasm::kCFunctionGetComputedStyleByKey, FiberGetComputedStyleByKey,
        WasmReturnKind::kAny, kRefStringArgs);
ARG_LIST(kTimerArgs, WasmArgKind::kExternRef, WasmArgKind::kI64);
BINDING(kSetTimeoutBinding, tasm::kSetTimeout, SetTimeout,
        WasmReturnKind::kI64, kTimerArgs);
ARG_LIST(kTimerIDArgs, WasmArgKind::kI64);
BINDING(kClearTimeoutBinding, tasm::kClearTimeout, ClearTimeout,
        WasmReturnKind::kVoid, kTimerIDArgs);
BINDING(kSetIntervalBinding, tasm::kSetInterval, SetInterval,
        WasmReturnKind::kI64, kTimerArgs);
BINDING(kClearIntervalBinding, tasm::kClearTimeInterval, ClearTimeInterval,
        WasmReturnKind::kVoid, kTimerIDArgs);
BINDING(kAdoptStyleSheetBinding, tasm::kCFuncAdoptStyleSheet, AdoptStyleSheet,
        WasmReturnKind::kVoid, kOneRefArgs);
BINDING0(kReplaceStyleSheetsBinding, tasm::kCFuncReplaceStyleSheets,
         ReplaceStyleSheets, WasmReturnKind::kVoid);

#undef BINDING0_WITH_COMPONENT_ID
#undef BINDING0
#undef BINDING_WITH_COMPONENT_ID
#undef BINDING
#undef ARG_LIST

#define WASM_I32 "i"
#define WASM_I64 "I"
#define WASM_BOOL "i"
#define WASM_STRING "ii"
#define WASM_REF "r"
#define WASM_ANY "iFiir"
#define WASM_STRING_OUT "ii"
#define WASM_ANY_OUT "iii"
#define SIG(args, result) "(" args ")" result

#define SYMBOL(binding, signature)                                      \
  { binding.name, reinterpret_cast<void*>(EngineHostFunction), signature, \
    const_cast<BindingDescriptor*>(&binding) }

NativeSymbol g_engine_host_symbols[] = {
    SYMBOL(kCreateElementBinding, SIG(WASM_STRING WASM_ANY, WASM_REF)),
    SYMBOL(kCreatePageBinding, SIG(WASM_I32 WASM_ANY, WASM_REF)),
    SYMBOL(kCreateViewBinding, SIG(WASM_ANY, WASM_REF)),
    SYMBOL(kCreateListBinding,
           SIG(WASM_REF WASM_REF WASM_ANY WASM_REF, WASM_REF)),
    SYMBOL(kCreateScrollViewBinding, SIG(WASM_ANY, WASM_REF)),
    SYMBOL(kCreateTextBinding, SIG(WASM_ANY, WASM_REF)),
    SYMBOL(kCreateImageBinding, SIG(WASM_ANY, WASM_REF)),
    SYMBOL(kCreateRawTextBinding, SIG(WASM_STRING WASM_ANY, WASM_REF)),
    SYMBOL(kCreateNonElementBinding, SIG("", WASM_REF)),
    SYMBOL(kCreateWrapperElementBinding, SIG("", WASM_REF)),
    SYMBOL(kAppendElementBinding, SIG(WASM_REF WASM_REF, WASM_REF)),
    SYMBOL(kRemoveElementBinding, SIG(WASM_REF WASM_REF, WASM_REF)),
    SYMBOL(kInsertElementBeforeBinding,
           SIG(WASM_REF WASM_REF WASM_ANY, WASM_REF)),
    SYMBOL(kFirstElementBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kLastElementBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kNextElementBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kReplaceElementBinding, SIG(WASM_REF WASM_REF, "")),
    SYMBOL(kSwapElementBinding, SIG(WASM_REF WASM_REF, "")),
    SYMBOL(kGetParentBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kGetChildrenBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kCloneElementBinding, SIG(WASM_REF WASM_ANY, WASM_REF)),
    SYMBOL(kElementIsEqualBinding, SIG(WASM_REF WASM_REF, WASM_BOOL)),
    SYMBOL(kGetElementUniqueIDBinding, SIG(WASM_REF, WASM_I64)),
    SYMBOL(kGetTagBinding, SIG(WASM_REF WASM_STRING_OUT, WASM_I32)),
    SYMBOL(kSetAttributeBinding, SIG(WASM_REF WASM_ANY WASM_ANY, "")),
    SYMBOL(kGetAttributesBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kAddClassBinding, SIG(WASM_REF WASM_STRING, "")),
    SYMBOL(kSetClassesBinding, SIG(WASM_REF WASM_STRING, "")),
    SYMBOL(kGetClassesBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kAddInlineStyleBinding, SIG(WASM_REF WASM_ANY WASM_ANY, "")),
    SYMBOL(kSetInlineStylesBinding, SIG(WASM_REF WASM_ANY, "")),
    SYMBOL(kGetInlineStylesBinding,
           SIG(WASM_REF WASM_STRING_OUT, WASM_I32)),
    SYMBOL(kSetParsedStylesBinding, SIG(WASM_REF WASM_STRING WASM_ANY, "")),
    SYMBOL(kGetComputedStylesBinding,
           SIG(WASM_REF WASM_ANY_OUT, WASM_REF)),
    SYMBOL(kAddEventBinding,
           SIG(WASM_REF WASM_STRING WASM_STRING WASM_ANY, "")),
    SYMBOL(kSetEventsBinding, SIG(WASM_REF WASM_ANY, "")),
    SYMBOL(kGetEventBinding, SIG(WASM_REF WASM_STRING WASM_STRING, WASM_REF)),
    SYMBOL(kGetEventsBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kSetIDBinding, SIG(WASM_REF WASM_ANY, "")),
    SYMBOL(kGetIDBinding, SIG(WASM_REF WASM_STRING_OUT, WASM_I32)),
    SYMBOL(kAddDatasetBinding, SIG(WASM_REF WASM_STRING WASM_ANY, "")),
    SYMBOL(kSetDatasetBinding, SIG(WASM_REF WASM_ANY, "")),
    SYMBOL(kGetDatasetBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kFlushElementTreeBinding, SIG(WASM_ANY WASM_ANY, "")),
    SYMBOL(kReportErrorBinding, SIG(WASM_ANY WASM_ANY, "")),
    SYMBOL(kGetDataByKeyBinding, SIG(WASM_REF WASM_STRING WASM_ANY_OUT,
                                     WASM_REF)),
    SYMBOL(kReplaceElementsBinding, SIG(WASM_REF WASM_ANY WASM_ANY, "")),
    SYMBOL(kQuerySelectorBinding, SIG(WASM_REF WASM_STRING WASM_ANY, WASM_REF)),
    SYMBOL(kQuerySelectorAllBinding,
           SIG(WASM_REF WASM_STRING WASM_ANY, WASM_REF)),
    SYMBOL(kAddConfigBinding, SIG(WASM_REF WASM_STRING WASM_ANY, "")),
    SYMBOL(kSetConfigBinding, SIG(WASM_REF WASM_ANY, "")),
    SYMBOL(kGetConfigBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kGetInlineStyleBinding, SIG(WASM_REF WASM_I32 WASM_ANY_OUT,
                                       WASM_REF)),
    SYMBOL(kGetAttributeByNameBinding,
           SIG(WASM_REF WASM_STRING WASM_ANY_OUT, WASM_REF)),
    SYMBOL(kGetAttributeNamesBinding, SIG(WASM_REF, WASM_REF)),
    SYMBOL(kGetPageElementBinding, SIG("", WASM_REF)),
    SYMBOL(kGetElementByUniqueIDBinding, SIG(WASM_I64, WASM_REF)),
    SYMBOL(kAddEventListenerBinding,
           SIG(WASM_REF WASM_STRING WASM_REF WASM_REF, "")),
    SYMBOL(kRemoveEventListenerBinding,
           SIG(WASM_REF WASM_STRING WASM_REF WASM_REF, "")),
    SYMBOL(kCreateEventBinding,
           SIG(WASM_I32 WASM_STRING WASM_REF WASM_REF, WASM_REF)),
    SYMBOL(kDispatchEventBinding, SIG(WASM_REF WASM_REF, WASM_BOOL)),
    SYMBOL(kStopPropagationBinding, SIG(WASM_REF, "")),
    SYMBOL(kStopImmediatePropagationBinding, SIG(WASM_REF, "")),
    SYMBOL(kInvokeUIMethodBinding,
           SIG(WASM_REF WASM_STRING WASM_REF WASM_REF, "")),
    SYMBOL(kGetComputedStyleByKeyBinding,
           SIG(WASM_REF WASM_STRING WASM_ANY_OUT, WASM_REF)),
    SYMBOL(kSetTimeoutBinding, SIG(WASM_REF WASM_I64, WASM_I64)),
    SYMBOL(kClearTimeoutBinding, SIG(WASM_I64, "")),
    SYMBOL(kSetIntervalBinding, SIG(WASM_REF WASM_I64, WASM_I64)),
    SYMBOL(kClearIntervalBinding, SIG(WASM_I64, "")),
    SYMBOL(kAdoptStyleSheetBinding, SIG(WASM_REF, "")),
    SYMBOL(kReplaceStyleSheetsBinding, SIG("", "")),
};

#undef SYMBOL
#undef SIG
#undef WASM_ANY_OUT
#undef WASM_STRING_OUT
#undef WASM_ANY
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
