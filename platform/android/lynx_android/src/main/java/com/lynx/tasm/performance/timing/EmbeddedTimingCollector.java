// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

package com.lynx.tasm.performance.timing;

import androidx.annotation.RestrictTo;
import com.lynx.react.bridge.JavaOnlyMap;
import com.lynx.tasm.TimingHandler;
import com.lynx.tasm.performance.IPerformanceObserver;
import com.lynx.tasm.performance.performanceobserver.PerformanceEntry;
import com.lynx.tasm.performance.performanceobserver.PerformanceEntryConverter;
import java.lang.ref.WeakReference;
import java.util.ArrayList;

/**
 * @brief Embedded timing collector that provides minimal timing tracking
 * for embedded mode, tracking essential load/update and optional extra timing
 * points.
 */
@RestrictTo(RestrictTo.Scope.LIBRARY)
public class EmbeddedTimingCollector {
  private long mLoadBundleStartUs;
  private long mOpenTimeUs;
  private long mContainerInitStartUs;
  private long mContainerInitEndUs;
  private long mPrepareTemplateStartUs;
  private long mPrepareTemplateEndUs;
  private final ArrayList<Long> mUpdateDataStartUsList = new ArrayList<>();
  private long mPaintEndUs;
  private boolean mHasEmitLoadBundleEvent = false;

  private WeakReference<IPerformanceObserver> mObserver;

  /**
   * Set timing observer for event callbacks
   */
  public void setObserver(WeakReference<IPerformanceObserver> observer) {
    mObserver = observer;
  }

  public boolean hasEmitLoadBundleEvent() {
    return mHasEmitLoadBundleEvent;
  }

  public boolean hasPendingUpdateEvent() {
    return !mUpdateDataStartUsList.isEmpty();
  }

  public void setExtraTiming(TimingHandler.ExtraTimingInfo extraTiming) {
    if (extraTiming == null) {
      return;
    }
    if (extraTiming.mOpenTime > 0) {
      mOpenTimeUs = millisToMicros(extraTiming.mOpenTime);
    }
    if (extraTiming.mContainerInitStart > 0) {
      mContainerInitStartUs = millisToMicros(extraTiming.mContainerInitStart);
    }
    if (extraTiming.mContainerInitEnd > 0) {
      mContainerInitEndUs = millisToMicros(extraTiming.mContainerInitEnd);
    }
    if (extraTiming.mPrepareTemplateStart > 0) {
      mPrepareTemplateStartUs = millisToMicros(extraTiming.mPrepareTemplateStart);
    }
    if (extraTiming.mPrepareTemplateEnd > 0) {
      mPrepareTemplateEndUs = millisToMicros(extraTiming.mPrepareTemplateEnd);
    }
  }

  public void markTiming(String key, long usTimestamp) {
    // Only track the essential timing points for embedded mode
    switch (key) {
      case TimingConstants.LOAD_BUNDLE_START:
        mLoadBundleStartUs = usTimestamp;
        break;
      case TimingConstants.UPDATE_DATA_START:
        mUpdateDataStartUsList.add(usTimestamp);
        break;
      case TimingConstants.PAINT_END:
        mPaintEndUs = usTimestamp;
        emitLoadBundleIfReady();
        emitUpdateDataIfReady();
        break;
      case TimingHandler.OPEN_TIME:
        mOpenTimeUs = usTimestamp;
        break;
      case TimingHandler.CONTAINER_INIT_START:
        mContainerInitStartUs = usTimestamp;
        break;
      case TimingHandler.CONTAINER_INIT_END:
        mContainerInitEndUs = usTimestamp;
        break;
      case TimingHandler.PREPARE_TEMPLATE_START:
        mPrepareTemplateStartUs = usTimestamp;
        break;
      case TimingHandler.PREPARE_TEMPLATE_END:
        mPrepareTemplateEndUs = usTimestamp;
        break;
      default:
        // Ignore other timing points in embedded mode
        break;
    }
  }

