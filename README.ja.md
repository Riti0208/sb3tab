# sb3tab

<p align="center">
  <img src="main/assets/logo.png" alt="sb3tab ロゴ" width="420">
</p>

<p align="center">
  <a href="LICENSE"><img alt="License: LGPL-3.0-or-later" src="https://img.shields.io/badge/License-LGPL--3.0--or--later-blue.svg"></a>
  <a href="https://github.com/Riti0208/sb3tab/actions/workflows/build.yml"><img alt="Build" src="https://github.com/Riti0208/sb3tab/actions/workflows/build.yml/badge.svg"></a>
  <img alt="Target" src="https://img.shields.io/badge/target-ESP32--P4-orange">
</p>

<!-- TODO: 実機デモ GIF / 動画に差し替え（QR スキャン → ダウンロード → 実行） -->
<!--
<p align="center">
  <img src="docs/demo.gif" alt="sb3tab が M5Stack Tab5 で Scratch プロジェクトを実行" width="540">
</p>
-->

Scratch プロジェクト（`.sb3` ファイル形式）を ESP32 マイコン上でネイティブ実行 — ブラウザもPCも不要、初回 WiFi 設定後はインターネット接続も不要です。

[ScratchEverywhere](https://github.com/ScratchEverywhere/ScratchEverywhere)（クロスプラットフォームの `.sb3` ランタイム）をベースにしています。

> `sb3tab` は独立した OSS プロジェクトです。Scratch Foundation / MIT による**公式プロジェクトでも、提携・後援を受けたものでもありません**。

## ハイライト

- **M5Stack Tab5（5インチ 720x1280 MIPI DSI）で平均 34 fps**（11スプライト + 10クローンのプロジェクトで計測）
- **QRコードをスキャン → scratch.mit.edu からダウンロード → 実行** — 初回 WiFi 設定後は完全スタンドアローン
- **ES8388 コーデック経由のオンボードサウンド**（WAV + MP3、ピッチ・パン対応）
- **タッチ + USB ゲームパッド入力**（Xbox 360 / Xbox One/Series / DualShock 4 / DualSense / 汎用 HID） — プロジェクトのキー割当を自動マッピング
- **吹き出しとモニターで日本語表示**（NotoSansJP、JIS X 0208 第一水準）
- ブートスプラッシュ、メインメニュー、設定 UI、本体内 QR スキャナ、SD カード永続化

## 対応機能

| 機能 | 状態 |
|---|---|
| スプライト描画（PNG/SVG、`<text>`/`<tspan>` 含む） | ✅ |
| ステージ背景 | ✅ |
| 吹き出し（say/think） | ✅ 日本語対応 |
| ペン描画 | ✅ 線・ドット・スタンプ・クリア |
| 変数 / リストモニター | ✅ |
| 明るさ・幽霊エフェクト | ✅ |
| 色接触判定（touching-color / color-touching） | ✅ オプトイン |
| 音声再生 | ✅ WAV + MP3、ピッチ・パン |
| クローン | ✅ |
| 動き / 制御 / 演算 / データブロック | ✅ |
| 入力（オンスクリーンタッチ / GPIO ボタン / USB ゲームパッド） | ✅ |
| 回転スタイル（LEFT_RIGHT, NONE, ALL_AROUND） | ✅ |

### USB ゲームパッド対応状況

### USB ゲームパッド対応状況

Tab5 の USB-A ポートに有線で挿すパッド向けです。実機動作確認済み:

- ✅ **Xbox 360 有線**
- ✅ **Sony DualShock 4** (PID 0x05C4 / 0x09CC)

コードは入ってるが実機未検証:

- **Sony DualSense** (PID 0x0CE6)
- **汎用 USB HID ゲームパッド** (interface class 0x03 / sub 0x00 / proto 0x00) —
  接続時に HID Report Descriptor を解析するので、「DirectInput / HID」表記の
  一般的なパッドはほぼ enumerate できる想定

非対応（このリストの項目で issue を立てる必要はないです）:

- **Nintendo Switch Pro Controller** — enumeration 時に ESP-IDF v5.4.1 USB host
  stack の `spinlock_acquire` assert で再起動。sb3tab 側ではなく ESP-IDF 側
  のバグ。
- **Xbox One / Series 有線** — GIP の電源 ON パケットは送信するが、認証 /
  capabilities / hello を含む完全ハンドシェイクが未実装なので入力が流れない。
- **Bluetooth 全般** — ESP32-C6 側に BT スタックが必要、未実装。

### 未実装（ランタイム）

- 画像エフェクト: COLOR, FISHEYE, WHIRL, PIXELATE, MOSAIC（上流の ScratchEverywhere にもなし）
- Ogg 音声
- 翻訳・Text-to-Speech ブロック

## ハードウェア

### メインターゲット — M5Stack Tab5

| サブシステム | 詳細 |
|---|---|
| MCU | ESP32-P4 @ 360 MHz、32 MB PSRAM、16 MB Flash |
| ディスプレイ | 5インチ 720x1280 MIPI DSI（ST7123）、静電容量タッチ |
| WiFi | ESP32-C6 を SDIO 接続、[esp_hosted](https://github.com/espressif/esp-hosted) v1.4.7 経由 |
| 音声 | ES8388 I²S コーデック + オンボードスピーカー |
| カメラ | SC202CS MIPI CSI（QR スキャンに使用） |
| ストレージ | SD カード（FAT）— プロジェクト + WiFi 認証情報 |
| 入力 | タッチ、GPIO ボタン、USB-A ホスト（Xbox 360 / Xbox One / DS4 / DualSense / HID ゲームパッド） |

オンボード ESP32-C6 のスレーブファームは [`slave/`](slave/) にあります（`esp_hosted` をベンダリング）。

### 動作確認済みのその他のボード

- **M5Stamp ESP32-P4** + ILI9341 SPI LCD（320x240） — 最小構成のブリングアップ用
- **XIAO ESP32-S3 Sense** + ILI9341 — 初期検証用

## ビルド

### 必要なもの

- [ESP-IDF v5.4.1](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32p4/get-started/)（v5.5.x には DSI のリグレッションあり）
- Python 3 と `qrcode` / `requests`（ホスト側ツール用）

### 手順

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/tty.usbmodem1101 flash monitor
```

> `set-target` 実行後は `sdkconfig` が `sdkconfig.defaults` から再生成されます。
> 設定が崩れた場合は `rm sdkconfig && idf.py reconfigure` が確実です。

## 使い方

### 方法 A — QR コードワークフロー（Tab5 推奨）

1. WiFi 用 QR を [Chrome 拡張](tools/chrome-extension/) または CLI で生成:
   ```bash
   python3 tools/gen_qr.py wifi "MySSID" "MyPassword"
   ```
2. scratch.mit.edu のプロジェクト ID から QR を生成:
   ```bash
   python3 tools/gen_qr.py project 1296865674
   ```
3. 本体のメニューから QR スキャナを起動:
   - WiFi QR → 認証情報を `/sd/wifi.txt` に保存
   - プロジェクト QR → Scratch CDN からアセットを `/sd/games/<id>/` にストリーミング保存して即実行

### 方法 B — USB テザリング（M5Stamp P4 / S3 ボード）

```bash
python3 -u tools/send_sb3.py <project.sb3 または scratch.mit.edu の URL> /dev/tty.usbmodem1101
```

ホスト側で `.sb3` を展開し（P4 RISC-V では miniz の inflate がクラッシュするため）、`project.json` と各アセットを USB シリアル経由でストリーミング送信します。`START` 受信時点でランタイムが開始します。

## アーキテクチャ

```
┌──────────────────────────────────────────────┐
│  ソース                                       │
│  • QR（カメラ） — scratch.mit.edu / WiFi     │
│  • USB シリアル — ホストで展開済み .sb3       │
│  • SD カード   — 過去にダウンロードしたゲーム  │
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│  ESP32-P4 (Tab5)                             │
│                                              │
│  ┌────────────────────────────────────────┐  │
│  │ scratch_core（インツリーコンポーネント）│  │
│  │  • ブロックランタイム + パーサ          │  │
│  │  • ヘッドレスレンダラー + 吹き出し      │  │
│  │  • 音声ミキサー（WAV / MP3）           │  │
│  └────────────────┬───────────────────────┘  │
│                   │                          │
│  ┌────────────────▼───────────────────────┐  │
│  │ main/                                  │  │
│  │  • SWRenderer（480x360、デュアルコア） │  │
│  │  • DSI パネル + PPA スケール/回転      │  │
│  │  • UI メニュー、設定、モーダル          │  │
│  │  • WiFi（esp_hosted）、SD、時刻同期    │  │
│  │  • カメラ + quirc QR デコード          │  │
│  │  • タッチ / GPIO / USB ゲームパッド入力 │  │
│  │  • ES8388 音声                         │  │
│  └────────────────────────────────────────┘  │
└──────────────────────────────────────────────┘
```

### レンダリングパイプライン

1. **Core 0**: 次フレームのバックバッファをクリア（前フレームの step とパイプライン）
2. **Core 1**: Scratch step を実行（ブロックディスパッチ、動き、センシング）
3. 両コア: スプライトを偶数 / 奇数で分割して並列合成
4. ペンレイヤを重ねる（RGBA8888）
5. 吹き出し + 変数モニター
6. PPA SRM: スケール 2× + 270° CCW 回転 + RGB888 → RGB565 を 1 回の HW 呼び出しで実行
7. DSI スワップ（ダブルバッファ、`memcpy` 不要）

### 主要な技術的判断

- **PPA（Pixel Processing Accelerator）** で最終のスケール / 回転 / 色変換を一括処理
- **デュアルコア + パイプライン化したクリア** — step とクリアをオーバーラップして PSRAM 帯域を隠蔽
- **`blitRGBA` の固定小数点 16.16 内側ループ + 内部 SRAM コスチュームキャッシュ**（≤ 64 KB）
- **ホスト側 `.sb3` 展開** — P4 RISC-V で miniz inflate が不安定なため
- **USB Serial/JTAG LL API** を直接使用（VFS ドライバはコンソールと競合）
- **`esp_hosted` v1.4.7** を使用（v2.x は FreeRTOS idle のメモリ確保でクラッシュ）
- **stb_truetype + NotoSansJP サブセット**（JIS X 0208 第一水準、フラッシュ埋め込み）

## ディレクトリ構成

```
main/                Tab5 ファームウェア（表示・入力・音声・UI・通信）
components/
  scratch_core/      Scratch ランタイム（ScratchEverywhere フォークをベンダリング）
slave/               ESP32-C6 用 esp_hosted スレーブファーム
tools/
  send_sb3.py        ホスト側 .sb3 展開 + USB ストリーマ
  gen_qr.py          WiFi / プロジェクト QR ジェネレータ
  chrome-extension/  QR 生成用ブラウザ拡張
  s3_lcd_test/       XIAO S3 + ILI9341 ブリングアップ
  subset_font.py     NotoSansJP サブセット生成
```

## クレジット

- [ScratchEverywhere](https://github.com/ScratchEverywhere/ScratchEverywhere) — このプロジェクトが基盤とする Scratch ランタイム
- [Scratch](https://scratch.mit.edu/) by MIT Media Lab
- [esp_hosted](https://github.com/espressif/esp-hosted) — ESP32-C6 WiFi コプロセッサスタック
- [quirc](https://github.com/dlbeer/quirc) — QR コードデコーダ
- [stb ライブラリ](https://github.com/nothings/stb) — stb_truetype, stb_image
- [nanosvg](https://github.com/memononen/nanosvg) — SVG パーサ＆ラスタライザ
- [minimp3](https://github.com/lieff/minimp3) — MP3 デコーダ
- [NotoSansJP](https://fonts.google.com/noto/specimen/Noto+Sans+JP) — Google の日本語フォント

## ライセンス

`sb3tab` は **LGPL-3.0-or-later** で配布されます。LGPL v3 / GPL v3 の全文は
[LICENSE](./LICENSE)、同梱しているサードパーティのソフトウェア・フォント等の
帰属表示は [THIRD_PARTY_LICENSES.md](./THIRD_PARTY_LICENSES.md) を参照してください。

### 商標について

**Scratch** は Scratch Foundation の商標です。本プロジェクトは `.sb3` ファイル
形式を nominative fair use の範囲で取り扱うものであり、製品名に "Scratch" を
含めていません。**ESP32** / **ESP-IDF** は Espressif Systems の商標、
**M5Stack** / **M5Tab5** / **M5Stamp** は M5Stack Technology Co., Ltd. の
商標です。その他の商標はすべて各所有者に帰属します。
