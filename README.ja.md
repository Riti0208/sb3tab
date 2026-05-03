# ScratchESP

[Scratch](https://scratch.mit.edu/) プロジェクト（`.sb3`）を ESP32 マイコン上でネイティブ実行 — ブラウザもPCも不要、初回 WiFi 設定後はインターネット接続も不要です。

[ScratchEverywhere](https://github.com/ScratchEverywhere/ScratchEverywhere)（クロスプラットフォーム Scratch ランタイム）をベースにしています。

## ハイライト

- **M5Stack Tab5（5インチ 720x1280 MIPI DSI）で平均 34 fps**（11スプライト + 10クローンのプロジェクトで計測）
- **QRコードをスキャン → scratch.mit.edu からダウンロード → 実行** — 初回 WiFi 設定後は完全スタンドアローン
- **ES8388 コーデック経由のオンボードサウンド**（WAV + MP3、ピッチ・パン対応）
- **タッチ + Xbox 360 USB ゲームパッド入力** — プロジェクトのキー割当を自動マッピング
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
| 入力（オンスクリーンタッチ / GPIO ボタン / Xbox 360 パッド） | ✅ |
| 回転スタイル（LEFT_RIGHT, NONE, ALL_AROUND） | ✅ |

### 未実装

- 画像エフェクト: COLOR, FISHEYE, WHIRL, PIXELATE, MOSAIC（上流の ScratchEverywhere にもなし）
- Ogg 音声
- 翻訳・Text-to-Speech ブロック
- Xbox One / Xbox Wireless Adapter（Xbox 360 有線パッドのみ認識）

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
| 入力 | タッチ、GPIO ボタン、USB-A ホスト（Xbox 360 有線パッド） |

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
│  │  • タッチ / GPIO / Xbox 360 入力       │  │
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
docs/                UI モックアップ
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

GPL-3.0（ScratchEverywhere から継承）。
