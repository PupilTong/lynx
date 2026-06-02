// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
package com.lynx.tasm.utils;

import java.nio.ByteBuffer;

public final class WamrWasmBytecodeUtils {
  private WamrWasmBytecodeUtils() {}

  public static boolean hasWamrWasmBytecodeMagic(byte[] data) {
    return data != null && data.length >= 4 && data[0] == 0 && data[1] == 'a'
        && data[2] == 's' && data[3] == 'm';
  }

  public static boolean hasWamrWasmBytecodeMagic(ByteBuffer buffer) {
    return buffer != null && buffer.limit() >= 4 && buffer.get(0) == 0
        && buffer.get(1) == 'a' && buffer.get(2) == 's' && buffer.get(3) == 'm';
  }

  public static boolean hasWamrWasmBytecodeMagic(byte[] data, ByteBuffer buffer) {
    return hasWamrWasmBytecodeMagic(data) || hasWamrWasmBytecodeMagic(buffer);
  }
}
