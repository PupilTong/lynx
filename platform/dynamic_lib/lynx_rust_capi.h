// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef PLATFORM_DYNAMIC_LIB_LYNX_RUST_CAPI_H_
#define PLATFORM_DYNAMIC_LIB_LYNX_RUST_CAPI_H_

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(LYNX_RUST_CAPI_IMPLEMENTATION)
#define LYNX_RUST_CAPI_EXPORT __declspec(dllexport)
#else
#define LYNX_RUST_CAPI_EXPORT __declspec(dllimport)
#endif
#else
#define LYNX_RUST_CAPI_EXPORT __attribute__((visibility("default")))
#endif

typedef struct lynx_view_builder_t lynx_view_builder_t;
typedef struct lynx_view_t lynx_view_t;

LYNX_RUST_CAPI_EXPORT void lynx_rust_view_builder_set_screen_size(
    lynx_view_builder_t* builder, float width, float height,
    float pixel_ratio);

LYNX_RUST_CAPI_EXPORT void lynx_rust_view_builder_set_frame(
    lynx_view_builder_t* builder, float x, float y, float width, float height);

LYNX_RUST_CAPI_EXPORT void lynx_rust_view_builder_set_font_scale(
    lynx_view_builder_t* builder, float scale);

LYNX_RUST_CAPI_EXPORT void lynx_rust_view_update_screen_metrics(
    lynx_view_t* view, float width, float height, float pixel_ratio);

LYNX_RUST_CAPI_EXPORT void lynx_rust_view_set_frame(lynx_view_t* view, float x,
                                                    float y, float width,
                                                    float height);

LYNX_RUST_CAPI_EXPORT void lynx_rust_view_set_font_scale(lynx_view_t* view,
                                                         float font_scale);

#ifdef __cplusplus
}
#endif

#endif  // PLATFORM_DYNAMIC_LIB_LYNX_RUST_CAPI_H_
