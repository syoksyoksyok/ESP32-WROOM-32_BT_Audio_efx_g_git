# Bluetooth A2DP受信とDAC出力の実装仕様書

## 概要
本文書は、ESP32-WROOM-32を使用したBluetooth A2DP (Advanced Audio Distribution Profile) オーディオレシーバーの実装仕様をまとめたものです。Raspberry Pi Pico 2 Wでの実装時の参考資料として、グラニュラーエフェクトを除いた基本的な音声受信・再生の仕組みを説明します。

---

## 1. システム構成

### 1.1 ハードウェア構成
```
[スマートフォン/PC]
        ↓ (Bluetooth A2DP)
   [ESP32-WROOM-32]
        ↓ (I2S)
     [I2S DAC]
        ↓
   [スピーカー/アンプ]
```

### 1.2 使用DAC
- **I2S対応DAC** (推奨チップ: PCM5102, PCM5102A, UDA1334, MAX98357A等)
- **接続方式**: I2S (Inter-IC Sound) デジタルオーディオインターフェース
- **特徴**: 3線式接続、高音質、ノイズに強い

---

## 2. Bluetooth A2DP受信の実装

### 2.1 使用ライブラリ
- **ESP32-A2DP** (Phil Schatzmann氏作)
- リポジトリ: https://github.com/pschatzmann/ESP32-A2DP.git
- 機能: ESP32でBluetooth A2DPシンク(受信機)を簡単に実装可能

### 2.2 実装コード (基本構造)

```cpp
#include "BluetoothA2DPSink.h"

BluetoothA2DPSink a2dp_sink;

// Bluetoothから音声データを受信するコールバック関数
void a2dp_data_callback(const uint8_t *data, uint32_t length) {
    // data: 16-bit ステレオPCMデータ (LRLRLR... の順)
    // length: データサイズ (バイト単位)

    int16_t* samples = (int16_t*)data;

    // サンプル数 = length / 4
    // (1サンプル = 左2バイト + 右2バイト = 4バイト)
    for(uint32_t i = 0; i < length/4; i++) {
        int16_t left  = samples[i*2];      // 左チャンネル
        int16_t right = samples[i*2 + 1];  // 右チャンネル

        // ここで音声データを処理
        // 例: バッファに書き込み、エフェクト処理など
    }
}

void setup() {
    // コールバック関数を登録
    a2dp_sink.set_stream_reader(a2dp_data_callback, false);

    // Bluetoothデバイスとして起動 (デバイス名: "ESP32-Audio")
    a2dp_sink.start("ESP32-Audio");
}
```

### 2.3 受信データフォーマット
- **フォーマット**: 16-bit リニアPCM
- **チャンネル**: ステレオ (Left/Right)
- **サンプリングレート**: 44.1kHz (A2DP標準)
- **バイト順**: リトルエンディアン
- **データ配列**: [L0_low, L0_high, R0_low, R0_high, L1_low, L1_high, R1_high, ...]

### 2.4 ステレオ→モノラル変換 (オプション)
```cpp
// 左右チャンネルを平均化してモノラルに変換
int16_t mono = (samples[i*2] >> 1) + (samples[i*2+1] >> 1);
```

---

## 3. I2S DAC出力の実装

### 3.1 I2S ピン配置 (ESP32)

| 信号名 | GPIO番号 | DAC側ピン名 | 説明 |
|--------|----------|-------------|------|
| BCLK   | GPIO 14  | BCK/SCK     | ビットクロック |
| LRC    | GPIO 15  | LRCK/WS     | Left/Right Clock (ワードセレクト) |
| DOUT   | GPIO 13  | DIN/SD      | データ出力 |
| -      | GND      | GND         | グランド |
| -      | 3.3V     | VIN/VCC     | 電源 (DACによる) |

**注意**: Raspberry Pi Pico 2 Wでは異なるGPIOピンを使用する必要があります。

### 3.2 I2S設定パラメータ

```cpp
#include "driver/i2s.h"

constexpr int I2S_OUT_BCLK = 14;  // ビットクロック
constexpr int I2S_OUT_LRC  = 15;  // ワードセレクト
constexpr int I2S_OUT_DOUT = 13;  // データ出力

void setupI2S() {
    // I2S設定構造体
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 44100,                           // 44.1kHz
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,  // 16bit
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // ステレオ
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,  // 標準I2S
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,                            // DMAバッファ数
        .dma_buf_len = 128,                            // 1バッファあたりのサンプル数
        .use_apll = false,                             // APLLクロック不使用
        .tx_desc_auto_clear = true                     // バッファ自動クリア
    };

    // ピン設定構造体
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_OUT_BCLK,
        .ws_io_num = I2S_OUT_LRC,
        .data_out_num = I2S_OUT_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE  // 入力なし
    };

    // I2Sドライバーインストール
    i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);

    // ピン設定適用
    i2s_set_pin(I2S_NUM_1, &pin_config);
}
```

