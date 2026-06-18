// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef CORE_RUNTIME_WASMR_WASMR_THREAD_ENV_GUARD_H_
#define CORE_RUNTIME_WASMR_WASMR_THREAD_ENV_GUARD_H_

#include "wasm_export.h"

namespace lynx {
namespace runtime {
namespace wasmr {

class WamrThreadEnvGuard {
 public:
  WamrThreadEnvGuard() {
    auto& depth = GuardDepth();
    if (depth > 0) {
      ++depth;
      ok_ = true;
      return;
    }

    // WAMR's wasm_runtime_thread_env_inited() does not check the POSIX signal
    // env when AOT is disabled, while the interpreter still requires it for
    // hardware bound checks. Initialize explicitly for each outer guard.
    if (!wasm_runtime_init_thread_env()) {
      return;
    }
    ++depth;
    outer_guard_ = true;
    ok_ = true;
  }

  WamrThreadEnvGuard(const WamrThreadEnvGuard&) = delete;
  WamrThreadEnvGuard& operator=(const WamrThreadEnvGuard&) = delete;

  ~WamrThreadEnvGuard() {
    if (!ok_) {
      return;
    }
    auto& depth = GuardDepth();
    if (depth > 0) {
      --depth;
    }
    if (outer_guard_ && depth == 0) {
      wasm_runtime_destroy_thread_env();
    }
  }

  bool ok() const { return ok_; }

 private:
  static int& GuardDepth() {
    static thread_local int depth = 0;
    return depth;
  }

  bool outer_guard_ = false;
  bool ok_ = false;
};

}  // namespace wasmr
}  // namespace runtime
}  // namespace lynx

#endif  // CORE_RUNTIME_WASMR_WASMR_THREAD_ENV_GUARD_H_
