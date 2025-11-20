/*
 * ESP32 A2DP Granular Effect - Snapshot Version v6 (Perf. Fix v2)
 * * 変更点:
 * 1. (v6以前) ... (省略) ...
 * 11. (v6) memsetによる表示キャッシュの初期化を廃止し、データ型を考慮した安全な初期化関数を実装。
 * これにより、Pitch表示が稀に "+4294" となるバグを修正。
 * 12. (Stability Fix v1) ADC読み取り処理(updateParametersFromPots)を、オーディオタスク(Core 1)から
 * メインループ(Core 0)に移動。これによりオーディオ処理の安定性を向上させ、グリッチのリスクを低減。
 * 13. (Perf. Fix v2) ピッチ計算のリアルタイム割り算を、事前計算した定数による掛け算に置き換え、
 * CPU負荷を軽減。
 */

// ================================================================= //
// SECTION: Headers & Libraries
// ================================================================= //
#include <Arduino.h>
#include <BluetoothA2DPSink.h>
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <TFT_eSPI.h>

// ================================================================= //
// SECTION: Pin Definitions
// ================================================================= //
constexpr int POT1_PIN = 36;
constexpr int POT2_PIN = 39;
constexpr int POT3_PIN = 34;
constexpr int POT4_PIN = 35;
constexpr int POT5_PIN = 32;
constexpr int POT6_PIN = 33;
constexpr int BUTTON_PIN = 25;
constexpr int POT4_BUTTON_PIN = 26;
constexpr int MODE_BUTTON_PIN = 27;
constexpr int TRIGGER_IN_PIN = 21;
constexpr int SNAPSHOT_1_BUTTON_PIN = 19;
constexpr int SNAPSHOT_2_BUTTON_PIN = 17;
constexpr int SNAPSHOT_3_BUTTON_PIN = 16;
constexpr int SNAPSHOT_4_BUTTON_PIN = 12; // 4番目のスナップショットボタン
constexpr int I2S_OUT_BCLK = 14;
constexpr int I2S_OUT_LRC = 15;
constexpr int I2S_OUT_DOUT = 13;
constexpr int BPM_LED_PIN = 2;
// ================================================================= //
// SECTION: System & Timing Constants
// ================================================================= //
constexpr unsigned long ADC_UPDATE_INTERVAL_MS = 55;
constexpr unsigned long DISPLAY_UPDATE_INTERVAL_MS = 33;  // 30fps (reduce flicker)
constexpr unsigned long BUTTON_LONG_PRESS_MS = 800;
constexpr unsigned long BUTTON_DEBOUNCE_MS = 15;
constexpr unsigned long RANDOMIZE_FLASH_DURATION_MS = 200;
constexpr unsigned long TAP_TEMPO_TIMEOUT_US = 2000000;
constexpr unsigned long BPM_LED_PULSE_DURATION_MS = 20;
// ← この値を短くする（例: 20ミリ秒）

// ================================================================= //
// SECTION: ADC & Parameter Constants
// ================================================================= //
constexpr float ADC_MAX_VALUE = 4095.0f;
constexpr int ADC_CHANGE_THRESHOLD = 40;
constexpr int ADC_SMOOTHING_SAMPLES = 32;
constexpr float PITCH_RANGE_SEMITONES = 48.0f;
constexpr float PITCH_CHANGE_THRESHOLD = 0.05f;
// ★★★ 変更点：ピッチ計算の割り算を事前に計算しておくための定数 ★★★
constexpr float PITCH_RANGE_SEMITONES_HALF = PITCH_RANGE_SEMITONES / 2.0f;
constexpr float PITCH_LUT_SCALE = (float)(257 - 1) / PITCH_RANGE_SEMITONES; // PITCH_LUT_SIZE is 257

// Pitch randomization range
constexpr float PITCH_RANDOM_MIN = -20.0f;
constexpr float PITCH_RANDOM_MAX = 7.0f;
constexpr float PITCH_RANDOM_RANGE = PITCH_RANDOM_MAX - PITCH_RANDOM_MIN; // 27.0f

// Grain calculation constants
constexpr float POSITION_TEXTURE_SCALE = 0.6f;  // 3/5 ratio for position randomization
constexpr float PITCH_TEXTURE_VARIANCE = 0.2f;   // Pitch variation from texture
constexpr float STEREO_SPREAD_SCALE = 0.5f;      // Stereo spread scaling factor
constexpr int16_t MIN_SIZE_Q15 = 3277;           // Minimum grain size in Q15 (~10% of range)

// Soft takeover parameters
constexpr float SOFT_TAKEOVER_DEADBAND = 0.03f;  // 3% tolerance for soft takeover

// Tempo validation bounds
constexpr unsigned long MIN_TEMPO_INTERVAL_US = 10000;    // ~6000 BPM maximum
constexpr unsigned long MAX_TEMPO_INTERVAL_US = 4000000;  // ~15 BPM minimum

// Feedback LUT range
constexpr float FEEDBACK_LUT_MIN = 0.1f;
constexpr float FEEDBACK_LUT_RANGE = 0.5f;  // Range from 0.1 to 0.6

// Pan center value (Q15 format)
constexpr int16_t PAN_CENTER_Q15 = 23170;  // ~0.707 in Q15 format for center panning

// ================================================================= //
// SECTION: Audio Engine Constants
// ================================================================= //
constexpr int RING_BUFFER_SIZE = 4096;
#define GRAIN_BUFFER_SIZE 32768   // 64KB (WROOM-32 has no PSRAM)
#define MAX_GRAIN_SIZE    32768   // Max ~0.74 seconds (limited by buffer)
#define GRAIN_BUFFER_MASK (GRAIN_BUFFER_SIZE - 1)
constexpr int MAX_GRAINS = 10;  // Increased from 6 for richer visuals
constexpr int MIN_GRAIN_SIZE = 512;  // Min ~11.6ms (was 128)
constexpr int FEEDBACK_BUFFER_SIZE = 512;
constexpr int I2S_BUFFER_SAMPLES = 128;
constexpr int DEJA_VU_BUFFER_SIZE = 16;
// ================================================================= //
// SECTION: UI Constants
// ================================================================= //
constexpr int UI_COL1_LABEL_X = 10;
constexpr int UI_COL1_BAR_X = 43;
constexpr int UI_COL2_LABEL_X = 185;
constexpr int UI_COL2_BAR_X = 213;
constexpr int UI_PARAM_Y_START = 5;
constexpr int UI_PARAM_Y_SPACING = 12;
constexpr int UI_BAR_WIDTH = 70;
constexpr int UI_BAR_HEIGHT = 8;
#define TFT_SKYBLUE 0x5D9B
#define TFT_AQUA 0x07FF
#define TFT_LIGHTBLUE 0xAFDF
#define GET_VISUALIZER_BG_COLOR() TFT_BLACK  // Black background
constexpr int UI_TRIGGER_LED_X = 310;
constexpr int UI_TRIGGER_LED_Y = 10;
constexpr int UI_TRIGGER_LED_RADIUS = 4;
constexpr unsigned long UI_TRIGGER_LED_DURATION_MS = 50;

// Particle Visualizer Constants
constexpr int VIZ_AREA_Y_START = 95;   // BPM/Grain display: below parameter area
constexpr int VIZ_INFO_HEIGHT = 15;
constexpr int VIZ_SEPARATOR_LINE_Y = 112;  // Separator line directly below BPM display
constexpr int VIZ_PARTICLE_Y_START = 115;  // Particle area: expanded upward (was 145)
constexpr int VIZ_PARTICLE_HEIGHT = 240 - VIZ_PARTICLE_Y_START - 28;  // Height: 97px (was 67px)
constexpr int VIZ_BUFFER_BAR_AREA_Y = VIZ_PARTICLE_Y_START + VIZ_PARTICLE_HEIGHT + 2;  // y=214
constexpr int VIZ_BUFFER_BAR_HEIGHT = 6;  // Half height (was 12)
constexpr int VIZ_BUFFER_BAR_WIDTH = 320;  // Full width (restored)
constexpr int VIZ_BUFFER_BAR_X_OFFSET = 0;  // Left aligned
constexpr int VIZ_PARTICLE_MAX_SIZE = 20;  // 2.5x larger (was 8)
constexpr int VIZ_PARTICLE_MIN_SIZE = 5;   // 2.5x larger (was 2)
// ================================================================= //
// SECTION: Look-Up Table (LUT) Sizes
// ================================================================= //
constexpr int WINDOW_LUT_SIZE = 128;
constexpr int PITCH_LUT_SIZE = 257;
constexpr int PAN_LUT_SIZE = 257;
constexpr int MIX_LUT_SIZE = 256;
constexpr int FEEDBACK_LUT_SIZE = 256;
constexpr int RECIPROCAL_LUT_SIZE = 256;
constexpr int RANDOM_PAN_LUT_SIZE = 128;
constexpr int RANDOM_LUT_SIZE = 256;
// ================================================================= //
// SECTION: Type Definitions & Enums
// ================================================================= //
enum PlayMode : uint8_t { MODE_GRANULAR = 0, MODE_REVERSE = 1 };
enum Pot4Mode : uint8_t {
    MODE_TEXTURE = 0,
    MODE_SPREAD = 1,
    MODE_FEEDBACK = 2,
    MODE_LOOP_LENGTH = 3,
    MODE_CLK_RESOLUTION = 4,
    POT4_MODE_COUNT = 5
};
struct FullParamSnapshot {
    int16_t position_q15;
    int16_t size_q15;
    int16_t deja_vu_q15;
    int16_t texture_q15;
    int16_t stereoSpread_q15;
    int16_t feedback_q15;
    int16_t dryWet_q15;
    float pitch_f;
    int8_t loop_length;
    PlayMode mode;
    Pot4Mode pot4_mode;
    int resolution_index;
};
struct UIDisplayCache {
    int16_t position_q15, size_q15, deja_vu_q15, dryWet_q15;
    float   pitch_f;
    PlayMode mode;
    Pot4Mode pot4_mode;
    Pot4Mode pot4_mode_for_text_update;
    int16_t texture_q15, stereoSpread_q15, feedback_q15;
    int8_t loop_length;
    bool   bt_connected;
    int    resolution_index;
    uint8_t active_grains;
    float   bpm;
};