### 3.3 I2S設定パラメータ詳細

| パラメータ | 値 | 説明 |
|------------|-----|------|
| サンプリングレート | 44100 Hz | CD品質の標準サンプリングレート |
| ビット深度 | 16-bit | サンプルあたりのビット数 |
| チャンネル | ステレオ | Left + Right |
| 通信フォーマット | フィリップスI2S | 標準I2Sプロトコル |
| DMAバッファ数 | 8 | 割り込み処理用のバッファ数 |
| DMAバッファ長 | 128サンプル | 1バッファあたりのサンプル数 |
| モード | MASTER + TX | ESP32がマスタークロック生成、送信のみ |

### 3.4 I2Sデータ出力

```cpp
// ステレオサンプルバッファ (128サンプル = 256要素)
int16_t i2s_buffer[128 * 2];  // Left, Right, Left, Right...

// I2Sへデータ書き込み
size_t bytes_written;
i2s_write(I2S_NUM_1, i2s_buffer, sizeof(i2s_buffer), &bytes_written, portMAX_DELAY);
```

---

## 4. シンプルな音声データフロー (エフェクトなし)

### 4.1 基本フロー図
```
[Bluetooth A2DP受信]
        ↓
  a2dp_data_callback()
  - 16-bit ステレオPCM受信
        ↓
  [リングバッファ]
  - タスク間データ受け渡し
  - 4096サンプル容量
        ↓
  [オーディオ処理タスク]
  - バッファから読み出し
  - 必要に応じて処理
        ↓
  [I2Sバッファ準備]
  - ステレオ形式に変換
  - 128サンプル × ステレオ
        ↓
  i2s_write()
        ↓
  [I2S DAC出力]
        ↓
  [スピーカー]
```

### 4.2 実装例 (シンプル版)

```cpp
#include "BluetoothA2DPSink.h"
#include "driver/i2s.h"

// グローバル変数
BluetoothA2DPSink a2dp_sink;
QueueHandle_t audio_queue;

// A2DPコールバック: Bluetoothから受信
void a2dp_data_callback(const uint8_t *data, uint32_t length) {
    int16_t* samples = (int16_t*)data;
    uint32_t sample_count = length / 4;  // ステレオサンプル数

    // キューにデータを送信
    for(uint32_t i = 0; i < sample_count; i++) {
        int16_t left = samples[i*2];
        int16_t right = samples[i*2 + 1];

        // ステレオペアをキューに送信
        int16_t stereo[2] = {left, right};
        xQueueSend(audio_queue, stereo, 0);
    }
}

// オーディオ出力タスク
void audioTask(void* param) {
    int16_t i2s_buffer[128 * 2];  // 128サンプル × ステレオ

    while(1) {
        // キューからデータを読み出してバッファに詰める
        for(int i = 0; i < 128; i++) {
            int16_t stereo[2];
            if(xQueueReceive(audio_queue, stereo, portMAX_DELAY)) {
                i2s_buffer[i*2] = stereo[0];      // Left
                i2s_buffer[i*2 + 1] = stereo[1];  // Right
            }
        }

        // I2Sに出力
        size_t bytes_written;
        i2s_write(I2S_NUM_1, i2s_buffer, sizeof(i2s_buffer),
                  &bytes_written, portMAX_DELAY);
    }
}

void setup() {
    // キュー作成 (4096サンプル分)
    audio_queue = xQueueCreate(4096, sizeof(int16_t) * 2);

    // I2S初期化
    setupI2S();

    // オーディオタスク起動 (Core 1で実行)
    xTaskCreatePinnedToCore(audioTask, "Audio", 4096, NULL, 5, NULL, 1);

    // Bluetooth A2DP起動
    a2dp_sink.set_stream_reader(a2dp_data_callback, false);
    a2dp_sink.start("ESP32-Audio");
}

void loop() {
    delay(1000);
}
```

---

## 5. タスク構成とマルチコア活用 (ESP32)

### 5.1 デュアルコア構成
ESP32はデュアルコアプロセッサ (PRO_CPU / APP_CPU) を搭載しています。

```
Core 0 (PRO_CPU):
- setup()、loop()実行
- UI処理 (ディスプレイ更新など)
- パラメータ調整
- 低優先度タスク

Core 1 (APP_CPU):
- オーディオ処理タスク
- リアルタイム処理
- 高優先度タスク
- 割り込み処理
```

### 5.2 タスク間通信
- **FreeRTOS Queue**: タスク間のデータ受け渡し
- **リングバッファ**: 高速なFIFOバッファ
- **セマフォ**: タスク同期

---

## 6. Raspberry Pi Pico 2 Wでの実装時の注意点

### 6.1 主な違い

