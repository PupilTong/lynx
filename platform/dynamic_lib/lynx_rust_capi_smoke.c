// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "platform/dynamic_lib/lynx_rust_capi.h"

void lynx_rust_capi_smoke(void) {
  lynx_rust_view_builder_set_screen_size((lynx_view_builder_t*)0, 0.0f, 0.0f,
                                         1.0f);
  lynx_rust_view_builder_set_frame((lynx_view_builder_t*)0, 0.0f, 0.0f, 0.0f,
                                   0.0f);
  lynx_rust_view_builder_set_font_scale((lynx_view_builder_t*)0, 1.0f);
  lynx_rust_view_update_screen_metrics((lynx_view_t*)0, 0.0f, 0.0f, 1.0f);
  lynx_rust_view_set_frame((lynx_view_t*)0, 0.0f, 0.0f, 0.0f, 0.0f);
  lynx_rust_view_set_font_scale((lynx_view_t*)0, 1.0f);
}