struct AudioRingBuffer {
    int16_t data[RING_BUFFER_SIZE];
    volatile uint16_t writePos = 0, readPos = 0;
    void init() { writePos = 0; readPos = 0; }
    bool write(int16_t sample) {
        uint16_t nextPos = (writePos + 1) & (RING_BUFFER_SIZE - 1);
        if (nextPos == readPos) return false;
        data[writePos] = sample;
        writePos = nextPos;
        return true;
    }
    bool read(int16_t& sample) {
        if (readPos == writePos) return false;
        sample = data[readPos];
        readPos = (readPos + 1) & (RING_BUFFER_SIZE - 1);
        return true;
    }
};
struct Grain {
    bool active;
    uint16_t startPos, length;
    int32_t position_q16, speed_q16;
    uint32_t reciprocal_length_q32;
    int16_t panL_q15, panR_q15;
    float pitch_f;  // Store pitch for accurate visualization
    void reset() {
        active = false;
        position_q16 = 0;
        speed_q16 = 1 << 16;
        reciprocal_length_q32 = 0; panL_q15 = PAN_CENTER_Q15; panR_q15 = PAN_CENTER_Q15;
        pitch_f = 0.0f;
    }
};
struct GranParams {
    float pitch_f;
    PlayMode mode;
    int16_t position_q15;
    int16_t size_q15;
    int16_t deja_vu_q15;
    int16_t texture_q15;
    int16_t stereoSpread_q15;
    int16_t feedback_q15;
    int16_t dryWet_q15;
    int8_t loop_length;
};

struct ButtonState {
    bool currentState = HIGH, lastState = HIGH;
    unsigned long pressStartTime = 0;
    void init() {
        currentState = HIGH;
        lastState = HIGH;
        pressStartTime = 0;
    }
};

struct ParamSnapshot {
    int16_t position_q15;
    int16_t size_q15;
    float pitch_f;
    int16_t texture_q15;
};

// ================================================================= //
// SECTION: Global Variables
// ================================================================= //
TFT_eSPI tft = TFT_eSPI();
BluetoothA2DPSink a2dp_sink;
bool g_inverse_mode = false;
// Audio Buffers
AudioRingBuffer g_ringBuffer;
int16_t g_grainBuffer[GRAIN_BUFFER_SIZE];  // 64KB buffer in internal DRAM
volatile uint16_t g_grainWritePos = 0;
bool g_grainBufferReady = false;

// Grain Management
Grain g_grains[MAX_GRAINS];
uint8_t g_activeGrainIndices[MAX_GRAINS];
uint8_t g_activeGrainCount = 0;

// Look-Up Tables
int16_t g_window_lut_q15[WINDOW_LUT_SIZE];
int32_t g_pitch_lut_q16[PITCH_LUT_SIZE];
int16_t g_pan_lut_q15[PAN_LUT_SIZE];
int16_t g_mix_lut_q15[MIX_LUT_SIZE];
int16_t g_feedback_lut_q15[FEEDBACK_LUT_SIZE];
uint32_t g_reciprocal_lut_q32[RECIPROCAL_LUT_SIZE];
float g_random_pan_lut[RANDOM_PAN_LUT_SIZE];
int16_t g_random_lut_q15[RANDOM_LUT_SIZE];
uint8_t g_random_pan_index = 0;
uint8_t g_random_index = 0;

// Button States
ButtonState g_button, g_pot4_button, g_mode_button;
ButtonState g_snapshot_button[4];
// Parameters
GranParams g_params;
Pot4Mode g_pot4_mode = MODE_TEXTURE;

// UI
UIDisplayCache g_display_cache;
bool g_randomize_flash_active = false;
unsigned long g_randomize_flash_start = 0;
bool g_snapshot_flash_active = false;
unsigned long g_snapshot_flash_start = 0;
int g_snapshot_flash_number = 0;

// Deja Vu
ParamSnapshot g_deja_vu_buffer[DEJA_VU_BUFFER_SIZE];
int g_deja_vu_step = 0;

// Trigger LED
volatile bool g_trigger_led_on = false;
volatile unsigned long g_trigger_led_start_time = 0;
// Clock & Trigger
volatile bool g_trigger_received_isr = false;
volatile unsigned long g_last_trigger_time_isr = 0;
unsigned long g_beat_interval_us = 500000;
unsigned long g_next_internal_trigger_time_us = 0;
// ★★★ ここから追加 ★★★
// 物理LEDを点滅させるための、素のBPM用タイマー
unsigned long g_next_raw_beat_time_us = 0;
// ★★★ ここまで追加 ★★★

const float g_resolutions[] = {0.25f, 0.3333333f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
const char* g_resolution_names[] = {"1/4", "1/3", "1/2", " x1", " x2", " x3", " x4"};
int g_current_resolution_index = 3;
unsigned long g_last_manual_tap_time_us = 0;
float g_current_bpm = 120.0f;

// Snapshot Storage
FullParamSnapshot g_snapshots[4];
bool g_snapshots_initialized[4] = {false, false, false, false};
// ★★★ ここから2行を追加してください ★★★
// 物理BPM LEDの状態を管理する変数
volatile bool g_raw_beat_led_on = false;
volatile unsigned long g_raw_beat_led_start_time = 0;
// --- Soft Takeover for Pitch (POT index 4) ---
bool  g_soft_takeover_active_pitch = false;  // ピッチ用テイクオーバー有効フラグ
float g_soft_takeover_target_pitch = 0.5f;
// 0.0〜1.0: つまみ正規化位置の目標

// ================================================================= //
// SECTION: Forward Declarations
// ================================================================= //
void granularTask(void* param);
void processAudioSample(int16_t inputSample);
void a2dp_data_callback(const uint8_t *data, uint32_t length);
void triggerGrain(int idx, const ParamSnapshot& params);
int16_t renderGrain(Grain& g);
void renderAllGrains(int32_t& wetL, int32_t& wetR);
void handleDejaVuTrigger();
void randomizeDejaVuBuffer();
void randomizeClockResolution();
void enablePitchSoftTakeover(float pitchSemitones);
void updateTempo(unsigned long tap_time_us);
void IRAM_ATTR triggerISR();
uint16_t calculateGrainLength(int16_t base_size, int16_t texture);
uint16_t calculateGrainStartPosition(int16_t base_pos, int16_t texture);
int32_t calculateGrainSpeed(float base_pitch, int16_t texture);
void calculateGrainPanning(int16_t& panL, int16_t& panR);
void updateAllButtons();
void updateMainButton();
void updatePot4Button();
void updateModeButton();
void updateSnapshotButtons();
void saveSnapshot(int slot);
void loadSnapshot(int slot);
void initializeSnapshots();
void updateParametersFromPots();
void updateDisplay();
bool updateFlashScreens();
void updatePot4ModeLabels(uint16_t txt_color, uint16_t bg_color, uint16_t highlight_color);
void updateTriggerLED();
void drawUiFrame();
void drawParameterBar(int x, int y, int16_t val, int16_t& lastVal, uint16_t color);
void drawPitchBar(int x, int y, float val, float& lastVal, uint16_t color);
void drawParticleVisualizer();
void initAllLuts();
const char* getModeString(PlayMode mode);
const char* getPot4ModeString(Pot4Mode mode);
bool handleButtonDebounce(ButtonState& b, int pin);
void invalidateDisplayCache();

// ================================================================= //
// SECTION: Main Setup & Loop
// ================================================================= //
void setup() {
    Serial.begin(115200);
    tft.init();
    tft.setRotation(1);
    initAllLuts();

    g_ringBuffer.init();
    for(int i = 0; i < MAX_GRAINS; i++) g_grains[i].reset();
    memset(g_grainBuffer, 0, sizeof(g_grainBuffer));
    // ★ 修正点: 安全なキャッシュ初期化関数を呼び出す
    invalidateDisplayCache();
    
    g_display_cache.bt_connected = !a2dp_sink.is_connected();
    
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(POT4_BUTTON_PIN, INPUT_PULLUP);
    pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);
    pinMode(TRIGGER_IN_PIN, INPUT_PULLUP);
    pinMode(SNAPSHOT_1_BUTTON_PIN, INPUT_PULLUP);
    pinMode(SNAPSHOT_2_BUTTON_PIN, INPUT_PULLUP);
    pinMode(SNAPSHOT_3_BUTTON_PIN, INPUT_PULLUP);
    pinMode(SNAPSHOT_4_BUTTON_PIN, INPUT_PULLUP);
    pinMode(BPM_LED_PIN, OUTPUT);
    digitalWrite(BPM_LED_PIN, LOW);
    attachInterrupt(digitalPinToInterrupt(TRIGGER_IN_PIN), triggerISR, FALLING);

    g_button.init();
    g_pot4_button.init();
    g_mode_button.init();
    g_snapshot_button[0].init();
    g_snapshot_button[1].init();
    g_snapshot_button[2].init();
    g_snapshot_button[3].init();

    g_params.texture_q15 = 0;
    g_params.stereoSpread_q15 = 29490;
    g_params.feedback_q15 = g_feedback_lut_q15[51];
    g_params.loop_length = 16;
    // 起動時にランダムなパラメータでスナップショットを初期化
    initializeSnapshots();
    
    drawUiFrame();

    xTaskCreatePinnedToCore(granularTask, "Granular", 8192, NULL, 2, NULL, 1);
    delay(500);

    a2dp_sink.set_stream_reader(a2dp_data_callback, false);
    a2dp_sink.start("ESP32-Granular");
    Serial.println("\nSetup Complete! (Perf. Fix v2)");
}

void loop() {
    updateAllButtons();

    // ADC読み取り処理をloop()タスク(Core 0)で実行
    static unsigned long lastPotUpdateTime = 0;
    if (millis() - lastPotUpdateTime > ADC_UPDATE_INTERVAL_MS) {
        lastPotUpdateTime = millis();
        updateParametersFromPots();
    }

    static unsigned long lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate > DISPLAY_UPDATE_INTERVAL_MS) {
        lastDisplayUpdate = millis();
        updateDisplay();
    }

    if (g_randomize_flash_active && millis() - g_randomize_flash_start > RANDOMIZE_FLASH_DURATION_MS) {
        g_randomize_flash_active = false;
        drawUiFrame();
        // ★ 修正点: memsetを削除 (UI更新はinvalidateDisplayCacheが担当)
    }

    if (g_snapshot_flash_active && millis() - g_snapshot_flash_start > RANDOMIZE_FLASH_DURATION_MS) {
        g_snapshot_flash_active = false;
        drawUiFrame();
        // ★ 修正点: memsetを削除 (UI更新はinvalidateDisplayCacheが担当)
    }

    vTaskDelay(pdMS_TO_TICKS(10));
}


