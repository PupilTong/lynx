// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
package com.lynx.tasm.performance;

import com.lynx.react.bridge.JavaOnlyMap;
import com.lynx.tasm.TimingHandler;
import com.lynx.tasm.performance.timing.TimingConstants;
import java.util.HashMap;

public class TimingOption {
  public final String pipelineOrigin;
  public final HashMap<String, Long> timingInfo;
  private final HashMap<String, Long> initTimingInfo;
  private boolean mUseEmbeddedMode = false;

  /**
   * Creates a TimingOption instance with initial timing measurements.
   * This factory method initializes a TimingOption with the current system time
   * for both pipeline start and the specified start timing type.
   *
   * @param pipelineOrigin The origin identifier for the pipeline
   * @param startTimingType The specific timing type to mark as started
   * @return A new TimingOption instance with initial timing data
   */
  public static TimingOption createTimingOption(
      String pipelineOrigin, String startTimingType, boolean useEmbeddedMode) {
    TimingOption timingOption = new TimingOption(pipelineOrigin);
    timingOption.mUseEmbeddedMode = useEmbeddedMode;
    if (useEmbeddedMode) {
      return timingOption;
    }
    long currentTimeMillis = PerformanceController.currentSystemTimeMicroseconds();
    timingOption.setTiming(TimingConstants.PIPELINE_START, currentTimeMillis);
    timingOption.setTiming(startTimingType, currentTimeMillis);
    return timingOption;
  }

  private TimingOption(String pipelineOrigin) {
    this.pipelineOrigin = pipelineOrigin;
    this.timingInfo = new HashMap<>();
    this.initTimingInfo = new HashMap<>();
  }

  public void setTiming(String key, long usTimingStamp) {
    if (mUseEmbeddedMode) {
      return;
    }
    this.timingInfo.put(key, usTimingStamp);
  }

  public void markTiming(String key) {
    if (mUseEmbeddedMode) {
      return;
    }
    long currentTimeMillis = PerformanceController.currentSystemTimeMicroseconds();
    this.timingInfo.put(key, currentTimeMillis);
  }

  public void setExtraTiming(TimingHandler.ExtraTimingInfo extraTiming) {
    if (mUseEmbeddedMode || extraTiming == null) {
      return;
    }
    setInitTiming(TimingHandler.OPEN_TIME, extraTiming.mOpenTime);
    setInitTiming(TimingHandler.CONTAINER_INIT_START, extraTiming.mContainerInitStart);
    setInitTiming(TimingHandler.CONTAINER_INIT_END, extraTiming.mContainerInitEnd);
    setInitTiming(TimingHandler.PREPARE_TEMPLATE_START, extraTiming.mPrepareTemplateStart);
    setInitTiming(TimingHandler.PREPARE_TEMPLATE_END, extraTiming.mPrepareTemplateEnd);
  }

  public JavaOnlyMap toJavaOnlyMap() {
    JavaOnlyMap result = new JavaOnlyMap();
    if (mUseEmbeddedMode) {
      return result;
    }
    result.putString(TimingConstants.PIPELINE_ORIGIN, this.pipelineOrigin);
    result.putMap(TimingConstants.TIMESTAMP_MAP, JavaOnlyMap.from(this.timingInfo));
    result.putMap(TimingConstants.INIT_TIMESTAMP_MAP, JavaOnlyMap.from(this.initTimingInfo));
    return result;
  }

  private void setInitTiming(String key, long msTimestamp) {
    if (msTimestamp > 0) {
      this.initTimingInfo.put(key, msTimestamp * 1000);
    }
  }
}
