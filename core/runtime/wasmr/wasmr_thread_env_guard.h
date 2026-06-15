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
    if (wasm_runtime_thread_env_inited()) {
      ok_ = true;
      return;
    }
    if (!wasm_runtime_init_thread_env()) {
      return;
    }
    owns_thread_env_ = true;
    ok_ = true;
  }

  WamrThreadEnvGuard(const WamrThreadEnvGuard&) = delete;
  WamrThreadEnvGuard& operator=(const WamrThreadEnvGuard&) = delete;

  ~WamrThreadEnvGuard() {
    if (owns_thread_env_) {
      wasm_runtime_destroy_thread_env();
    }
  }

  bool ok() const { return ok_; }

 private:
  bool owns_thread_env_ = false;
  bool ok_ = false;
};

}  // namespace wasmr
}  // namespace runtime
}  // namespace lynx

#endif  // CORE_RUNTIME_WASMR_WASMR_THREAD_ENV_GUARD_H_