// ★ 追加: 表示キャッシュを安全に無効化する関数
void invalidateDisplayCache() {
    // 各パラメータを、通常ありえない値に設定することで、次回の描画を強制する
    g_display_cache.position_q15 = -1;
    g_display_cache.size_q15 = -1;
    g_display_cache.deja_vu_q15 = -1;
    g_display_cache.dryWet_q15 = -1;
    g_display_cache.pitch_f = -1000.0f; // float型にはレンジ外の値を
    g_display_cache.mode = (PlayMode)-1;
    // enum型には無効な値を
    g_display_cache.pot4_mode = (Pot4Mode)-1;
    g_display_cache.pot4_mode_for_text_update = (Pot4Mode)-1;
    g_display_cache.texture_q15 = -1;
    g_display_cache.stereoSpread_q15 = -1;
    g_display_cache.feedback_q15 = -1;
    g_display_cache.loop_length = -1;
    g_display_cache.resolution_index = -1;
    g_display_cache.active_grains = 255; // uint8_tには最大値などを
    g_display_cache.bpm = -1000.0f;
    // g_display_cache.bt_connected はsetupで初期化されるのでここでは不要
}

// ================================================================= //
// SECTION: Interrupt Service Routine
// ================================================================= //
void IRAM_ATTR triggerISR() {
    g_last_trigger_time_isr = micros();
    g_trigger_received_isr = true;
}

// ================================================================= //
// SECTION: Audio Processing Task (Core 1)
// ================================================================= //
void granularTask(void* param) {
    Serial.println("Granular task started on Core 1");
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 128,
        .use_apll = false,
     
 
       .tx_desc_auto_clear = true
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_OUT_BCLK,
        .ws_io_num = I2S_OUT_LRC,
        .data_out_num = I2S_OUT_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    // Initialize I2S driver with error checking
    esp_err_t i2s_err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    if (i2s_err != ESP_OK) {
        Serial.printf("ERROR: I2S driver install failed: %d\n", i2s_err);
        vTaskDelete(NULL);
        return;
    }

    i2s_err = i2s_set_pin(I2S_NUM_1, &pin_config);
    if (i2s_err != ESP_OK) {
        Serial.printf("ERROR: I2S set pin failed: %d\n", i2s_err);
        vTaskDelete(NULL);
        return;
    }

    i2s_zero_dma_buffer(I2S_NUM_1);

    while (true) {
        unsigned long current_time_us = micros();
        if (g_trigger_received_isr) {
            unsigned long isr_time = g_last_trigger_time_isr;
            g_trigger_received_isr = false;
            updateTempo(isr_time);
        }

        // 分解能が適用されたクロック（画面LEDやエフェクトのトリガー用）
        if (current_time_us >= g_next_internal_trigger_time_us && g_next_internal_trigger_time_us > 0) {
            handleDejaVuTrigger();
            unsigned long internal_interval = g_beat_interval_us / g_resolutions[g_current_resolution_index];
            g_next_internal_trigger_time_us += internal_interval;
        }

        // 分解能適用前の「素のBPM」クロック（物理LED用）
        if (current_time_us >= g_next_raw_beat_time_us && g_next_raw_beat_time_us > 0) {
            // LEDの点灯フラグを立てる
            g_raw_beat_led_on = true;
            g_raw_beat_led_start_time = millis();
            // 次の点滅時間をスケジュールする
            g_next_raw_beat_time_us += g_beat_interval_us;
        }
        
        // 物理BPM LEDの制御
        if (g_raw_beat_led_on) {
            digitalWrite(BPM_LED_PIN, HIGH);
            // 一定時間が経過したらLEDを消灯し、フラグをリセット
            if (millis() - g_raw_beat_led_start_time > BPM_LED_PULSE_DURATION_MS) {
                g_raw_beat_led_on = false;
                digitalWrite(BPM_LED_PIN, LOW);
            }
        }

        int16_t inputSample;
        if (g_ringBuffer.read(inputSample)) {
            processAudioSample(inputSample);
        } else {
            vTaskDelay(1);
        }
    }
}

void processAudioSample(int16_t inputSample) {
    static int16_t i2s_buffer[I2S_BUFFER_SAMPLES * 2];
    static int16_t feedbackBuffer[FEEDBACK_BUFFER_SIZE];
    static int i2s_buffer_pos = 0;
    static uint16_t fbWritePos = 0;

    int16_t fbSample = feedbackBuffer[fbWritePos];
    int32_t mixed = inputSample + (((int32_t)fbSample * g_params.feedback_q15) >> 15);
    mixed = constrain(mixed, -32767, 32767);

    g_grainBuffer[g_grainWritePos] = (int16_t)mixed;
    g_grainWritePos = (g_grainWritePos + 1) & GRAIN_BUFFER_MASK;

    if (!g_grainBufferReady && g_grainWritePos > GRAIN_BUFFER_SIZE / 2) {
        g_grainBufferReady = true;
    }

    int32_t wetL_accumulator = 0, wetR_accumulator = 0;
    renderAllGrains(wetL_accumulator, wetR_accumulator);

    int16_t wetL = constrain(wetL_accumulator, -32767, 32767);
    int16_t wetR = constrain(wetR_accumulator, -32768, 32767);

    int16_t wet_q15 = g_params.dryWet_q15;
    int16_t dry_q15 = 32767 - wet_q15;
    int16_t outL = constrain(((int32_t)inputSample*dry_q15+(int32_t)wetL*wet_q15)>>15, -32768, 32767);
    int16_t outR = constrain(((int32_t)inputSample*dry_q15+(int32_t)wetR*wet_q15)>>15, -32768, 32767);
    feedbackBuffer[fbWritePos] = (int16_t)((((long)outL + outR) >> 1) * g_params.feedback_q15 >> 15);
    fbWritePos = (fbWritePos + 1) & (FEEDBACK_BUFFER_SIZE - 1);
    i2s_buffer[i2s_buffer_pos++] = outL;
    i2s_buffer[i2s_buffer_pos++] = outR;

    if (i2s_buffer_pos >= I2S_BUFFER_SAMPLES * 2) {
        size_t bytes_written;
        esp_err_t i2s_result = i2s_write(I2S_NUM_1, i2s_buffer, i2s_buffer_pos*sizeof(int16_t), &bytes_written, portMAX_DELAY);

        // Check for I2S write errors (avoid logging in real-time path to prevent performance degradation)
        if (i2s_result != ESP_OK) {
            static uint32_t error_count = 0;
            error_count++;
            // Only log every 1000th error to avoid flooding serial output
            if (error_count % 1000 == 0) {
                Serial.printf("WARNING: I2S write error: %d (count: %u)\n", i2s_result, error_count);
            }
        }

        i2s_buffer_pos = 0;
    }
}

void a2dp_data_callback(const uint8_t *data, uint32_t length) {
    int16_t* samples = (int16_t*)data;
    for(uint32_t i=0; i<length/4; i++) {
        g_ringBuffer.write((samples[i*2]>>1)+(samples[i*2+1]>>1));
    }
}

// ================================================================= //
// SECTION: Grain Generation & Rendering
// ================================================================= //
void handleDejaVuTrigger() {
    if (!g_grainBufferReady) return;
    g_trigger_led_on = true;
    g_trigger_led_start_time = millis();

    bool replay = (esp_random() % 32768) < g_params.deja_vu_q15;
    ParamSnapshot params_to_use;
    int current_step_in_loop = g_deja_vu_step % g_params.loop_length;

    if (replay) {
        params_to_use = g_deja_vu_buffer[current_step_in_loop];
    } else {
        int16_t rand_val = (esp_random() % 65535) - 32767;
        int32_t pos_offset = ((int32_t)g_params.texture_q15 * rand_val) >> 14;
        params_to_use.position_q15 = constrain(g_params.position_q15 + pos_offset, 0, 32767);
        rand_val = (esp_random() % 65535) - 32767;
        int32_t size_offset = ((int32_t)g_params.texture_q15 * rand_val) >> 15;
        params_to_use.size_q15 = constrain(g_params.size_q15 + size_offset, 1000, 32767);

        rand_val = (esp_random() % 65535) - 32767;
        float pitch_offset = (g_params.texture_q15 / 32767.0f) * 5.0f * (rand_val / 32767.0f);
        params_to_use.pitch_f = g_params.pitch_f + pitch_offset;
        params_to_use.texture_q15 = g_params.texture_q15;
        g_deja_vu_buffer[current_step_in_loop] = params_to_use;
    }

    // Generate 1-3 grains per trigger for richer polyphony
    uint8_t grains_to_generate = 1 + (esp_random() % 3);  // Random 1-3
    uint8_t grains_generated = 0;

    for (int i = 0; i < MAX_GRAINS && grains_generated < grains_to_generate; i++) {
        if (!g_grains[i].active) {
            // Add slight variation to each grain for richer sound
            ParamSnapshot varied_params = params_to_use;

            if (grains_generated > 0) {
                // Add small pitch variation (±2 semitones)
                int16_t pitch_var = (esp_random() % 4001) - 2000;  // -2000 to +2000
                varied_params.pitch_f = params_to_use.pitch_f + (pitch_var / 1000.0f);

                // Add small position variation
                int16_t pos_var = (esp_random() % 6553) - 3276;  // ±10% of range
                varied_params.position_q15 = constrain(params_to_use.position_q15 + pos_var, 0, 32767);
            }

            triggerGrain(i, varied_params);
            grains_generated++;
        }
    }

    g_deja_vu_step = (g_deja_vu_step + 1) % DEJA_VU_BUFFER_SIZE;
}

// ================================================================= //
// SECTION: Soft Takeover Helper
// ================================================================= //
void enablePitchSoftTakeover(float pitchSemitones) {
    // Convert pitch in semitones to normalized position (0..1) for soft takeover
    float v = (pitchSemitones / PITCH_RANGE_SEMITONES) + 0.5f;
    if (!isfinite(v)) v = 0.5f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_soft_takeover_target_pitch = v;
    g_soft_takeover_active_pitch = true;
}