| 項目 | ESP32-WROOM-32 | Raspberry Pi Pico 2 W |
|------|----------------|------------------------|
| CPU | Xtensa LX6 デュアルコア 240MHz | ARM Cortex-M33 デュアルコア 150MHz |
| RAM | 520 KB SRAM | 520 KB SRAM |
| Bluetooth | 内蔵 (Classic + BLE) | **CYW43439内蔵 (Classic + BLE)** |
| I2S | ハードウェアI2S | **PIOでエミュレート必要** |
| OS | FreeRTOS | SDK標準、またはFreeRTOS |

### 6.2 Raspberry Pi Pico 2 Wでの課題

#### 6.2.1 Bluetooth接続
- **状況**: Pico 2 WはCYW43439チップを搭載しており、Bluetooth Classic (5.2) に対応
- **課題**: Pico SDKでのBluetooth Classicサポートは限定的 (主にBLE向け)
- **解決策**:
  1. **BTstack ライブラリを使用** (推奨)
     - BlueKitchen BTstackはCYW43439のBluetooth Classic/BLEに対応
     - A2DP Sinkプロファイルの実装例あり
     - pico-sdkとの統合サンプルあり
     - 参考: https://github.com/bluekitchen/btstack
  2. **外付けBluetoothモジュール使用** (簡易的)
     - 例: RN52 (A2DP専用モジュール)
     - UARTまたはI2Sで接続
  3. **ESP32をBluetoothブリッジとして使用** (確実だが複雑)

#### 6.2.2 I2S出力
- **問題**: Pico 2 WにはハードウェアI2Sペリフェラルがない
- **解決策**:
  1. **PIO (Programmable I/O)** でI2Sをエミュレート
  2. Pico SDKのI2S PIOサンプルコードを参考にする
  3. DMAを併用して高速転送

#### 6.2.3 参考ライブラリ (Pico用)
- **pico-extras**: I2S PIO実装サンプル
  - https://github.com/raspberrypi/pico-extras
- **pico-playground**: 各種オーディオサンプル
  - https://github.com/raspberrypi/pico-playground

### 6.3 推奨実装パス

#### パス1: BTstackでPico単体実装 (推奨)
```
[スマホ] → BT A2DP → [Pico 2 W (BTstack + PIO I2S)] → [DAC]
```
- **メリット**: 最小構成、ハードウェアはPico 2 W + DAC のみ
- **デメリット**: 実装難易度がやや高い
- **実装ステップ**:
  1. BTstackをpico-sdkに統合
  2. A2DP Sinkプロファイルのサンプルコードをベースに実装
  3. PIOでI2S出力を実装 (pico-extras参考)
  4. 受信したPCMデータをI2S DACに出力
- **参考資料**:
  - BTstack Pico examples: https://github.com/bluekitchen/btstack/tree/master/port/raspberry-pi-pico
  - A2DP Sink example: https://github.com/bluekitchen/btstack/blob/master/example/a2dp_sink_demo.c

#### パス2: 外付けBluetoothモジュール使用 (簡易)
```
[スマホ] → BT A2DP → [RN52モジュール] → I2S → [Pico 2 W] → 処理 → I2S → [DAC]
```
- **メリット**: Bluetooth実装が不要、動作確実
- **デメリット**: 追加ハードウェア必要、モジュール制約あり
- **推奨モジュール**: RN52 (A2DP専用、I2S出力対応)

#### パス3: ESP32をBluetoothレシーバーとして使用
```
[スマホ] → BT A2DP → [ESP32] → I2S → [Pico 2 W] → 処理 → I2S → [DAC]
```
- **メリット**: A2DP受信が確実に動作 (本プロジェクトと同じ)
- **デメリット**: 2つのマイコンが必要、コスト増

---

## 7. DAC接続の実例 (PCM5102A)

### 7.1 PCM5102A ピン配置

```
PCM5102A DAC モジュール (ブレイクアウトボード)

VCC  ← 3.3V または 5V (モジュールによる)
GND  ← GND
SCK  ← GPIO 14 (BCLK)
BCK  ← GPIO 14 (BCLK) *SCKと同じ
DIN  ← GPIO 13 (DOUT)
LCK  ← GPIO 15 (LRC)
GND  ← GND (複数あり)
FLT  ← GND (フィルター選択: GNDでノーマル)
DEMP ← GND (ディエンファシス: GNDで無効)
XSMT ← 3.3V (ミュート解除)
FMT  ← GND (I2Sフォーマット選択)
```

### 7.2 最小接続 (5線)
```
ESP32 GPIO 14 → PCM5102A SCK/BCK
ESP32 GPIO 15 → PCM5102A LCK
ESP32 GPIO 13 → PCM5102A DIN
ESP32 3.3V    → PCM5102A VCC
ESP32 GND     → PCM5102A GND
```

