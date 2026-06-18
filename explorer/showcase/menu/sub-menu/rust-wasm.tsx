// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import { root } from '@lynx-js/react';
import { AppContextProvider } from '@explorer/lib';
import { ItemProps } from '@components/menu-item';
import { Menu } from '@components/menu';

const ITEMS: ItemProps[] = [
  {
    title: 'React Lynx Yew',
    description: 'A Yew example compiled to wasm32-wasip1 for WAMR',
    url: 'file://lynx?local://showcase/rust-wasm/react.wasm',
  },
  {
    title: 'ColorfulView Yew',
    description: 'Nested colorful view benchmark compiled from Rust/Yew',
    url: 'file://lynx?local://showcase/rust-wasm/colorful-view.wasm',
  },
];

root.render(
  <AppContextProvider>
    <Menu items={ITEMS} />
  </AppContextProvider>
);