void randomizeDejaVuBuffer() {
    // Deja Vuバッファのランダマイズ
    for (int i = 0; i < DEJA_VU_BUFFER_SIZE; i++) {
        g_deja_vu_buffer[i].position_q15 = esp_random() % 32768;
        g_deja_vu_buffer[i].size_q15     = 1000 + (esp_random() % 31767);
        g_deja_vu_buffer[i].pitch_f      = (float)((esp_random() % 240) - 120) / 10.0f;
        g_deja_vu_buffer[i].texture_q15  = esp_random() % 32768;
    }

    // 現在のパラメータもランダマイズ
    g_params.position_q15     = esp_random() % 32768;
    g_params.size_q15         = 1000 + (esp_random() % 31767);
    g_params.deja_vu_q15      = esp_random() % 32768;
    g_params.texture_q15      = esp_random() % 32768;
    g_params.stereoSpread_q15 = esp_random() % 32768;
    g_params.feedback_q15     = g_feedback_lut_q15[esp_random() % FEEDBACK_LUT_SIZE];
    g_params.dryWet_q15       = 32767;

    // 0.0f～1.0fのランダムなfloatを生成し、pitch範囲に変換する
    float random_float = (float)esp_random() / (float)UINT32_MAX;
    g_params.pitch_f = PITCH_RANDOM_MIN + (PITCH_RANDOM_RANGE * random_float);

    g_params.loop_length      = 2 + (esp_random() % (DEJA_VU_BUFFER_SIZE - 1));
    g_params.mode             = (esp_random() % 2 == 0) ?
    MODE_GRANULAR : MODE_REVERSE;
    g_pot4_mode               = (Pot4Mode)(esp_random() % POT4_MODE_COUNT);
    g_current_resolution_index = esp_random() % (sizeof(g_resolutions) / sizeof(g_resolutions[0]));

    g_deja_vu_step = 0;
    // ★ ピッチつまみ用ソフトテイクオーバー有効化
    enablePitchSoftTakeover(g_params.pitch_f);
    // 演出（フラッシュ表示）
    g_randomize_flash_active = true;
    g_randomize_flash_start  = millis();
    invalidateDisplayCache();
}

void triggerGrain(int idx, const ParamSnapshot& params) {
    if (idx < 0 || idx >= MAX_GRAINS) return;
    Grain& g = g_grains[idx];
    g.length = calculateGrainLength(params.size_q15, params.texture_q15);
    g.startPos = calculateGrainStartPosition(params.position_q15, params.texture_q15);
    g.speed_q16 = calculateGrainSpeed(params.pitch_f, params.texture_q15);
    g.pitch_f = params.pitch_f;  // Store pitch for visualization
    calculateGrainPanning(g.panL_q15, g.panR_q15);
    g.position_q16 = (g_params.mode == MODE_REVERSE) ? (int32_t)(g.length - 1) << 16 : 0;
    uint16_t lut_idx = ((g.length - MIN_GRAIN_SIZE) * (RECIPROCAL_LUT_SIZE - 1)) / (MAX_GRAIN_SIZE - MIN_GRAIN_SIZE);
    g.reciprocal_length_q32 = g_reciprocal_lut_q32[min(lut_idx, (uint16_t)(RECIPROCAL_LUT_SIZE - 1))];
    g.active = true;

    bool found = false;
    for(uint8_t i=0; i<g_activeGrainCount; i++) {
        if(g_activeGrainIndices[i]==idx) found=true;
    }
    if(!found && g_activeGrainCount<MAX_GRAINS) {
        g_activeGrainIndices[g_activeGrainCount++]=idx;
    }
}

void renderAllGrains(int32_t& wetL, int32_t& wetR) {
    if (!g_grainBufferReady || g_activeGrainCount == 0) return;
    for (uint8_t i = 0; i < g_activeGrainCount; ) {
        uint8_t grain_idx = g_activeGrainIndices[i];
        Grain& grain = g_grains[grain_idx];
        int16_t sample = renderGrain(grain);

        if (grain.active) {
            wetL += ((int32_t)sample * grain.panL_q15) >> 15;
            wetR += ((int32_t)sample * grain.panR_q15) >> 15;
            i++;
        } else {
            for (uint8_t j = i; j < g_activeGrainCount - 1; j++) {
                g_activeGrainIndices[j] = g_activeGrainIndices[j+1];
            }
            g_activeGrainCount--;
        }
    }
}

int16_t renderGrain(Grain& g) {
    uint16_t pos_int = g.position_q16 >> 16;
    if (pos_int >= g.length) {
        g.active = false;
        return 0;
    }

    uint16_t read_idx = (g.startPos + ((g_params.mode == MODE_REVERSE) ? g.length-1-pos_int : pos_int)) & GRAIN_BUFFER_MASK;
    int16_t sample = g_grainBuffer[read_idx];

    uint16_t window_idx = ((uint32_t)pos_int * g.reciprocal_length_q32) >> 25;
    int16_t window_val = g_window_lut_q15[min((uint16_t)window_idx, (uint16_t)(WINDOW_LUT_SIZE-1))];
    int32_t windowed_sample = (int32_t)sample * window_val;

    g.position_q16 += (g_params.mode == MODE_REVERSE) ? -g.speed_q16 : g.speed_q16;
    if (g.position_q16 < 0) {
        g.active = false;
    }

    return (int16_t)(windowed_sample >> 15);
}

uint16_t calculateGrainLength(int16_t base_size, int16_t texture) {
    int16_t rand_val = g_random_lut_q15[(g_random_index++)&(RANDOM_LUT_SIZE-1)];
    int32_t size_rand_comp = ((int32_t)texture * rand_val) >> 15;
    int16_t size_q15 = constrain(base_size + (size_rand_comp >> 1), MIN_SIZE_Q15, 32767);
    return MIN_GRAIN_SIZE + (((uint32_t)(MAX_GRAIN_SIZE - MIN_GRAIN_SIZE) * size_q15) >> 15);
}

uint16_t calculateGrainStartPosition(int16_t base_pos, int16_t texture) {
    int16_t rand_val = g_random_lut_q15[(g_random_index++)&(RANDOM_LUT_SIZE-1)];
    int32_t pos_rand_comp = (int32_t)((((int32_t)texture * rand_val) >> 15) * POSITION_TEXTURE_SCALE);
    int16_t pos_q15 = constrain(base_pos + pos_rand_comp, 0, 32767);
    uint32_t lookback = ((uint32_t)GRAIN_BUFFER_SIZE * pos_q15) >> 15;
    return (g_grainWritePos - lookback + GRAIN_BUFFER_SIZE) & GRAIN_BUFFER_MASK;
}

int32_t calculateGrainSpeed(float base_pitch, int16_t texture) {
    int16_t rand_val = g_random_lut_q15[(g_random_index++)&(RANDOM_LUT_SIZE-1)];
    float pitch_rand_comp = (texture/32767.0f) * PITCH_TEXTURE_VARIANCE * (rand_val/32767.0f);
    float pitch = base_pitch + pitch_rand_comp;

    // ★★★ 変更点：事前計算した定数を使って割り算を掛け算に置き換え ★★★
    float index_f = (pitch + PITCH_RANGE_SEMITONES_HALF) * PITCH_LUT_SCALE;

    index_f = constrain(index_f,0.0f,PITCH_LUT_SIZE-2.0f);

    int index_i = (int)index_f;
    int32_t frac_q8 = (int32_t)((index_f - index_i) * 256.0f);
    int32_t y0 = g_pitch_lut_q16[index_i], y1 = g_pitch_lut_q16[index_i+1];
    int32_t speed = y0 + (((y1 - y0) * frac_q8) >> 8);
    return constrain(speed, 1<<14, 4<<16);
}

void calculateGrainPanning(int16_t& panL, int16_t& panR) {
    float pan_random = g_random_pan_lut[(g_random_pan_index++)&(RANDOM_PAN_LUT_SIZE-1)];
    float pan = 0.5f+(g_params.stereoSpread_q15/32767.0f)*STEREO_SPREAD_SCALE*pan_random;
    pan = constrain(pan,0.0f,1.0f);

    float pan_index_f = pan*(PAN_LUT_SIZE-1);
    int pan_index_i = constrain((int)pan_index_f,0,PAN_LUT_SIZE-2);
    int32_t frac_q8 = (int32_t)((pan_index_f-pan_index_i)*256.0f);
    panR = g_pan_lut_q15[pan_index_i]+(((g_pan_lut_q15[pan_index_i+1]-g_pan_lut_q15[pan_index_i])*frac_q8)>>8);

    float pan_index_l_f = (PAN_LUT_SIZE-1)-pan_index_f;
    int pan_index_l_i = constrain((int)pan_index_l_f,0,PAN_LUT_SIZE-2);
    int32_t frac_l_q8 = (int32_t)((pan_index_l_f-pan_index_l_i)*256.0f);
    panL = g_pan_lut_q15[pan_index_l_i]+(((g_pan_lut_q15[pan_index_l_i+1]-g_pan_lut_q15[pan_index_l_i])*frac_l_q8)>>8);
}

// ================================================================= //
// SECTION: Controls - Tempo & Parameters
// ================================================================= //
void updateTempo(unsigned long tap_time_us) {
    static unsigned long last_any_tap_time_us = 0;
    if (last_any_tap_time_us > 0) {
        unsigned long interval = tap_time_us - last_any_tap_time_us;
        if (interval > MIN_TEMPO_INTERVAL_US && interval < MAX_TEMPO_INTERVAL_US) {
            g_beat_interval_us = interval;
            g_current_bpm = 60000000.0f / g_beat_interval_us;
        }
    }

    last_any_tap_time_us = tap_time_us;
    g_next_internal_trigger_time_us = tap_time_us;
    // 物理LED用のタイマーも、このタイミングでリセット（同期）する
    g_next_raw_beat_time_us = tap_time_us;

    // 物理LEDの点灯フラグを立て、時間を記録する
    g_raw_beat_led_on = true;
    g_raw_beat_led_start_time = millis();
}