### 7.3 音質改善のための追加接続
```
PCM5102A FLT  → GND (ノーマルフィルター)
PCM5102A DEMP → GND (ディエンファシス無効)
PCM5102A XSMT → 3.3V (ミュート解除)
PCM5102A FMT  → GND (I2Sフォーマット)
```

### 7.4 出力
```
PCM5102A OUTL → 左チャンネル出力 (アンプまたはスピーカー)
PCM5102A OUTR → 右チャンネル出力
PCM5102A AGND → オーディオグランド
```

---

## 8. トラブルシューティング

### 8.1 音が出ない場合

#### チェックリスト:
1. **Bluetooth接続確認**
   - `a2dp_sink.start()` が実行されているか
   - スマホ側でペアリング・接続されているか
   - コールバック関数が呼ばれているか (Serial.print()で確認)

2. **I2S配線確認**
   - BCLK, LRC, DINの配線が正しいか
   - DACの電源 (VCC, GND) が供給されているか
   - グランドが共通接続されているか

3. **I2S設定確認**
   - サンプリングレートが44100Hzか
   - チャンネルフォーマットがステレオか
   - `i2s_write()` が実行されているか

4. **DAC設定確認**
   - PCM5102AのXSMTピンが3.3V (ミュート解除)
   - FMTピンがGND (I2Sモード)

### 8.2 音が途切れる場合

#### 原因:
- バッファアンダーラン (データ供給が間に合わない)
- CPU負荷が高すぎる

#### 対策:
1. DMAバッファを増やす (`dma_buf_count`, `dma_buf_len`)
2. オーディオタスクの優先度を上げる
3. Core 1をオーディオ専用にする
4. 他の処理を軽量化

### 8.3 ノイズが乗る場合

#### 対策:
1. DACの電源を安定化 (デカップリングコンデンサ追加)
2. グランドプレーンを広く取る
3. I2S信号線を短くする
4. 電源ラインとオーディオラインを分離

---

## 9. 参考資料

### 9.1 ライブラリ
- ESP32-A2DP: https://github.com/pschatzmann/ESP32-A2DP
- ESP32 I2S Driver: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html

### 9.2 DAC データシート
- PCM5102A: https://www.ti.com/product/PCM5102A
- UDA1334A: https://www.nxp.com/docs/en/data-sheet/UDA1334ATS.pdf
- MAX98357A: https://www.analog.com/media/en/technical-documentation/data-sheets/MAX98357A-MAX98357B.pdf

### 9.3 I2Sプロトコル
- Philips I2S Specification: https://www.sparkfun.com/datasheets/BreakoutBoards/I2SBUS.pdf

### 9.4 Raspberry Pi Pico
- Pico SDK: https://github.com/raspberrypi/pico-sdk
- Pico PIO I2S: https://github.com/raspberrypi/pico-extras/tree/master/src/rp2_common/pico_audio_i2s
- BTstack (Bluetooth Stack): https://github.com/bluekitchen/btstack
- BTstack Pico Port: https://github.com/bluekitchen/btstack/tree/master/port/raspberry-pi-pico
- BTstack A2DP Sink Example: https://github.com/bluekitchen/btstack/blob/master/example/a2dp_sink_demo.c

---

## 10. まとめ

### 10.1 ESP32での実装 (本プロジェクト)
✅ **動作確認済み**
- Bluetooth A2DP受信: ESP32-A2DPライブラリで簡単実装
- I2S DAC出力: ハードウェアI2Sで高音質・低CPU負荷
- デュアルコアでリアルタイム処理可能

### 10.2 Raspberry Pi Pico 2 Wでの実装
✅ **実装可能** (CYW43439がBluetooth Classic対応)
- **Bluetooth A2DP**: BTstackライブラリで実装可能 (サンプルコードあり)
- **I2S出力**: PIOでエミュレート (pico-extrasに実装例あり)
- **総合難易度**: ESP32より高いが、十分実現可能

### 10.3 推奨アプローチ (Pico 2 W向け)

#### 初心者向け:
1. **まずPIOでI2S出力のみテスト** (WAVファイル再生など)
2. **外付けBluetoothモジュール (RN52) で動作確認**
3. **動作確認後、BTstackへ移行を検討**

#### 中級者以上向け:
1. **BTstack + PIO I2Sで単体実装にチャレンジ**
2. **BTstackのA2DP Sinkサンプルをベースに開発**
3. **pico-extrasのI2Sサンプルと統合**

#### 確実に動作させたい場合:
- **ESP32との2段構成** (本プロジェクトのESP32コードをそのまま活用)

---

**作成日**: 2025-11-17
**対象プロジェクト**: ESP32-WROOM-32_BT_Audio_efx_g
**対象デバイス**: ESP32-WROOM-32 / Raspberry Pi Pico 2 W
**DAC**: PCM5102A (I2S)