  /**
   * Emit timing event if all required data is available
   */
  private void emitLoadBundleIfReady() {
    if (mHasEmitLoadBundleEvent) {
      return;
    }
    if (mObserver == null) {
      return;
    }
    IPerformanceObserver observer = mObserver.get();
    if (observer == null) {
      return;
    }
    mHasEmitLoadBundleEvent = true;

    JavaOnlyMap entryMap = new JavaOnlyMap();
    entryMap.put("entryType", "pipeline");
    entryMap.put("name", TimingConstants.LOAD_BUNDLE);
    entryMap.put(TimingConstants.LOAD_BUNDLE_START, (double) mLoadBundleStartUs / 1000);
    entryMap.put(TimingConstants.PAINT_END, (double) mPaintEndUs / 1000);
    putTimestampIfPresent(entryMap, TimingHandler.OPEN_TIME, mOpenTimeUs);
    putTimestampIfPresent(entryMap, TimingHandler.CONTAINER_INIT_START, mContainerInitStartUs);
    putTimestampIfPresent(entryMap, TimingHandler.CONTAINER_INIT_END, mContainerInitEndUs);
    putTimestampIfPresent(entryMap, TimingHandler.PREPARE_TEMPLATE_START, mPrepareTemplateStartUs);
    putTimestampIfPresent(entryMap, TimingHandler.PREPARE_TEMPLATE_END, mPrepareTemplateEndUs);
    putMetricIfReady(entryMap, "lynxFcp", TimingConstants.LOAD_BUNDLE_START, mLoadBundleStartUs,
        TimingConstants.PAINT_END, mPaintEndUs);
    putMetricIfReady(entryMap, "fcp", TimingHandler.PREPARE_TEMPLATE_START,
        mPrepareTemplateStartUs, TimingConstants.PAINT_END, mPaintEndUs);
    putMetricIfReady(entryMap, "totalFcp", TimingHandler.OPEN_TIME, mOpenTimeUs,
        TimingConstants.PAINT_END, mPaintEndUs);

    PerformanceEntry entry = PerformanceEntryConverter.makePerformanceEntry(entryMap);
    observer.onPerformanceEvent(entry);
  }

  private void emitUpdateDataIfReady() {
    if (mObserver == null) {
      return;
    }
    IPerformanceObserver observer = mObserver.get();
    if (observer == null) {
      return;
    }

    ArrayList<Long> updateDataStartUsList = new ArrayList<>(mUpdateDataStartUsList);
    mUpdateDataStartUsList.clear();
    for (Long updateDataStartUs : updateDataStartUsList) {
      JavaOnlyMap entryMap = new JavaOnlyMap();
      entryMap.put("entryType", "pipeline");
      entryMap.put("name", TimingConstants.UPDATE_TRIGGERED_BY_NATIVE);
      entryMap.put(TimingConstants.PIPELINE_START, (double) updateDataStartUs / 1000);
      entryMap.put(TimingConstants.PAINT_END, (double) mPaintEndUs / 1000);

      PerformanceEntry entry = PerformanceEntryConverter.makePerformanceEntry(entryMap);
      observer.onPerformanceEvent(entry);
    }
  }

  private static long millisToMicros(long msTimestamp) {
    return msTimestamp * 1000;
  }

  private static void putTimestampIfPresent(JavaOnlyMap entryMap, String key, long usTimestamp) {
    if (usTimestamp > 0) {
      entryMap.put(key, (double) usTimestamp / 1000);
    }
  }

  private static void putMetricIfReady(JavaOnlyMap entryMap, String metricName, String startName,
      long startUs, String endName, long endUs) {
    if (startUs <= 0 || endUs <= 0 || endUs < startUs) {
      return;
    }
    JavaOnlyMap metricMap = new JavaOnlyMap();
    metricMap.put("name", metricName);
    metricMap.put("startTimestampName", startName);
    metricMap.put("startTimestamp", (double) startUs / 1000);
    metricMap.put("endTimestampName", endName);
    metricMap.put("endTimestamp", (double) endUs / 1000);
    metricMap.put("duration", (double) (endUs - startUs) / 1000);
    entryMap.put(metricName, metricMap);
  }
}