void updateParametersFromPots() {
    static int   last_adc_values[6] = {-1,-1,-1,-1,-1,-1};
    static float last_val_f[6]      = {NAN,NAN,NAN,NAN,NAN,NAN};
    // 0..1 の前回値
    const int pot_pins[] = {POT1_PIN, POT2_PIN, POT3_PIN, POT4_PIN, POT5_PIN, POT6_PIN};
    for(int i=0; i<6; i++) {
        uint32_t adc_accumulator = 0;
        for(int s=0; s<ADC_SMOOTHING_SAMPLES; s++) {
            adc_accumulator += analogRead(pot_pins[i]);
        }
        int smoothed_adc_val = adc_accumulator / ADC_SMOOTHING_SAMPLES;
        if (abs(smoothed_adc_val - last_adc_values[i]) > ADC_CHANGE_THRESHOLD) {
            last_adc_values[i] = smoothed_adc_val;
            float val_f = smoothed_adc_val / ADC_MAX_VALUE;   // 0..1
            if (!isfinite(val_f)) val_f = 0.5f;
            if (val_f < 0.0f) val_f = 0.0f; if (val_f > 1.0f) val_f = 1.0f;
            switch(i) {
                case 0:
                    g_params.position_q15 = (int16_t)(val_f * 32767.0f);
                    break;

                case 1:
                    g_params.size_q15 = (int16_t)(val_f * 32767.0f);
                    break;

                case 2:
                    g_params.deja_vu_q15 = (int16_t)(val_f * 32767.0f);
                    break;

                case 3: // POT4: モード切替によって割当が変わる
                    switch(g_pot4_mode) {
                        case MODE_TEXTURE:
                            g_params.texture_q15 = (int16_t)(val_f * 32767.0f);
                            break;
                        case MODE_SPREAD:
                            g_params.stereoSpread_q15 = (int16_t)(val_f * 32767.0f);
                            break;
                        case MODE_FEEDBACK:
                            g_params.feedback_q15 = g_feedback_lut_q15[(int)(val_f * (FEEDBACK_LUT_SIZE - 1))];
                            break;
                        case MODE_LOOP_LENGTH: {
                            int len = map(smoothed_adc_val, 0, 4095, 2, DEJA_VU_BUFFER_SIZE + 1);
                            g_params.loop_length = constrain(len, 2, DEJA_VU_BUFFER_SIZE);
                            break;
                        }
                        case MODE_CLK_RESOLUTION: {
                            int resolution = map(smoothed_adc_val, 0, 4095, 0, 6);
                            g_current_resolution_index = constrain(resolution, 0, 6);
                            break;
                        }
                    }
                    break;
                case 4: { // ★ ピッチ（ソフトテイクオーバー対応）
                    // ランダマイズ／読込直後は、物理つまみが「目標位置」に近づくまで上書きしない
                    if (g_soft_takeover_active_pitch) {
                        const float deadband = SOFT_TAKEOVER_DEADBAND;
                        // 3% 以内ならキャッチとみなす
                        float target = g_soft_takeover_target_pitch;
                        bool pass_through = false;
                        if (isfinite(last_val_f[4])) {
                            // 目標を跨いだか？
                            pass_through = ( (last_val_f[4] < target && val_f >= target) ||
                           
                                  (last_val_f[4] > target && val_f <= target) );
                        }
                        bool near_enough = (fabsf(val_f - target) <= deadband);
                        if (!(pass_through || near_enough)) {
                            // まだキャッチしていない → このサイクルはピッチ更新しない
                            last_val_f[4] = val_f;
                            break; // case 4 を抜ける（他パラメータへの影響なし）
                        }

                        // キャッチ成立 → 以降は通常更新
                        g_soft_takeover_active_pitch = false;
                    }

                    g_params.pitch_f = (val_f - 0.5f) * PITCH_RANGE_SEMITONES;
                    if (!isfinite(g_params.pitch_f)) g_params.pitch_f = 0.0f;
                    if (g_params.pitch_f >  PITCH_RANGE_SEMITONES_HALF) g_params.pitch_f =  PITCH_RANGE_SEMITONES_HALF;
                    if (g_params.pitch_f < -PITCH_RANGE_SEMITONES_HALF) g_params.pitch_f = -PITCH_RANGE_SEMITONES_HALF;

                    last_val_f[4] = val_f;
                    break;
                }

                case 5:
                    g_params.dryWet_q15 = g_mix_lut_q15[(int)(val_f * (MIX_LUT_SIZE - 1))];
                    break;
            }
        }
    }
}
// ================================================================= //
// SECTION: Snapshot Functions
// ================================================================= //
void initializeSnapshots() {
    Serial.println("Initializing snapshots with random parameters...");
    // Deja Vuバッファも起動時にランダム化
    for (int i = 0; i < DEJA_VU_BUFFER_SIZE; i++) {
        g_deja_vu_buffer[i].position_q15 = esp_random() % 32768;
        g_deja_vu_buffer[i].size_q15     = 1000 + (esp_random() % 31767);
        g_deja_vu_buffer[i].pitch_f      = (float)((esp_random() % 240) - 120) / 10.0f;
        g_deja_vu_buffer[i].texture_q15  = esp_random() % 32768;
    }

    for (int i = 0; i < 4; i++) {
        g_snapshots[i].position_q15     = esp_random() % 32768;
        g_snapshots[i].size_q15         = 1000 + (esp_random() % 31767);
        g_snapshots[i].deja_vu_q15      = esp_random() % 32768;
        g_snapshots[i].texture_q15      = esp_random() % 32768;
        g_snapshots[i].stereoSpread_q15 = esp_random() % 32768;
        g_snapshots[i].feedback_q15     = g_feedback_lut_q15[esp_random() % FEEDBACK_LUT_SIZE];
        // 0.0f～1.0fのランダムなfloatを生成し、pitch範囲に変換する
        float random_float = (float)esp_random() / (float)UINT32_MAX;
        g_snapshots[i].pitch_f = PITCH_RANDOM_MIN + (PITCH_RANDOM_RANGE * random_float);

        g_snapshots[i].loop_length  = 2 + (esp_random() % (DEJA_VU_BUFFER_SIZE - 1));
        g_snapshots[i].mode         = (esp_random() % 2 == 0) ? MODE_GRANULAR : MODE_REVERSE;
        g_snapshots[i].pot4_mode    = (Pot4Mode)(esp_random() % POT4_MODE_COUNT);
        g_snapshots[i].resolution_index = 3 + (esp_random() % 4);
        g_snapshots[i].dryWet_q15   = (i < 3) ? 32767 : 0;

        g_snapshots_initialized[i] = true;
    }

    // スナップショット1をロード → この中で g_params.pitch_f が設定される
    loadSnapshot(0);
    // ★ ピッチつまみ用ソフトテイクオーバー有効化
    // ランダムで決まった g_params.pitch_f に対応する物理つまみの正規化位置(0..1)を計算
    enablePitchSoftTakeover(g_params.pitch_f);

    Serial.println("Initialization complete. Snapshot 1 loaded.");
}

void saveSnapshot(int slot) {
    if (slot < 0 || slot >= 4) return;
    g_snapshots[slot].position_q15 = g_params.position_q15;
    g_snapshots[slot].size_q15 = g_params.size_q15;
    g_snapshots[slot].deja_vu_q15 = g_params.deja_vu_q15;
    g_snapshots[slot].texture_q15 = g_params.texture_q15;
    g_snapshots[slot].stereoSpread_q15 = g_params.stereoSpread_q15;
    g_snapshots[slot].feedback_q15 = g_params.feedback_q15;
    g_snapshots[slot].dryWet_q15 = g_params.dryWet_q15;
    g_snapshots[slot].pitch_f = g_params.pitch_f;
    g_snapshots[slot].loop_length = g_params.loop_length;
    g_snapshots[slot].mode = g_params.mode;
    g_snapshots[slot].pot4_mode = g_pot4_mode;
    g_snapshots[slot].resolution_index = g_current_resolution_index;
    g_snapshots_initialized[slot] = true;
    g_snapshot_flash_active = true;
    g_snapshot_flash_start = millis();
    g_snapshot_flash_number = slot + 1;

    Serial.printf("Snapshot %d saved\n", slot + 1);
}
void loadSnapshot(int slot) {
    if (slot < 0 || slot >= 4 || !g_snapshots_initialized[slot]) {
        Serial.printf("Snapshot %d not initialized\n", slot + 1);
        return;
    }

    g_params.position_q15     = g_snapshots[slot].position_q15;
    g_params.size_q15         = g_snapshots[slot].size_q15;
    g_params.deja_vu_q15      = g_snapshots[slot].deja_vu_q15;
    g_params.texture_q15      = g_snapshots[slot].texture_q15;
    g_params.stereoSpread_q15 = g_snapshots[slot].stereoSpread_q15;
    g_params.feedback_q15     = g_snapshots[slot].feedback_q15;
    g_params.dryWet_q15       = g_snapshots[slot].dryWet_q15;
    g_params.pitch_f          = g_snapshots[slot].pitch_f;
    g_params.loop_length      = g_snapshots[slot].loop_length;
    g_params.mode             = g_snapshots[slot].mode;
    // ★ ここを追加：CLK（分解能）をスナップショットから復元
    int idx = g_snapshots[slot].resolution_index;
    if (!isfinite((float)idx)) idx = 3;
    // 念のため
    g_current_resolution_index = constrain(idx, 0, 6);
    // 安全に丸める（×1以上に限定したいなら 3,6 に）

    // ★ ピッチつまみ用ソフトテイクオーバー有効化
    enablePitchSoftTakeover(g_params.pitch_f);

    invalidateDisplayCache();
    Serial.printf("Snapshot %d loaded\n", slot + 1);
}

// ================================================================= //
// SECTION: Button Handling
// ================================================================= //
bool handleButtonDebounce(ButtonState& b, int pin) {
    static int lastReading[7] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
    static unsigned long lastDebounceTime[7] = {0, 0, 0, 0, 0, 0, 0};

    int buttonIndex;
    if (pin == BUTTON_PIN) buttonIndex = 0;
    else if (pin == POT4_BUTTON_PIN) buttonIndex = 1;
    else if (pin == MODE_BUTTON_PIN) buttonIndex = 2;
    else if (pin == SNAPSHOT_1_BUTTON_PIN) buttonIndex = 3;
    else if (pin == SNAPSHOT_2_BUTTON_PIN) buttonIndex = 4;
    else if (pin == SNAPSHOT_3_BUTTON_PIN) buttonIndex = 5;
    else buttonIndex = 6;
    int reading = digitalRead(pin);

    if (reading != lastReading[buttonIndex]) {
        lastDebounceTime[buttonIndex] = millis();
    }
    lastReading[buttonIndex] = reading;

    if ((millis() - lastDebounceTime[buttonIndex]) > BUTTON_DEBOUNCE_MS) {
        if (reading != b.currentState) {
            b.currentState = reading;
            return true;
        }
    }
    return false;
}

void updateMainButton() {
    handleButtonDebounce(g_button, BUTTON_PIN);
    if (g_button.lastState == HIGH && g_button.currentState == LOW) {
        g_button.pressStartTime = millis();
    }

    if (g_button.lastState == LOW && g_button.currentState == HIGH) {
        unsigned long pressDuration = millis() - g_button.pressStartTime;
        if (pressDuration < BUTTON_LONG_PRESS_MS) {
            unsigned long now_us = micros();
            updateTempo(now_us);

            if (g_last_manual_tap_time_us == 0 || (now_us - g_last_manual_tap_time_us >= TAP_TEMPO_TIMEOUT_US)) {
                handleDejaVuTrigger();
            }

            g_last_manual_tap_time_us = now_us;
        } else {
            randomizeDejaVuBuffer();
        }
    }

    g_button.lastState = g_button.currentState;
}


