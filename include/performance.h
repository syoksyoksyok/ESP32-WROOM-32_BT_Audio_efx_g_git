// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ESP32 Performance Profiling & Monitoring Utilities
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//
// 使用方法:
//   1. platformio.ini の test 環境でビルド: pio run -e test
//   2. 性能測定が有効化されます（PROFILE_ENABLED定義時）
//   3. シリアルモニターで統計情報を確認: pio device monitor
//
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

#ifndef PERFORMANCE_H
#define PERFORMANCE_H

#include <Arduino.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"

// ================================================================
// プロファイリング制御
// ================================================================
#ifdef PROFILE_ENABLED

// 統計情報の出力間隔（ミリ秒）
#define PROFILE_REPORT_INTERVAL_MS 5000

// ================================================================
// パフォーマンスカウンター
// ================================================================
struct PerformanceCounters {
    // 関数実行時間（マイクロ秒）
    uint32_t processAudioSample_us;
    uint32_t renderGrain_us;
    uint32_t renderAllGrains_us;
    uint32_t updateDisplay_us;

    // 実行回数
    uint32_t processAudioSample_count;
    uint32_t renderGrain_count;
    uint32_t grainTrigger_count;

    // 累積時間（オーバーフロー対策）
    uint64_t total_audio_processing_us;
    uint64_t total_render_time_us;

    // CPU使用率（0-100%）
    float cpu_usage_core0;
    float cpu_usage_core1;

    // メモリ使用量
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t free_psram;

    // オーディオバッファ状態
    uint32_t audio_buffer_underruns;
    uint32_t active_grain_max;

    // 最大実行時間（マイクロ秒）
    uint32_t max_processAudioSample_us;
    uint32_t max_renderGrain_us;
};

// グローバルカウンター
extern PerformanceCounters g_perf;

// ================================================================
// 高精度タイマーマクロ
// ================================================================
#define PROFILE_START(name) \
    uint64_t __profile_start_##name = esp_timer_get_time()

#define PROFILE_END(name) \
    do { \
        uint64_t __profile_end_##name = esp_timer_get_time(); \
        uint32_t __profile_duration_##name = (uint32_t)(__profile_end_##name - __profile_start_##name); \
        g_perf.name##_us = __profile_duration_##name; \
        if (__profile_duration_##name > g_perf.max_##name##_us) { \
            g_perf.max_##name##_us = __profile_duration_##name; \
        } \
    } while(0)

// ================================================================
// CPU使用率測定
// ================================================================
inline void updateCpuUsage() {
    static uint32_t lastIdleTime0 = 0, lastIdleTime1 = 0;
    static uint32_t lastTotalTime = 0;

    uint32_t idleTime0 = xTaskGetIdleTaskCountForCore(0);
    uint32_t idleTime1 = xTaskGetIdleTaskCountForCore(1);
    uint32_t totalTime = millis();

    if (lastTotalTime > 0) {
        uint32_t deltaTime = totalTime - lastTotalTime;
        uint32_t deltaIdle0 = idleTime0 - lastIdleTime0;
        uint32_t deltaIdle1 = idleTime1 - lastIdleTime1;

        // CPU使用率 = 100% - (アイドル時間の割合)
        g_perf.cpu_usage_core0 = 100.0f - (deltaIdle0 * 100.0f / deltaTime);
        g_perf.cpu_usage_core1 = 100.0f - (deltaIdle1 * 100.0f / deltaTime);
    }

    lastIdleTime0 = idleTime0;
    lastIdleTime1 = idleTime1;
    lastTotalTime = totalTime;
}

// ================================================================
// メモリ使用量測定
// ================================================================
inline void updateMemoryStats() {
    g_perf.free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    g_perf.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    g_perf.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

// ================================================================
// 統計情報のレポート出力
// ================================================================
inline void printPerformanceReport() {
    static unsigned long lastReportTime = 0;
    unsigned long currentTime = millis();

    if (currentTime - lastReportTime < PROFILE_REPORT_INTERVAL_MS) {
        return;
    }
    lastReportTime = currentTime;

    // CPU使用率とメモリを更新
    updateCpuUsage();
    updateMemoryStats();

    Serial.println(F("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
    Serial.println(F("PERFORMANCE REPORT"));
    Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));

    // CPU使用率
    Serial.printf("CPU Usage: Core 0: %.1f%% | Core 1: %.1f%%\n",
                  g_perf.cpu_usage_core0, g_perf.cpu_usage_core1);

    // メモリ使用量
    Serial.printf("Memory: Free Heap: %u bytes | Min Free: %u bytes | Free PSRAM: %u bytes\n",
                  g_perf.free_heap, g_perf.min_free_heap, g_perf.free_psram);

    // 関数実行時間（平均）
    Serial.println(F("\n[Function Execution Times]"));
    Serial.printf("  processAudioSample: %u μs (max: %u μs)\n",
                  g_perf.processAudioSample_us, g_perf.max_processAudioSample_us);
    Serial.printf("  renderGrain: %u μs (max: %u μs)\n",
                  g_perf.renderGrain_us, g_perf.max_renderGrain_us);
    Serial.printf("  renderAllGrains: %u μs\n", g_perf.renderAllGrains_us);
    Serial.printf("  updateDisplay: %u μs\n", g_perf.updateDisplay_us);

    // 実行回数
    Serial.println(F("\n[Call Counts]"));
    Serial.printf("  Audio samples processed: %u\n", g_perf.processAudioSample_count);
    Serial.printf("  Grains rendered: %u\n", g_perf.renderGrain_count);
    Serial.printf("  Grains triggered: %u\n", g_perf.grainTrigger_count);

    // オーディオバッファ状態
    Serial.println(F("\n[Audio Buffer Status]"));
    Serial.printf("  Buffer underruns: %u\n", g_perf.audio_buffer_underruns);
    Serial.printf("  Max active grains: %u\n", g_perf.active_grain_max);

    // レイテンシ推定
    float estimated_latency_ms = (g_perf.processAudioSample_us / 1000.0f);
    Serial.printf("\n[Estimated Latency] %.2f ms per sample\n", estimated_latency_ms);

    Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"));
}

// ================================================================
// カウンターリセット
// ================================================================
inline void resetPerformanceCounters() {
    memset(&g_perf, 0, sizeof(PerformanceCounters));
}

// ================================================================
// プロファイリング無効時のダミーマクロ
// ================================================================
#else

// プロファイリング無効時はすべてノーオペレーション
#define PROFILE_START(name) do {} while(0)
#define PROFILE_END(name) do {} while(0)
inline void printPerformanceReport() {}
inline void resetPerformanceCounters() {}
inline void updateCpuUsage() {}
inline void updateMemoryStats() {}

#endif // PROFILE_ENABLED

#endif // PERFORMANCE_H
