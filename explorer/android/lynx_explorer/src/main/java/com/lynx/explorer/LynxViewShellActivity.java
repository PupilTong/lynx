// Copyright 2024 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
package com.lynx.explorer;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.Toolbar;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import com.lynx.explorer.input.LynxExplorerInput;
import com.lynx.explorer.modules.LynxSettingManager;
import com.lynx.explorer.provider.DemoGenericResourceFetcher;
import com.lynx.explorer.provider.DemoMediaResourceFetcher;
import com.lynx.explorer.provider.DemoTemplateResourceFetcher;
import com.lynx.explorer.utils.QueryMapUtils;
import com.lynx.tasm.LynxBooleanOption;
import com.lynx.tasm.LynxView;
import com.lynx.tasm.LynxViewBuilder;
import com.lynx.tasm.LynxViewClient;
import com.lynx.tasm.LynxViewClientV2;
import com.lynx.tasm.TemplateData;
import com.lynx.tasm.ThreadStrategyForRendering;
import com.lynx.tasm.TimingHandler;
import com.lynx.tasm.behavior.Behavior;
import com.lynx.tasm.behavior.LynxContext;
import com.lynx.tasm.performance.performanceobserver.LoadBundleEntry;
import com.lynx.tasm.performance.performanceobserver.PerformanceMetric;
import com.lynx.tasm.performance.performanceobserver.PerformanceEntry;
import com.lynx.tasm.performance.performanceobserver.PipelineEntry;
import com.lynx.tasm.utils.DisplayMetricsHolder;
import com.lynx.xelement.XElementBehaviors;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

public class LynxViewShellActivity extends AppCompatActivity {
  public static final String URL_KEY = "url";
  public static final String PREFERENCES = "ExplorerStorage";
  private static final String URL_PREFIX = "file://lynx?local://";
  private static final String TAG = "LynxViewShellActivity";
  private static final String HOME_PAGE_URL =
      "file://lynx?local://homepage.lynx.bundle?fullscreen=true&orientation=portrait";
  private static final String DEFAULT_TOP_BAR_COLOR = "#F0F2F5";
  private static final String DEFAULT_TOP_BAR_TITLE_COLOR = "#000000";
  private static final String DEFAULT_TOP_BAR_BACK_BUTTON_STYLE = "light";
  private static final String PERF_OVERLAY_PARAM = "perf_overlay";
  private static final String PERF_LABEL_PARAM = "perf_label";
  private ViewGroup mLynxContainer;
  private LynxView mLynxView;
  private String mFrontendTheme;
  private TextView mPerfOverlay;
  private boolean mPerfOverlayEnabled;
  private String mPerfLabel = "Lynx";
  private String mPendingPerfText;
  private String mPerfTimingText;
  private boolean mHasPerformanceEntryTiming;
  private TimingHandler.ExtraTimingInfo extraTimingInfo = new TimingHandler.ExtraTimingInfo();

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    extraTimingInfo.mOpenTime = System.currentTimeMillis();
    extraTimingInfo.mContainerInitStart = System.currentTimeMillis();

    Intent intent = getIntent();

    // Check for initial URL from intent extra / data, used by automation.
    String initialUrl = intent.getStringExtra("lynx_initial_url");
    if (initialUrl == null || initialUrl.isEmpty()) {
      initialUrl = intent.getStringExtra(URL_KEY);
    }
    if ((initialUrl == null || initialUrl.isEmpty()) && intent.getData() != null) {
      initialUrl = intent.getData().getQueryParameter(URL_KEY);
    }

    final String url = (initialUrl != null && !initialUrl.isEmpty()) ? initialUrl : HOME_PAGE_URL;
    if (initialUrl != null && !initialUrl.isEmpty()) {
      Log.d(TAG, "Opening initial URL: " + initialUrl);
    }

    setTopBarAppearance(url);
    mLynxContainer = findViewById(R.id.lynx_container);

    extraTimingInfo.mContainerInitEnd = System.currentTimeMillis();