void randomizeClockResolution() {
    // ×1以上（1.0, 2.0, 3.0, 4.0）からランダム選択 → インデックス 3..6
    const int min_idx = 3;
    const int max_idx = 6;
    g_current_resolution_index = min_idx + (esp_random() % (max_idx - min_idx + 1));

    g_randomize_flash_active = true;
    g_randomize_flash_start = millis();
}


void updatePot4Button() {
    handleButtonDebounce(g_pot4_button, POT4_BUTTON_PIN);
    if(g_pot4_button.lastState == HIGH && g_pot4_button.currentState == LOW) {
        g_pot4_button.pressStartTime = millis();
    }

    if (g_pot4_button.lastState == LOW && g_pot4_button.currentState == HIGH) {
        if (millis() - g_pot4_button.pressStartTime < BUTTON_LONG_PRESS_MS) {
            g_pot4_mode = (Pot4Mode)((g_pot4_mode + 1) % POT4_MODE_COUNT);
        }
    }

    g_pot4_button.lastState = g_pot4_button.currentState;
}

void updateModeButton() {
    handleButtonDebounce(g_mode_button, MODE_BUTTON_PIN);
    if (g_mode_button.lastState == HIGH && g_mode_button.currentState == LOW) {
        g_mode_button.pressStartTime = millis();
    }

    if (g_mode_button.lastState == LOW && g_mode_button.currentState == HIGH) {
        unsigned long pressDuration = millis() - g_mode_button.pressStartTime;
        if (pressDuration < BUTTON_LONG_PRESS_MS) {
            // 短押し：再生モードを切り替え
            g_params.mode = (g_params.mode == MODE_GRANULAR) ?
            MODE_REVERSE : MODE_GRANULAR;
        } else {
            // 長押し：全スナップショットを再ランダマイズ
            initializeSnapshots();
            // ランダマイズを知らせるために画面をフラッシュ
            g_randomize_flash_active = true;
            g_randomize_flash_start = millis();
            invalidateDisplayCache();
        }
    }

    g_mode_button.lastState = g_mode_button.currentState;
}
void updateSnapshotButtons() {
    const int pins[4] = {SNAPSHOT_1_BUTTON_PIN, SNAPSHOT_2_BUTTON_PIN, SNAPSHOT_3_BUTTON_PIN, SNAPSHOT_4_BUTTON_PIN};
    for (int i = 0; i < 4; i++) {
        handleButtonDebounce(g_snapshot_button[i], pins[i]);
        if (g_snapshot_button[i].lastState == HIGH && g_snapshot_button[i].currentState == LOW) {
            g_snapshot_button[i].pressStartTime = millis();
        }

        if (g_snapshot_button[i].lastState == LOW && g_snapshot_button[i].currentState == HIGH) {
            unsigned long pressDuration = millis() - g_snapshot_button[i].pressStartTime;
            if (pressDuration >= BUTTON_LONG_PRESS_MS) {
                // 長押し：現在の設定を押されたボタンのスロットに保存
                saveSnapshot(i);
            } else {
                // 短押し：スナップショットをロード
                loadSnapshot(i);
            }
        }

        g_snapshot_button[i].lastState = g_snapshot_button[i].currentState;
    }
}

void updateAllButtons() {
    updateMainButton();
    updatePot4Button();
    updateModeButton();
    updateSnapshotButtons();
}

// ================================================================= //
// SECTION: User Interface
// ================================================================= //

// Display helper: Handle flash screen messages
// Returns true if a flash screen was shown (indicating early return)
bool updateFlashScreens() {
    if (g_randomize_flash_active) {
        tft.fillScreen(TFT_WHITE);
        tft.setTextColor(TFT_RED, TFT_WHITE);
        tft.setTextSize(4);
        tft.setCursor(50, 100);
        tft.print("RANDOM!");
        return true;
    }

    if (g_snapshot_flash_active) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setTextSize(3);
        tft.setCursor(40, 90);
        tft.print("SNAPSHOT ");
        tft.print(g_snapshot_flash_number);
        tft.setCursor(70, 120);
        tft.print("SAVED!");
        return true;
    }

    return false;
}

// Display helper: Update Pot4 mode label highlighting
void updatePot4ModeLabels(uint16_t txt_color, uint16_t bg_color, uint16_t highlight_color) {
    if (g_pot4_mode == g_display_cache.pot4_mode) {
        return;
    }

    const char* labels1[] = {"POS", "SIZ", "DEJA", "TEX", "FBK", "CLK", "LOOP"};
    const char* labels2[] = {"PIT", "MIX", "GRNS", "SPR", "MODE", "BT", "POT4"};

    // Clear old highlighted label
    tft.setTextColor(txt_color, bg_color);
    Pot4Mode old_mode = g_display_cache.pot4_mode;
    switch(old_mode) {
        case MODE_TEXTURE:
            tft.setCursor(UI_COL1_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 3 + 2);
            tft.print(labels1[3]);
            break;
        case MODE_SPREAD:
            tft.setCursor(UI_COL2_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 3 + 2);
            tft.print(labels2[3]);
            break;
        case MODE_FEEDBACK:
            tft.setCursor(UI_COL1_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 4 + 2);
            tft.print(labels1[4]);
            break;
        case MODE_CLK_RESOLUTION:
            tft.setCursor(UI_COL1_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 5 + 2);
            tft.print(labels1[5]);
            break;
        case MODE_LOOP_LENGTH:
            tft.setCursor(UI_COL1_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 6 + 2);
            tft.print(labels1[6]);
            break;
    }

    // Highlight new label
    tft.setTextColor(highlight_color, bg_color);
    switch(g_pot4_mode) {
        case MODE_TEXTURE:
            tft.setCursor(UI_COL1_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 3 + 2);
            tft.print(labels1[3]);
            break;
        case MODE_SPREAD:
            tft.setCursor(UI_COL2_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 3 + 2);
            tft.print(labels2[3]);
            break;
        case MODE_FEEDBACK:
            tft.setCursor(UI_COL1_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 4 + 2);
            tft.print(labels1[4]);
            break;
        case MODE_CLK_RESOLUTION:
            tft.setCursor(UI_COL1_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 5 + 2);
            tft.print(labels1[5]);
            break;
        case MODE_LOOP_LENGTH:
            tft.setCursor(UI_COL1_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 6 + 2);
            tft.print(labels1[6]);
            break;
    }

    g_display_cache.pot4_mode = g_pot4_mode;
}

// Display helper: Update trigger LED animation
void updateTriggerLED() {
    static bool last_led_state = false;

    if (g_trigger_led_on) {
        if (!last_led_state) {
            tft.fillCircle(UI_TRIGGER_LED_X, UI_TRIGGER_LED_Y, UI_TRIGGER_LED_RADIUS, TFT_RED);
            last_led_state = true;
        }
        if (millis() - g_trigger_led_start_time > UI_TRIGGER_LED_DURATION_MS) {
            g_trigger_led_on = false;
        }
    } else {
        if (last_led_state) {
            uint16_t led_bg_color = g_inverse_mode ? TFT_WHITE : TFT_BLACK;
            tft.fillCircle(UI_TRIGGER_LED_X, UI_TRIGGER_LED_Y, UI_TRIGGER_LED_RADIUS, led_bg_color);
            tft.drawCircle(UI_TRIGGER_LED_X, UI_TRIGGER_LED_Y, UI_TRIGGER_LED_RADIUS, TFT_DARKGREY);
            last_led_state = false;
        }
    }
}

void updateDisplay() {
    // Handle flash screen messages (RANDOM! / SNAPSHOT SAVED!)
    if (updateFlashScreens()) {
        return;
    }

    tft.setTextSize(1);
    uint16_t txt_color = g_inverse_mode ? TFT_BLACK : TFT_WHITE;
    uint16_t bg_color = g_inverse_mode ? TFT_WHITE : TFT_BLACK;
    uint16_t highlight_color = TFT_YELLOW;

    // Update Pot4 mode label highlighting
    updatePot4ModeLabels(txt_color, bg_color, highlight_color);

    tft.setTextColor(txt_color, bg_color);
    drawParameterBar(UI_COL1_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 0, g_params.position_q15, g_display_cache.position_q15, TFT_SKYBLUE);
    drawPitchBar(UI_COL2_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 0, g_params.pitch_f, g_display_cache.pitch_f, TFT_AQUA);
    drawParameterBar(UI_COL1_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 1, g_params.size_q15, g_display_cache.size_q15, TFT_SKYBLUE);
    drawParameterBar(UI_COL2_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 1, g_params.dryWet_q15, g_display_cache.dryWet_q15, TFT_LIGHTBLUE);
    drawParameterBar(UI_COL1_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 2, g_params.deja_vu_q15, g_display_cache.deja_vu_q15, TFT_SKYBLUE);
    // Note: Grain count moved to visualizer area
    drawParameterBar(UI_COL1_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 3, g_params.texture_q15, g_display_cache.texture_q15, TFT_AQUA);
    drawParameterBar(UI_COL2_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 3, g_params.stereoSpread_q15, g_display_cache.stereoSpread_q15, TFT_AQUA);
    drawParameterBar(UI_COL1_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 4, g_params.feedback_q15, g_display_cache.feedback_q15, TFT_AQUA);
    if (g_params.mode != g_display_cache.mode) {
        g_display_cache.mode = g_params.mode;
        tft.fillRect(UI_COL2_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 4, 60, 10, bg_color);
        tft.setCursor(UI_COL2_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 4 + 2);
        tft.print(getModeString(g_display_cache.mode));
    }
    if (g_current_resolution_index != g_display_cache.resolution_index) {
        g_display_cache.resolution_index = g_current_resolution_index;
        tft.fillRect(UI_COL1_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 5, 60, 10, bg_color);
        tft.setCursor(UI_COL1_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 5 + 2);
        tft.print(g_resolution_names[g_current_resolution_index]);
    }
    bool is_bt_connected = a2dp_sink.is_connected();
    if (is_bt_connected != g_display_cache.bt_connected) {
        g_display_cache.bt_connected = is_bt_connected;
        tft.fillRect(UI_COL2_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 5, 60, 10, bg_color);
        tft.setCursor(UI_COL2_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 5 + 2);
        tft.setTextColor(is_bt_connected ? TFT_BLUE : TFT_DARKGREY, bg_color);
        tft.print(is_bt_connected ? "CONN" : "----");
        tft.setTextColor(txt_color, bg_color);
    }
    if (g_params.loop_length != g_display_cache.loop_length) {
        g_display_cache.loop_length = g_params.loop_length;
        tft.fillRect(UI_COL1_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 6, 80, 10, bg_color);
        tft.setCursor(UI_COL1_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 6 + 2);
        tft.printf("%d steps", g_params.loop_length);
    }
    if(g_pot4_mode != g_display_cache.pot4_mode_for_text_update) {
        g_display_cache.pot4_mode_for_text_update = g_pot4_mode;
        tft.fillRect(UI_COL2_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 6, 60, 10, bg_color);
        tft.setCursor(UI_COL2_BAR_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * 6 + 2);
        tft.print(getPot4ModeString(g_pot4_mode));
    }
    // Compact BPM display (white background, black text)
    if (abs(g_current_bpm - g_display_cache.bpm) > 0.1f) {
        g_display_cache.bpm = g_current_bpm;
        tft.fillRect(5, VIZ_AREA_Y_START + 2, 80, 10, TFT_WHITE);
        tft.setTextColor(TFT_BLACK, TFT_WHITE);
        tft.setTextSize(1);
        tft.setCursor(5, VIZ_AREA_Y_START + 2);
        tft.printf("%.1fBPM", g_current_bpm);
    }

    // Grain count display (white background, black text)
    if (g_activeGrainCount != g_display_cache.active_grains) {
        g_display_cache.active_grains = g_activeGrainCount;
        tft.fillRect(240, VIZ_AREA_Y_START + 2, 75, 10, TFT_WHITE);
        tft.setTextColor(TFT_BLACK, TFT_WHITE);
        tft.setCursor(240, VIZ_AREA_Y_START + 2);
        tft.printf("%d/%dgrn", g_activeGrainCount, MAX_GRAINS);
    }

    // Draw particle visualizer
    drawParticleVisualizer();

    // Update trigger LED animation
    updateTriggerLED();
}

