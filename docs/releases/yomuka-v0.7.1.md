## CrossPoint Yomuka v0.7.1

> CrossPoint Yomuka は CrossPoint Reader / CrossPoint JP を基にした非公式コミュニティフォークです。

### 主な変更

- EPUBを移動・改名した後や、同じファイル名の更新版へ差し替えた後も、読書位置、しおり、本ごとの読書設定、読書履歴を引き継ぎます。
  - 書籍本体の内容を基準にした識別子を使い、生成し直せる読書キャッシュの保存場所とは分離しました。
  - 以前の保存場所にある読書位置としおりは、初回に互換読み込みして新しい保存場所へ移します。旧データはすぐには削除しません。
- 本の履歴の重複を抑え、既読・完読アイコンを再起動後も維持します。
- 書庫へ移した本は長押しメニューから元の場所へ戻せます。戻し先に同名ファイルがある場合は、既存ファイルを上書きしません。
- 縦書きの文字方向を [Unicode UAX #50](https://www.unicode.org/reports/tr50/) の既定値へ合わせました。
  - 丸数字、ローマ数字、温度記号などは直立します。
  - 矢印、ギリシャ文字、キリル文字、欧文は90度時計回りに回転します。矢印は字形も回転するため、見た目の向きが変わるのは仕様です。

### SDカードフォント

別配布の [SD Card Fonts](https://github.com/ponto1216-ai/crosspoint-jp/releases/tag/sd-fonts) も更新しました。Noto Sans/Serif JP、BIZ UD Gothic/Mincho、Zen Maru Gothicで、キリル文字、`℃`、`ℓ`を追加しています。Noto系とZen Maru Gothicには`℉`も含まれます。

### 更新時の注意

縦書きのページ配置を更新しています。v0.7.1へ更新後、縦書きEPUBを初めて開く際は読書キャッシュを自動で再生成します。再生成中は最初の表示に時間がかかることがありますが、読書位置、しおり、本ごとの設定は保持されます。

SDカードフォントを更新する場合は、端末でのダウンロード後または配布ZIPを展開後に端末を再起動してください。

### 更新方法

1. [v0.7.1 リリース](https://github.com/ponto1216-ai/crosspoint-jp/releases/tag/yomuka-v0.7.1) から `firmware.bin` をダウンロードします。
2. 更新前に書籍、設定、SDフォントをバックアップします。
3. `firmware.bin` を名前を変えずにSDカードへ置き、端末で **設定 → 本体 → SDカードファームウェア更新** を実行します。
4. 更新中は電源を切ったり、SDカードを抜いたりしません。

`bootloader.bin` と `partitions.bin` は初回書き込み・復旧用、`SHA256SUMS.txt` はダウンロードした配布物の検証用です。通常のSDカード更新では `firmware.bin` を使用します。

詳しい操作は [基本操作・設定・不具合の確認](https://github.com/ponto1216-ai/crosspoint-jp/blob/main/docs/basic-operations-ja.md)、フォント導入は [日本語フォントの導入](https://github.com/ponto1216-ai/crosspoint-jp/blob/main/docs/cjk-fonts.md) を参照してください。
