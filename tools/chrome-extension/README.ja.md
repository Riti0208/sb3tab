# sb3tab QR — Chrome 拡張

sb3tab の本体側 QR スキャナが読み取る QR コードを生成する拡張機能です。

- **WiFi QR** — SSID とパスワードを encode して、sb3tab がネットワーク
  に接続し、認証情報を `/sd/wifi.txt` に保存できるようにします。
- **プロジェクト QR** — `scratch.mit.edu/projects/<id>` のページを
  開いている状態で popup を起動すると、プロジェクト ID を自動検出して、
  Scratch CDN から `/sd/games/<id>/` にストリーミングダウンロードする
  ための QR を生成します。

両方とも [`tools/gen_qr.py`](../gen_qr.py) と同じ JSON 形式の QR を
作るので、device 側の挙動は CLI から作っても拡張から作っても同じです。

## インストール（unpacked / 開発者モード）

Chrome ウェブストアには未公開なので、手動で読み込みます:

1. `chrome://extensions/` を開く
2. 右上の **デベロッパーモード** を ON
3. **パッケージ化されていない拡張機能を読み込む** をクリック
4. この `tools/chrome-extension/` ディレクトリを選択
5. **sb3tab QR** をピン留めしてツールバーから 1 クリックで開けるように
   しておく

## 使い方

### Scratch プロジェクトを送る（通常の用途）

1. ブラウザで `https://scratch.mit.edu/projects/<id>` を開く
2. 拡張機能のアイコンをクリック。popup は **Project** タブで開き、
   表示中のページから ID を自動検出します
3. 生成された QR を sb3tab の本体スキャナで読み取り。`project.json` と
   全アセットが `/sd/games/<id>/` に保存され、転送完了と同時に
   プロジェクトが起動します

### WiFi 設定を device に登録する（初回 1 回だけ）

1. 拡張機能のアイコンをクリック
2. **WiFi** タブに切り替え
3. SSID とパスワードを入力
4. **Generate QR** をクリックして sb3tab で読み取り
5. device がネットワークに接続し、`/sd/wifi.txt` に認証情報を保存
   するので、次回起動時から自動接続されます

## プライバシー

この拡張機能は、Scratch プロジェクト ID を検出するためにアクティブな
タブの URL を読むだけで、それ以外の通信は行いません。SSID / パスワード
は `chrome.storage` にローカル保存されるので（次回入力を省略するため）、
端末外には何も送信されません。QR の生成も popup 内で完結します。