void drawUiFrame() {
    uint16_t bg_color = g_inverse_mode ? TFT_WHITE : TFT_BLACK;
    uint16_t text_color = g_inverse_mode ? TFT_BLACK : TFT_WHITE;
    constexpr int UI_SEPARATOR_Y_NEW = 95;
    uint16_t line_color = g_inverse_mode ?
    TFT_LIGHTGREY : TFT_DARKGREY;

    tft.fillScreen(bg_color);
    tft.setTextSize(1);
    tft.setTextColor(text_color, bg_color);
    const char* labels1[] = {"POS", "SIZ", "DEJA", "TEX", "FBK", "CLK", "LOOP"};
    const char* labels2[] = {"PIT", "MIX", "", "SPR", "MODE", "BT", "POT4"};
    for (int i = 0; i < 7; i++) {
        tft.setCursor(UI_COL1_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * i + 2);
        tft.print(labels1[i]);
        if (labels2[i] != "") {
            tft.setCursor(UI_COL2_LABEL_X, UI_PARAM_Y_START + UI_PARAM_Y_SPACING * i + 2);
            tft.print(labels2[i]);
        }
    }

    tft.drawLine(0, UI_SEPARATOR_Y_NEW, 320, UI_SEPARATOR_Y_NEW, line_color);
    tft.fillRect(0, UI_SEPARATOR_Y_NEW + 1, 320, 240 - (UI_SEPARATOR_Y_NEW + 1),
                 GET_VISUALIZER_BG_COLOR());
    tft.drawCircle(UI_TRIGGER_LED_X, UI_TRIGGER_LED_Y, UI_TRIGGER_LED_RADIUS, TFT_DARKGREY);

    // Draw visualizer separator line (directly below BPM display)
    tft.drawLine(0, VIZ_SEPARATOR_LINE_Y, 320, VIZ_SEPARATOR_LINE_Y, line_color);
}

void drawParameterBar(int x, int y, int16_t val, int16_t& lastVal, uint16_t color) {
    if(val == lastVal) return;
    uint16_t bg_color = g_inverse_mode ? TFT_WHITE : TFT_BLACK;
    uint16_t border_color = g_inverse_mode ? TFT_BLACK : TFT_WHITE;
    int fill_w = map(val, 0, 32767, 0, UI_BAR_WIDTH);
    tft.fillRect(x, y, UI_BAR_WIDTH, UI_BAR_HEIGHT, bg_color);
    tft.fillRect(x, y, fill_w, UI_BAR_HEIGHT, color);
    tft.drawRect(x, y, UI_BAR_WIDTH, UI_BAR_HEIGHT, border_color);

    tft.setTextColor(border_color, bg_color);
    tft.fillRect(x+UI_BAR_WIDTH+5, y, 30, UI_BAR_HEIGHT+2, bg_color);
    tft.setCursor(x+UI_BAR_WIDTH+5, y);
    tft.printf("%d%%", (int)map(val, 0, 32767, 0, 100));
    lastVal = val;
}

void drawPitchBar(int x, int y, float val, float& lastVal, uint16_t color) {
    // 非有限値・範囲外をガード
    if (!isfinite(val)) val = 0.0f;
    if (val >  PITCH_RANGE_SEMITONES_HALF) val =  PITCH_RANGE_SEMITONES_HALF;
    if (val < -PITCH_RANGE_SEMITONES_HALF) val = -PITCH_RANGE_SEMITONES_HALF;

    // lastVal 側も非有限なら強制更新扱い
    bool shouldUpdate = !isfinite(lastVal) ||
    (fabsf(val - lastVal) >= PITCH_CHANGE_THRESHOLD);
    if (!shouldUpdate) return;

    const int center_x = x + UI_BAR_WIDTH / 2;
    uint16_t bg_color = g_inverse_mode ? TFT_WHITE : TFT_BLACK;
    uint16_t border_color = g_inverse_mode ? TFT_BLACK : TFT_WHITE;
    // バー本体
    tft.fillRect(x, y, UI_BAR_WIDTH, UI_BAR_HEIGHT, bg_color);
    // map() を使わず自前で線形変換（0..halfRange → 0..UI_BAR_WIDTH/2）
    float ratio = fabsf(val) / PITCH_RANGE_SEMITONES_HALF;
    // 0..1
    if (ratio > 1.0f) ratio = 1.0f;
    int fill_w = (int)(ratio * (UI_BAR_WIDTH / 2));
    if (val >= 0) {
        tft.fillRect(center_x, y, fill_w, UI_BAR_HEIGHT, color);
    } else {
        tft.fillRect(center_x - fill_w, y, fill_w, UI_BAR_HEIGHT, color);
    }

    // 枠線・センターライン
    tft.drawRect(x, y, UI_BAR_WIDTH, UI_BAR_HEIGHT, border_color);
    tft.drawFastVLine(center_x, y, UI_BAR_HEIGHT,
                      g_inverse_mode ? TFT_LIGHTGREY : TFT_DARKGREY);
    // 文字表示（printfではなく drawFloat を使用）
    tft.setTextColor(border_color, bg_color);
    tft.fillRect(x + UI_BAR_WIDTH + 5, y, 50, UI_BAR_HEIGHT + 2, bg_color);
    // 第2引数=小数点以下桁数, 第3/4引数=表示位置
    tft.drawFloat(val, 1, x + UI_BAR_WIDTH + 5, y);

    lastVal = val;
}