    openTargetUrl(url);
  }

  @Override
  protected void onDestroy() {
    if (mLynxView != null) {
      mLynxView.destroy();
    }
    super.onDestroy();
  }

  @Override
  public boolean onOptionsItemSelected(MenuItem item) {
    if (item.getItemId() == android.R.id.home) {
      finish();
      return true;
    }
    return super.onOptionsItemSelected(item);
  }

  private String getStorageItem(String key) {
    SharedPreferences p = this.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
    String value = p.getString(key, null);
    return value;
  }

  private void setTopBarAppearance(String url) {
    if (isAssetFilename(url)) {
      url = getAssetFilename(url);
    }

    QueryMapUtils queryMap = new QueryMapUtils();
    queryMap.parse(url);
    boolean isFullscreen = queryMap.getBoolean("fullscreen", false);

    if (!isFullscreen) {
      setContentView(R.layout.default_display);

      Toolbar toolbar = findViewById(R.id.toolbar);

      setWindowColor(toolbar, queryMap);
      setBackButtonStyle(toolbar, queryMap);
      setActionBarTitle(toolbar, queryMap);

      setSupportActionBar(toolbar);
      getSupportActionBar().setDisplayShowTitleEnabled(false);
      getSupportActionBar().setDisplayHomeAsUpEnabled(true);
    } else {
      setContentView(R.layout.fullscreen_display);
      setStatusBarAppearance();
    }
  }

  private void setWindowColor(Toolbar toolbar, QueryMapUtils queryMap) {
    String color = queryMap.contains("bar_color") ? "#" + queryMap.getString("bar_color")
                                                  : DEFAULT_TOP_BAR_COLOR;

    toolbar.setBackgroundColor(Color.parseColor(color));
    getWindow().setStatusBarColor(Color.parseColor(color));

    // set background color as the action bar color for better visual experience
    getWindow().getDecorView().setBackgroundColor(Color.parseColor(color));
  }

  private void setBackButtonStyle(Toolbar toolbar, QueryMapUtils queryMap) {
    String backButtonStyle = queryMap.contains("back_button_style")
        ? queryMap.getString("back_button_style")
        : DEFAULT_TOP_BAR_BACK_BUTTON_STYLE;
    if (backButtonStyle.equals("dark")) {
      toolbar.setNavigationIcon(R.drawable.back_dark);

      mFrontendTheme = "dark";
    } else {
      toolbar.setNavigationIcon(R.drawable.back_light);

      mFrontendTheme = "light";
    }
  }

  private void setActionBarTitle(Toolbar toolbar, QueryMapUtils queryMap) {
    TextView tv = toolbar.findViewById(R.id.toolbar_title);
    String title = queryMap.contains("title") ? queryMap.getString("title") : null;
    String titleColor = queryMap.contains("title_color") ? "#" + queryMap.getString("title_color")
                                                         : DEFAULT_TOP_BAR_TITLE_COLOR;

    if (tv != null) {
      tv.setText(title);
      tv.setTextColor(Color.parseColor(titleColor));
    }
  }

  public boolean isNotchScreen() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
      return false;
    }

    WindowManager windowManager = getSystemService(WindowManager.class);
    if (windowManager == null) {
      return false;
    }

    Display display = windowManager.getDefaultDisplay();
    DisplayCutout cutout = display.getCutout();
    return cutout != null;
  }

  private void setStatusBarAppearance() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
      return;
    }

    if (isNotchScreen()) {
      WindowManager.LayoutParams params = getWindow().getAttributes();
      params.layoutInDisplayCutoutMode =
          WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
      getWindow().setAttributes(params);
      getWindow().getDecorView().setSystemUiVisibility(
          View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    WindowInsetsControllerCompat windowInsetsController =
        WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView());

    windowInsetsController.setSystemBarsBehavior(
        WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
  }

  private void openTargetUrl(String url) {
    if (url == null) {
      Log.i(TAG, "openTargetUrl failed: url is null.");
      return;
    }

    LynxViewBuilder builder = new LynxViewBuilder();
    builder.addBehaviors(new ImageBehavior().create());
    builder.addBehaviors(new XElementBehaviors().create());
    // for homepage only
    builder.addBehavior(new Behavior("explorer-input", false) {
      @Override
      public LynxExplorerInput createUI(LynxContext context) {
        return new LynxExplorerInput(context);
      }
    });
    builder.setEnableGenericResourceFetcher(LynxBooleanOption.TRUE);
    builder.setGenericResourceFetcher(new DemoGenericResourceFetcher());
    builder.setTemplateResourceFetcher(new DemoTemplateResourceFetcher(this));
    builder.setMediaResourceFetcher(new DemoMediaResourceFetcher());
    builder.setThreadStrategyForRendering(
        LynxSettingManager.getInstance().getSettingInfo().strategy == 0
            ? ThreadStrategyForRendering.ALL_ON_UI
            : ThreadStrategyForRendering.MOST_ON_TASM);
    // Parse the URL parameters and specify the LynxView width, height, and density according to the
    // parameters.
    QueryMapUtils queryMap = new QueryMapUtils();
    if (isAssetFilename(url)) {
      queryMap.parse(getAssetFilename(url));
    } else {
      queryMap.parse(url);
    }

    if (queryMap.contains("width") && queryMap.contains("height")) {
      builder.setPresetMeasuredSpec(
          View.MeasureSpec.makeMeasureSpec(queryMap.getInt("width", 720), View.MeasureSpec.EXACTLY),
          View.MeasureSpec.makeMeasureSpec(
              queryMap.getInt("height", 1280), View.MeasureSpec.EXACTLY));
    }
    if (queryMap.contains("density")) {
      builder.setDensity(queryMap.getFloat("density", 320) / 160.f);
    }

    if (queryMap.contains("orientation")) {
      switch (queryMap.getString("orientation")) {
        case "portrait":
          setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
          break;
        case "landscape":
          setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
          break;
        default:
          setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR);
          break;
      }
    }

    configurePerformanceOverlay(queryMap);
    LynxView lynxView = builder.build(this);
    installPerformanceClientIfNeeded(lynxView);
    lynxView.updateGlobalProps(getGlobalProps(this, queryMap));
    extraTimingInfo.mPrepareTemplateStart = System.currentTimeMillis();

    renderLynxViewWithUrl(lynxView, url);
    mLynxContainer.addView(lynxView,
        new FrameLayout.LayoutParams(queryMap.getInt("width", ViewGroup.LayoutParams.MATCH_PARENT),
            queryMap.getInt("height", ViewGroup.LayoutParams.MATCH_PARENT)));
    mLynxView = lynxView;
    ensurePerformanceOverlay();
  }

  private void configurePerformanceOverlay(QueryMapUtils queryMap) {
    mPerfOverlayEnabled = queryMap.getBoolean(PERF_OVERLAY_PARAM, false);
    if (queryMap.contains(PERF_LABEL_PARAM)) {
      mPerfLabel = queryMap.getString(PERF_LABEL_PARAM);
    }
    if (mPerfOverlayEnabled) {
      mPerfTimingText = "waiting for lynx_fcp...";
      mPendingPerfText = composePerformanceOverlayText();
    }
  }

  private void installPerformanceClientIfNeeded(LynxView lynxView) {
    if (!mPerfOverlayEnabled) {
      return;
    }
    lynxView.addLynxViewClient(new LynxViewClient() {
      @Override
      public void onTimingSetup(Map<String, Object> timingInfo) {
        if (!mHasPerformanceEntryTiming) {
          updatePerformanceTiming(formatSetupPerformance(timingInfo));
        }
      }
    });
    lynxView.addLynxViewClientV2(new LynxViewClientV2() {
      @Override
      public void onPerformanceEvent(@NonNull PerformanceEntry entry) {
        if (entry instanceof LoadBundleEntry) {
          mHasPerformanceEntryTiming = true;
          updatePerformanceTiming(formatLoadBundlePerformance((LoadBundleEntry) entry));
        } else if (!mHasPerformanceEntryTiming && entry instanceof PipelineEntry) {
          updatePerformanceTiming(formatPipelinePerformance(entry.name, (PipelineEntry) entry));
        }
      }
    });
  }

  private void ensurePerformanceOverlay() {
    if (!mPerfOverlayEnabled || mLynxContainer == null) {
      return;
    }
    if (mPerfOverlay != null) {
      mPerfOverlay.bringToFront();
      return;
    }
    TextView overlay = new TextView(this);
    overlay.setTextColor(Color.WHITE);
    overlay.setTextSize(11);
    overlay.setGravity(Gravity.START);
    overlay.setPadding(18, 12, 18, 12);
    overlay.setBackgroundColor(Color.argb(210, 0, 0, 0));
    overlay.setText(mPendingPerfText == null ? "Perf: " + mPerfLabel : mPendingPerfText);
    FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP);
    mLynxContainer.addView(overlay, params);
    mPerfOverlay = overlay;
  }

  private void updatePerformanceTiming(String text) {
    mPerfTimingText = text;
    String overlayText = composePerformanceOverlayText();
    Log.i(TAG, "PerfOverlay " + overlayText.replace('\n', '|'));
    updatePerformanceOverlay(overlayText);
  }

  private void updatePerformanceOverlay(String text) {
    mPendingPerfText = text;
    runOnUiThread(() -> {
      ensurePerformanceOverlay();
      if (mPerfOverlay != null) {
        mPerfOverlay.setText(mPendingPerfText);
      }
    });
  }

  private String composePerformanceOverlayText() {
    StringBuilder builder = new StringBuilder();
    builder.append("Perf: ").append(mPerfLabel).append('\n');
    if (mPerfTimingText != null) {
      builder.append(mPerfTimingText);
      if (mPerfTimingText.length() > 0
          && mPerfTimingText.charAt(mPerfTimingText.length() - 1) != '\n') {
        builder.append('\n');
      }
    }
    return builder.toString();
  }

  private String formatSetupPerformance(Map<String, Object> timingInfo) {
    Map<String, Object> setupTiming = getNestedMap(timingInfo, "setup_timing");
    Map<String, Object> metrics = getNestedMap(timingInfo, "metrics");
    StringBuilder builder = new StringBuilder();
    Double execute = duration(setupTiming, "lepus_excute_start", "lepus_excute_end");
    Double lynxFcp = firstNonNull(metric(metrics, "lynx_fcp"), metric(metrics, "lynxFcp"));
    Double loadTemplate = duration(setupTiming, "load_template_start", "load_template_end");
    Double afterLoadToFlush =
        signedDuration(setupTiming, "load_template_end", "ui_operation_flush_end");
    Double paintWait = subtract(subtract(lynxFcp, loadTemplate), afterLoadToFlush);
    appendLynxFcpHeader(builder, lynxFcp, loadTemplate, afterLoadToFlush, paintWait);
    builder.append("diagnostics\n");
    appendMetric(builder, 2, "render_cpu",
        firstNonNull(duration(setupTiming, "mtsRenderStart", "mtsRenderEnd"), execute));
    builder.append("  pipeline\n");
    appendMetric(builder, 4, "layout", duration(setupTiming, "layout_start", "layout_end"));
    appendMetric(builder, 4, "ui_flush", duration(setupTiming, "ui_operation_flush_start",
                     "ui_operation_flush_end"));
    return builder.toString();
  }

  private String formatLoadBundlePerformance(LoadBundleEntry entry) {
    StringBuilder builder = new StringBuilder();
    Double execute = duration(entry.rawMap, "vmExecuteStart", "vmExecuteEnd");
    Double lynxFcp = metricDuration(entry.lynxFcp);
    Double loadTemplate = duration(entry.loadBundleStart, entry.loadBundleEnd);
    Double afterLoadToFlush =
        signedDuration(entry.loadBundleEnd, entry.layoutUiOperationExecuteEnd);
    Double paintWait = duration(entry.layoutUiOperationExecuteEnd, entry.paintEnd);
    appendLynxFcpHeader(builder, lynxFcp, loadTemplate, afterLoadToFlush, paintWait);
    builder.append("diagnostics\n");
    appendMetric(builder, 2, "render_cpu",
        firstNonNull(duration(entry.mtsRenderStart, entry.mtsRenderEnd), execute));
    appendWamrMetrics(builder, entry.frameworkRenderingTiming);
    builder.append("  pipeline\n");
    appendMetric(builder, 4, "layout", duration(entry.layoutStart, entry.layoutEnd));
    appendMetric(builder, 4, "ui_flush",
        duration(entry.paintingUiOperationExecuteStart, entry.layoutUiOperationExecuteEnd));
    return builder.toString();
  }

  private String formatPipelinePerformance(String name, PipelineEntry entry) {
    StringBuilder builder = new StringBuilder();
    builder.append(name == null || name.isEmpty() ? "pipeline" : name).append('\n');
    appendPipelineMetrics(builder, entry);
    return builder.toString();
  }

  private void appendPipelineMetrics(StringBuilder builder, PipelineEntry entry) {
    appendPipelineMetrics(builder, entry, null);
  }

  private void appendPipelineMetrics(
      StringBuilder builder, PipelineEntry entry, Double renderCpuFallback) {
    appendMetric(builder, "render_cpu",
        firstNonNull(duration(entry.mtsRenderStart, entry.mtsRenderEnd), renderCpuFallback));
    appendMetric(builder, "layout", duration(entry.layoutStart, entry.layoutEnd));
    appendMetric(builder, "ui_flush",
        duration(entry.paintingUiOperationExecuteStart, entry.layoutUiOperationExecuteEnd));
    appendMetric(
        builder, "paint_wait", duration(entry.layoutUiOperationExecuteEnd, entry.paintEnd));
    appendWamrMetrics(builder, entry.frameworkRenderingTiming);
  }

  private void appendWamrMetrics(StringBuilder builder, Map<String, Object> timing) {
    if (!hasAnyKey(timing, "wamrLoadStart", "wamrEntryStart")) {
      return;
    }
    builder.append("  wamr\n");
    appendMetric(builder, 4, "load", duration(timing, "wamrLoadStart", "wamrLoadEnd"));
    appendMetric(builder, 4, "entry", duration(timing, "wamrEntryStart", "wamrEntryEnd"));
  }

  private boolean hasAnyKey(Map<String, Object> timing, String... keys) {
    for (String key : keys) {
      if (timing.containsKey(key)) {
        return true;
      }
    }
    return false;
  }

  private void appendLynxFcpHeader(StringBuilder builder, Double lynxFcp, Double loadTemplate,
      Double afterLoadToFlush, Double paintWait) {
    builder.append("lynx_fcp\n");
    appendMetric(builder, "lynx_fcp", lynxFcp);
    builder.append("  range: loadBundleStart -> paintEnd\n");
    builder.append("  components\n");
    appendMetric(builder, 4, "load_template", loadTemplate);
    appendMetric(builder, 4, "after_load_to_flush", afterLoadToFlush);
    appendMetric(builder, 4, "paint_wait", paintWait);
    appendMetric(builder, 4, "sum", sum(loadTemplate, afterLoadToFlush, paintWait));
  }

  private Map<String, Object> getNestedMap(Map<String, Object> map, String key) {
    if (map == null) {
      return new HashMap<>();
    }
    Object value = map.get(key);
    if (!(value instanceof Map)) {
      return new HashMap<>();
    }
    return (Map<String, Object>) value;
  }

  private Double duration(Map<String, Object> timing, String startKey, String endKey) {
    Double start = numberValue(timing.get(startKey));
    Double end = numberValue(timing.get(endKey));
    if (start == null || end == null || end < start) {
      return null;
    }
    return end - start;
  }

  private Double signedDuration(Map<String, Object> timing, String startKey, String endKey) {
    Double start = numberValue(timing.get(startKey));
    Double end = numberValue(timing.get(endKey));
    if (start == null || end == null) {
      return null;
    }
    return signedDuration(start, end);
  }

  private Double signedDuration(double start, double end) {
    if (start < 0 || end < 0) {
      return null;
    }
    return end - start;
  }

  private Double duration(double start, double end) {
    if (start < 0 || end < 0 || end < start) {
      return null;
    }
    return end - start;
  }

  private Double metric(Map<String, Object> metrics, String key) {
    return numberValue(metrics.get(key));
  }

  private Double metricDuration(PerformanceMetric metric) {
    if (metric == null || metric.duration < 0) {
      return null;
    }
    return metric.duration;
  }

  private Double firstNonNull(Double value, Double fallback) {
    return value != null ? value : fallback;
  }

  private Double numberValue(Object value) {
    if (value instanceof Number) {
      return ((Number) value).doubleValue();
    }
    return null;
  }

  private Double subtract(Double left, Double right) {
    if (left == null || right == null) {
      return null;
    }
    return left - right;
  }

  private Double sum(Double... values) {
    double total = 0;
    for (Double value : values) {
      if (value == null) {
        return null;
      }
      total += value;
    }
    return total;
  }

  private void appendMetric(StringBuilder builder, String name, Double value) {
    appendMetric(builder, 0, name, value);
  }

  private void appendMetric(StringBuilder builder, int indent, String name, Double value) {
    appendIndent(builder, indent);
    builder.append(name).append(": ");
    if (value == null) {
      builder.append("--");
    } else {
      builder.append(String.format(Locale.US, "%.2f ms", value));
    }
    builder.append('\n');
  }

  private void appendIndent(StringBuilder builder, int indent) {
    for (int i = 0; i < indent; ++i) {
      builder.append(' ');
    }
  }

  private void renderLynxViewWithUrl(LynxView lynxView, String url) {
    // Add a mock initData as example.
    Map<String, Object> initData = new HashMap<>();
    initData.put("mockData", "Hello Lynx Explorer");

    if (isAssetFilename(url)) {
      // get file from asset
      url = getAssetFilename(url);
      // parse url
      String[] strs = url.split("[?]");
      if (strs.length > 1) {
        url = strs[0];
      }
      strs = url.split("[&]");
      if (strs.length > 1) {
        url = strs[0];
      }
      byte[] templateBundleData = readFileFromAssets(this, url);
      extraTimingInfo.mPrepareTemplateEnd = System.currentTimeMillis();
      lynxView.setExtraTiming(extraTimingInfo);
      lynxView.renderTemplateWithBaseUrl(templateBundleData, initData, url);
    } else if (url.startsWith("https://") || url.startsWith("http://")) {
      extraTimingInfo.mPrepareTemplateEnd = System.currentTimeMillis();
      lynxView.setExtraTiming(extraTimingInfo);
      lynxView.renderTemplateUrl(url, initData);
    } else if (url.startsWith("assets://")) {
      url = url.substring("assets://".length());
      byte[] templateBundleData = readFileFromAssets(this, url);
      extraTimingInfo.mPrepareTemplateEnd = System.currentTimeMillis();
      lynxView.setExtraTiming(extraTimingInfo);
      lynxView.renderTemplateWithBaseUrl(templateBundleData, initData, url);
    } else {
      Log.i(TAG, "openTargetUrl failed: not supported url.");
    }
  }
  private TemplateData getGlobalProps(Context context, QueryMapUtils queryMap) {
    DisplayMetrics displayMetrics = DisplayMetricsHolder.getRealScreenDisplayMetrics(context);
    Map globalProps = new HashMap();
    globalProps.put("isNotchScreen", isNotchScreen());
    globalProps.put("screenWidth", displayMetrics.widthPixels / displayMetrics.density);
    globalProps.put("screenHeight", displayMetrics.heightPixels / displayMetrics.density);

    String theme = "Light";
    if ((context.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
        == Configuration.UI_MODE_NIGHT_YES) {
      theme = "Dark";
    }
    globalProps.put("theme", theme);

    String preferredTheme = getStorageItem("preferredTheme");
    globalProps.put("preferredTheme", preferredTheme);

    if (mFrontendTheme == "dark") {
      globalProps.put("frontendTheme", "dark");
    } else {
      globalProps.put("frontendTheme", "light");
    }

    queryMap.toMap().forEach((key, value) -> {
      int leadingUnderline = 0;
      while (leadingUnderline < key.length() && key.charAt(leadingUnderline) == '_') {
        leadingUnderline++;
      }
      key = key.substring(leadingUnderline);

      String[] parts = key.split("_");
      String propsKey = parts[0];
      for (int i = 1; i < parts.length; i++) {
        if (parts[i].isEmpty()) {
          continue;
        }
        propsKey += parts[i].substring(0, 1).toUpperCase() + parts[i].substring(1);
      }
      globalProps.put(propsKey, value);
    });

    return TemplateData.fromMap(globalProps);
  }

  public static boolean isAssetFilename(String url) {
    return url.startsWith(URL_PREFIX);
  }

  public static String getAssetFilename(String url) {
    return url.substring(URL_PREFIX.length());
  }

  public static byte[] readFileFromAssets(Context context, String fileName) {
    AssetManager assetManager = context.getAssets();
    ByteArrayOutputStream byteArrayOutputStream = null;
    InputStream inputStream = null;

    try {
      inputStream = assetManager.open(fileName);
      byteArrayOutputStream = new ByteArrayOutputStream();
      byte[] buffer = new byte[1024];
      int length;

      while ((length = inputStream.read(buffer)) != -1) {
        byteArrayOutputStream.write(buffer, 0, length);
      }

      return byteArrayOutputStream.toByteArray();
    } catch (IOException e) {
      e.printStackTrace();
      return null;
    } finally {
      try {
        if (inputStream != null) {
          inputStream.close();
        }
        if (byteArrayOutputStream != null) {
          byteArrayOutputStream.close();
        }
      } catch (IOException e) {
        e.printStackTrace();
      }
    }
  }
}
