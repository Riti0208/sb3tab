# ScratchESP

[Scratch](https://scratch.mit.edu/) プロジェクト（`.sb3`）を ESP32 マイコン上でネイティブ実行 — ブラウザもPCもインターネットも不要。

[ScratchEverywhere](https://github.com/ScratchEverywhere/ScratchEverywhere)（クロスプラットフォーム Scratch ランタイム）をベースにしています。

## 対応機能

| 機能 | 状態 |
|---|---|
| スプライト描画（PNG/SVG） | ✅ |
| ステージ背景 | ✅ |
| 吹き出し（say/think） | ✅ 日本語テキスト対応 |
| ペン描画 | ✅ 線・ドット・スタンプ・クリア |
| 変数/リストモニター | ✅ |
| 明るさエフェクト | ✅ |
| 幽霊エフェクト | ✅ |
| 音声再生（WAV） | ✅ |
| 音声ピッチ/パン | ✅ |
| クローン | ✅ |
| 動き/制御/演算/データブロック | ✅ |

### 未実装
- 画像エフェクト: COLOR, FISHEYE, WHIRL, PIXELATE, MOSAIC（上流の ScratchEverywhere にもなし）
- 色接触判定（上流にもなし）
- MP3/Ogg 音声（WAV PCM のみ）
- 入力（キーボード/マウス/タッチ — ハードウェアが必要）
- Text-to-Speech（WiFi が必要）

## ハードウェア

### 動作確認済み
- **ESP32-P4**（M5Stamp P4）+ SPI LCD（ILI9341, 320x240） — 10-13 fps
- **ESP32-S3**（XIAO ESP32-S3 Sense）+ SPI LCD — 完動

### 予定
- **M5Stack Tab5**（ESP32-P4 + 5インチ 1280x720 MIPI DSI） — 30fps以上を目標

### 音声（オプション）
- MAX98357A I2S DAC モジュール
- I2S ピン: BCLK=GPIO5, LRCLK=GPIO6, DOUT=GPIO8

## ビルド

### 必要なもの
- [ESP-IDF v5.4.1](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32p4/get-started/)
- ESP32-P4 ターゲット（ESP32-S3 でも若干の変更で動作可能）

### 手順

```bash
# ターゲット設定
idf.py set-target esp32p4

# ビルド
idf.py build

# 書き込み
idf.py -p /dev/tty.usbmodem1101 flash
```

> **注意:** `set-target` 後は `sdkconfig` の設定がリセットされることがあります:
> `rm sdkconfig && idf.py reconfigure`

## 使い方

### 1. Scratch プロジェクトを送信

```bash
# scratch.mit.edu からダウンロードした .sb3 ファイルを送信
python3 -u tools/send_sb3.py <project.sb3 または URL> /dev/tty.usbmodem1101
```

ホスト側で `.sb3` を展開し（P4 の RISC-V では zip 解凍がクラッシュするため）、`project.json` とアセットを USB シリアル経由で個別に送信します。

### 2. 実行を確認

プロジェクトは即座に ESP32 上で実行開始されます。スプライトは内部で 480x360 にレンダリングされ、LCD の解像度にスケーリングして表示されます。

## アーキテクチャ

```
┌──────────────────────────────────┐
│  ホスト PC                        │
│  tools/send_sb3.py               │
│  (.sb3 を展開し USB で送信)        │
└──────────┬───────────────────────┘
           │ USB Serial (LL API)
┌──────────▼───────────────────────┐
│  ESP32-P4                        │
│                                  │
│  ┌─────────────────────────────┐ │
│  │ scratch_core (component)    │ │
│  │  - ランタイム（ブロック実行） │ │
│  │  - パーサ（JSON → スプライト）│ │
│  │  - ヘッドレスレンダラー       │ │
│  │  - SpeechManager (TTF)      │ │
│  │  - 音声 (I2S + WAV)         │ │
│  └─────────────┬───────────────┘ │
│                │                 │
│  ┌─────────────▼───────────────┐ │
│  │ main/                       │ │
│  │  - SWRenderer (480x360)     │ │
│  │  - LCD ドライバ (SPI/DSI)    │ │
│  │  - USB プロトコルハンドラ     │ │
│  └─────────────────────────────┘ │
└──────────────────────────────────┘
```

### レンダリングパイプライン
1. フレームバッファをクリア（白）
2. ステージ背景を描画
3. ペンレイヤーを合成（RGBA8888）
4. スプライトを回転・スケーリング・アルファ付きで描画
5. 吹き出しを描画（stb_truetype + NotoSansJP）
6. 変数/リストモニターを描画
7. LCD にスケーリング＆転送

### 主要な技術的判断
- **stb_truetype** でフォントレンダリング（NotoSansJP サブセット、331KB、フラッシュ埋め込み）
- **nanosvg** で SVG ラスタライゼーション
- **USB LL API** を使用（USB ドライバは VFS と競合するため）
- **ホスト側で .sb3 展開**（miniz が P4 の RISC-V でクラッシュするため）
- **ペンコールバック方式** で scratch_core コンポーネント → main の SWRenderer を橋渡し

## クレジット

- [ScratchEverywhere](https://github.com/ScratchEverywhere/ScratchEverywhere) — このプロジェクトが基盤とする Scratch ランタイム
- [Scratch](https://scratch.mit.edu/) by MIT Media Lab
- [stb ライブラリ](https://github.com/nothings/stb) — stb_truetype, stb_image
- [nanosvg](https://github.com/memononen/nanosvg) — SVG パーサ＆ラスタライザ
- [NotoSansJP](https://fonts.google.com/noto/specimen/Noto+Sans+JP) — Google の日本語フォント

## ライセンス

このプロジェクトは GPL-3.0 ライセンスの ScratchEverywhere を使用しています。
