## CrossPoint Yomuka v0.7.4

> CrossPoint Yomuka は CrossPoint Reader / CrossPoint JP を基にした非公式コミュニティフォークです。

### 主な変更

- WebUIを日本語化し、ファイル管理、アクセスポイント、青空TXT→EPUB変換、スリープ画面の画像管理を分かりやすくしました。
  - スリープ画像は一覧とテキスト表示を切り替えられます。
  - BMPと透過BMPのアップロードに対応し、失敗時はブラウザ情報を含むエラー報告をコピーできます。
- EPUB読書とキャッシュ生成のメモリ処理を改善しました。
  - WebUIまたはCalibreで転送したEPUBを、再起動せずに開けるようメモリを回復します。
  - 大きい章やZenMaruGothic縦書きで、キャッシュ生成中にメモリ不足になりにくくしました。
  - 全章キャッシュ生成では章ごとに作業用フォントデータを解放します。
- 読書中メニューと表示設定を改善しました。
  - 「この本の表示」で文字サイズを本ごとに指定できます。
  - X4の画面回転時のページ送り・戻しと、読書メニューの側面ボタン表示を整えました。
  - 診断レポートを保存した後も読書画面へ戻れます。
- X4の充電中バッテリー表示を安定させ、SDカードのSPI転送を改善しました。
- X4 Classic（X4 V2）向けにESP32-S3、PSRAM、SDMMC、RTC、バッテリー、物理ボタンを共有できるExperimentalビルド基盤を追加しました。X4 Classic実機での確認は未実施です。

### 更新時の注意

通常のX3/X4配布ファームウェアを更新するだけなら、読書キャッシュを手動で全削除する必要はありません。EPUBをWebUIまたはCalibreで上書き転送した場合は、その本のキャッシュを自動的に作り直します。

転送直後に本を開く際、最初の「キャッシュを生成」確認では「あとで」を選んで本文を先に開けます。全章キャッシュ生成は読書メニューから後で実行できます。問題が続く場合は、読書メニューの「この本のキャッシュを消去」を実行してから開き直し、[不具合の報告](../reporting-bugs-ja.md)の手順で診断またはシリアルログを保存してください。

X4 Classic（X4 V2）はこのリリースの配布対象ではありません。Experimentalビルドを試す場合は、[X4 Classic Experimental確認手順](../x4-classic-experimental-ja.md)を読み、既存X4用のファームウェアとして使用しないでください。

### 更新方法

1. [v0.7.4 リリース](https://github.com/ponto1216-ai/crosspoint-jp/releases/tag/yomuka-v0.7.4) から `firmware.bin` をダウンロードします。
2. 更新前に書籍、設定、SDカードフォントをバックアップします。
3. `firmware.bin` を名前を変えずにSDカードへ置き、端末で **設定 → 本体 → SDカードファームウェア更新** を実行します。
4. 更新中は電源を切ったり、SDカードを抜いたりしません。

`bootloader.bin` と `partitions.bin` は初回書き込み・復旧用、`SHA256SUMS.txt` はダウンロードした配布物の検証用です。通常のSDカード更新では `firmware.bin` を使用します。

詳しい操作は [基本操作・設定・不具合の確認](https://github.com/ponto1216-ai/crosspoint-jp/blob/main/docs/basic-operations-ja.md)、フォント導入は [日本語フォントの導入](https://github.com/ponto1216-ai/crosspoint-jp/blob/main/docs/cjk-fonts.md) を参照してください。