// ================================================================= //
// SECTION: Particle Visualizer
// ================================================================= //
void drawParticleVisualizer() {
    static uint16_t last_write_pos = 0xFFFF;
    static bool buffer_bar_initialized = false;
    uint16_t bg_color = g_inverse_mode ? TFT_WHITE : TFT_BLACK;
    uint16_t fg_color = g_inverse_mode ? TFT_BLACK : TFT_WHITE;

    // Clear BPM/Grain display area to prevent particle color residue
    tft.fillRect(0, VIZ_AREA_Y_START, 320, VIZ_INFO_HEIGHT, TFT_BLACK);

    // Redraw BPM and grain count every frame (to prevent particle residue)
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(5, VIZ_AREA_Y_START + 2);
    tft.printf("%.1fBPM", g_current_bpm);
    tft.setCursor(240, VIZ_AREA_Y_START + 2);
    tft.printf("%d/%dgrn", g_activeGrainCount, MAX_GRAINS);

    // Redraw separator line every frame
    uint16_t line_color = g_inverse_mode ? TFT_LIGHTGREY : TFT_DARKGREY;
    tft.drawLine(0, VIZ_SEPARATOR_LINE_Y, 320, VIZ_SEPARATOR_LINE_Y, line_color);

    // Calculate pitch scale positions (used for initial draw and particle positioning)
    int y_center = VIZ_PARTICLE_Y_START + (VIZ_PARTICLE_HEIGHT / 2);
    int y_top = VIZ_PARTICLE_Y_START;
    int y_bottom = VIZ_PARTICLE_Y_START + VIZ_PARTICLE_HEIGHT - 1;

    // Initialize particle area once (first frame only)
    static bool particle_area_initialized = false;
    if (!particle_area_initialized) {
        // Clear entire particle area initially
        int clear_y_start = VIZ_SEPARATOR_LINE_Y + 1;
        int clear_height = VIZ_BUFFER_BAR_AREA_Y - clear_y_start - 1;
        tft.fillRect(0, clear_y_start, 320, clear_height, TFT_BLACK);

        // Draw center reference line (pitch = 0) as dashed line
        for (int x = 0; x < 320; x += 8) {
            tft.drawLine(x, y_center, x + 4, y_center, TFT_LIGHTGREY);
        }

        // Draw pitch scale labels (only once)
        tft.setTextSize(1);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);

        // Left side labels
        tft.setCursor(2, y_top);
        tft.print("+24");
        tft.setCursor(2, y_center - 4);
        tft.print(" 0");
        tft.setCursor(2, y_bottom - 8);
        tft.print("-24");

        // Right side labels
        tft.setCursor(302, y_top);
        tft.print("+24");
        tft.setCursor(302, y_center - 4);
        tft.print(" 0");
        tft.setCursor(302, y_bottom - 8);
        tft.print("-24");

        particle_area_initialized = true;
    }

    // Trail effect: store previous particle positions
    struct ParticleTrail {
        int x, y, radius;
        uint16_t color;
        bool valid;
    };
    static ParticleTrail trails[MAX_GRAINS] = {};

    // Clear previous particle positions only (using trail data)
    for (uint8_t i = 0; i < MAX_GRAINS; i++) {
        if (trails[i].valid) {
            // Clear the old particle by filling with black
            tft.fillCircle(trails[i].x, trails[i].y, trails[i].radius, TFT_BLACK);
        }
    }

    // Draw enhanced buffer progress bar at bottom
    if (!buffer_bar_initialized || last_write_pos != g_grainWritePos) {
        // Clear buffer bar area with black background
        tft.fillRect(0, VIZ_BUFFER_BAR_AREA_Y, 320, 48, TFT_BLACK);

        // Draw scale markers (0%, 25%, 50%, 75%, 100%) - white text on black
        tft.setTextSize(1);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setCursor(0, VIZ_BUFFER_BAR_AREA_Y);
        tft.print("0");
        tft.setCursor(75, VIZ_BUFFER_BAR_AREA_Y);
        tft.print("25");
        tft.setCursor(155, VIZ_BUFFER_BAR_AREA_Y);
        tft.print("50");
        tft.setCursor(235, VIZ_BUFFER_BAR_AREA_Y);
        tft.print("75");
        tft.setCursor(302, VIZ_BUFFER_BAR_AREA_Y);
        tft.print("100%");

        int bar_y = VIZ_BUFFER_BAR_AREA_Y + 8;

        // Draw segmented buffer bar (battery-style with purple segments)
        constexpr int SEGMENT_COUNT = 32;
        constexpr int SEGMENT_WIDTH = 9;
        constexpr int SEGMENT_GAP = 1;
        constexpr int SEGMENT_TOTAL_WIDTH = SEGMENT_WIDTH + SEGMENT_GAP;

        // Calculate how many segments to fill based on buffer progress
        int filled_segments = (g_grainWritePos * SEGMENT_COUNT) / GRAIN_BUFFER_SIZE;

        // Draw each segment
        for (int i = 0; i < SEGMENT_COUNT; i++) {
            int seg_x = i * SEGMENT_TOTAL_WIDTH;

            if (i < filled_segments) {
                // Filled segment (purple)
                tft.fillRect(seg_x, bar_y, SEGMENT_WIDTH, VIZ_BUFFER_BAR_HEIGHT, TFT_PURPLE);
            } else {
                // Empty segment (light gray on white background)
                tft.fillRect(seg_x, bar_y, SEGMENT_WIDTH, VIZ_BUFFER_BAR_HEIGHT, TFT_LIGHTGREY);
            }
        }

        // Draw current write position marker (red line)
        int x_pos = (g_grainWritePos * (SEGMENT_COUNT * SEGMENT_TOTAL_WIDTH)) / GRAIN_BUFFER_SIZE;
        tft.fillRect(x_pos - 1, bar_y, 2, VIZ_BUFFER_BAR_HEIGHT, TFT_RED);

        // Draw tick marks at 25% intervals (white on black)
        for (int i = 0; i <= 4; i++) {
            int tick_x = (i * SEGMENT_COUNT * SEGMENT_TOTAL_WIDTH) / 4;
            tft.drawFastVLine(tick_x, bar_y - 2, 2, TFT_WHITE);
        }

        // Draw border around entire bar area
        tft.drawRect(0, bar_y, SEGMENT_COUNT * SEGMENT_TOTAL_WIDTH, VIZ_BUFFER_BAR_HEIGHT, TFT_WHITE);

        // Draw buffer info text (32768 samples / ~743ms) - white text on black background
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setCursor(80, VIZ_BUFFER_BAR_AREA_Y + VIZ_BUFFER_BAR_HEIGHT + 11);
        tft.print("Buf:32768smp/743ms");

        last_write_pos = g_grainWritePos;
        buffer_bar_initialized = true;
    }

    // Draw current particles and update trails
    for (uint8_t i = 0; i < g_activeGrainCount; i++) {
        uint8_t grain_idx = g_activeGrainIndices[i];
        Grain& grain = g_grains[grain_idx];

        if (!grain.active) continue;

        // Calculate X position (buffer position: 20-300 to avoid scale labels)
        uint16_t current_pos = grain.position_q16 >> 16;
        uint16_t buffer_pos = (grain.startPos + current_pos) & GRAIN_BUFFER_MASK;
        constexpr int VIZ_PARTICLE_X_MIN = 20;
        constexpr int VIZ_PARTICLE_X_MAX = 300;
        constexpr int VIZ_PARTICLE_X_RANGE = VIZ_PARTICLE_X_MAX - VIZ_PARTICLE_X_MIN;
        int x = VIZ_PARTICLE_X_MIN + (buffer_pos * VIZ_PARTICLE_X_RANGE) / GRAIN_BUFFER_SIZE;

        // Calculate particle size (envelope progress) - calculate first
        float progress = (float)current_pos / grain.length;
        // Use Hann window for size (larger in middle, smaller at edges)
        float envelope = 0.5f * (1.0f - cosf(2.0f * PI * progress));
        int size = VIZ_PARTICLE_MIN_SIZE + (int)(envelope * (VIZ_PARTICLE_MAX_SIZE - VIZ_PARTICLE_MIN_SIZE));
        size = constrain(size, VIZ_PARTICLE_MIN_SIZE, VIZ_PARTICLE_MAX_SIZE);
        int particle_radius = size / 2;

        // Calculate Y position (pitch: -24 to +24 semitones mapped to Y axis)
        // Add margin to align particles with label centers (text is 8px tall, center offset is 4px)
        // pitch_f = +24 -> near top (y=119, aligns with +24 label center)
        // pitch_f = 0   -> center (y=163, aligns with 0 label center)
        // pitch_f = -24 -> near bottom (y=207, aligns with -24 label center)
        constexpr int LABEL_MARGIN = 4;  // Margin to align with label centers
        int y_center = VIZ_PARTICLE_Y_START + (VIZ_PARTICLE_HEIGHT / 2);
        int effective_half_range = (VIZ_PARTICLE_HEIGHT / 2) - LABEL_MARGIN;  // 48 - 4 = 44
        int y = y_center - (int)((grain.pitch_f / 24.0f) * effective_half_range);
        // Keep particle within bounds
        y = constrain(y, VIZ_PARTICLE_Y_START, VIZ_PARTICLE_Y_START + VIZ_PARTICLE_HEIGHT - 1);

        // Calculate color (progress-based gradient)
        uint16_t color;
        if (progress < 0.33f) {
            // Start: Cyan to Yellow
            color = TFT_CYAN;
        } else if (progress < 0.66f) {
            // Middle: Yellow to Magenta
            color = TFT_YELLOW;
        } else {
            // End: Magenta to Red
            color = TFT_MAGENTA;
        }

        // Draw particle (filled circle)
        tft.fillCircle(x, y, particle_radius, color);

        // Save current position as trail for next frame
        trails[grain_idx].x = x;
        trails[grain_idx].y = y;
        trails[grain_idx].radius = particle_radius;
        trails[grain_idx].color = color;
        trails[grain_idx].valid = true;
    }

    // Invalidate trails for inactive grains
    for (uint8_t i = 0; i < MAX_GRAINS; i++) {
        bool is_active = false;
        for (uint8_t j = 0; j < g_activeGrainCount; j++) {
            if (g_activeGrainIndices[j] == i) {
                is_active = true;
                break;
            }
        }
        if (!is_active) {
            trails[i].valid = false;
        }
    }
}

// ================================================================= //
// SECTION: Initialization & Helpers
// ================================================================= //

// Initialize window LUT (Hann window squared for grain envelope)
void initWindowLut() {
    for(int i=0; i<WINDOW_LUT_SIZE; i++) {
        float t = (float)i / (WINDOW_LUT_SIZE - 1);
        float w = 0.5f * (1.0f - cosf(2.0f * PI * t));
        g_window_lut_q15[i] = (int16_t)((w * w) * 32767.0f);
    }
}

// Initialize pitch LUT (exponential pitch shift values)
void initPitchLut() {
    for(int i=0; i<PITCH_LUT_SIZE; i++) {
        float s = ((float)i / (PITCH_LUT_SIZE - 1)) * PITCH_RANGE_SEMITONES - PITCH_RANGE_SEMITONES_HALF;
        g_pitch_lut_q16[i] = (int32_t)(exp2f(s / 12.0f) * 65536.0f);
    }
}

// Initialize pan LUT (sine curve for equal-power panning)
void initPanLut() {
    for(int i=0; i<PAN_LUT_SIZE; i++) {
        float a = ((float)i / (PAN_LUT_SIZE - 1)) * (PI * 0.5f);
        g_pan_lut_q15[i] = (int16_t)(sinf(a) * 32767.0f);
    }
}

// Initialize mix LUT (linear dry/wet mix values)
void initMixLut() {
    for(int i=0; i<MIX_LUT_SIZE; i++) {
        g_mix_lut_q15[i] = (int16_t)((i * 32767L) / (MIX_LUT_SIZE - 1));
    }
}

// Initialize feedback LUT (scaled feedback values)
void initFeedbackLut() {
    for(int i=0; i<FEEDBACK_LUT_SIZE; i++) {
        float f = FEEDBACK_LUT_MIN + ((float)i / (FEEDBACK_LUT_SIZE - 1)) * FEEDBACK_LUT_RANGE;
        g_feedback_lut_q15[i] = (int16_t)(f * 32767.0f);
    }
}

// Initialize reciprocal LUT (fast division for grain processing)
void initReciprocalLut() {
    for(int i=0; i<RECIPROCAL_LUT_SIZE; i++) {
        uint16_t l = MIN_GRAIN_SIZE + ((MAX_GRAIN_SIZE - MIN_GRAIN_SIZE) * i) / (RECIPROCAL_LUT_SIZE - 1);
        g_reciprocal_lut_q32[i] = (l > 0) ? (uint32_t)(((1ULL << 32) - 1) / l) : 0;
    }
}

// Initialize random pan LUT (pre-generated random panning values)
void initRandomPanLut() {
    for(int i=0; i<RANDOM_PAN_LUT_SIZE; i++) {
        g_random_pan_lut[i] = ((esp_random() % 20001) / 10000.0f) - 1.0f;
    }
}

// Initialize random LUT (pre-generated random values)
void initRandomLut() {
    for(int i=0; i<RANDOM_LUT_SIZE; i++) {
        g_random_lut_q15[i] = (esp_random() % 65535) - 32767;
    }
}

// Initialize all lookup tables
void initAllLuts() {
    initWindowLut();
    initPitchLut();
    initPanLut();
    initMixLut();
    initFeedbackLut();
    initReciprocalLut();
    initRandomPanLut();
    initRandomLut();
}

const char* getModeString(PlayMode m) {
    return (m == MODE_GRANULAR) ? "GRAN" : "REV ";
}

const char* getPot4ModeString(Pot4Mode m) {
    switch(m) {
        case MODE_TEXTURE:     return "TEX";
        case MODE_SPREAD:      return "SPR";
        case MODE_FEEDBACK:    return "FBK";
        case MODE_LOOP_LENGTH: return "LEN";
        case MODE_CLK_RESOLUTION: return "CLK";
        default:               return "---";
    }
}