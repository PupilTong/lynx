// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import { root } from '@lynx-js/react';
import { AppContextProvider } from '@explorer/lib';
import { ItemProps } from '@components/menu-item';
import { Menu } from '@components/menu';

const PERF_QUERY = 'perf_overlay=true';

const ITEMS: ItemProps[] = [
  {
    title: 'ReactLynx3 React',
    description: 'Production ReactLynx3 bundle with inline image assets',
    url:
      'file://lynx?local://showcase/menu/reactexample.lynx.bundle?' +
      `${PERF_QUERY}&perf_label=ReactLynx3`,
  },
  {
    title: 'WAMR Yew',
    description: 'Rust/Yew wasm32-wasip1 bundle optimized for WAMR',
    url:
      'file://lynx?local://showcase/rust-wasm/react.wasm?' +
      `${PERF_QUERY}&perf_label=WAMR`,
  },
  {
    title: 'WAMR ColorfulView',
    description: 'Rust/Yew nested colorful view benchmark for WAMR',
    url:
      'file://lynx?local://showcase/rust-wasm/colorful-view.wasm?' +
      `${PERF_QUERY}&perf_label=WAMR_ColorfulView`,
  },
];

root.render(
  <AppContextProvider>
    <Menu items={ITEMS} />
  </AppContextProvider>
);
