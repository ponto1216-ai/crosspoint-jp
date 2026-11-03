## CrossPoint Yomuka v0.8.4

> CrossPoint Yomuka は CrossPoint Reader / CrossPoint JP を基にした非公式コミュニティフォークです。

### EPUBの表示

- 本文の一部分に指定された文字サイズを、縦書き・横書きの両方で反映します。
- 複数のCSSファイルにまたがる指定順を保ち、スタイルの読み込みが途中で失敗した場合に不完全なキャッシュを残さないようにしました。
- CSSの改ページ指定を反映します。
- 長い表のセルがページ末で途切れないよう、次のページへ続けて表示します。
- 横書きで隣り合うルビの重なりを防ぎ、縦書きでは長いルビが列末に重ならないようにしました。
- 半角の閉じ括弧 `)` が、鍵括弧 `」` の前で行頭や列頭に残る場合を修正しました。

改行・改ページ位置が変わるため、従来のセクションキャッシュは初回表示時に作り直されます。

### SDフォントとメモリ

- 日本語本文に不足する字形の代替表示で、必要なメモリを抑えました。
- SDフォント一覧の未使用枠を先に確保しないようにしました。
- 同じ内容の文字範囲表を複数サイズのSDフォントで共有し、読み込み時のメモリ使用量を抑えました。

### 変更の出所

EPUB表示の改善は、CrossPoint Readerの[EPUB表示拡張 #3891](https://github.com/crosspoint-reader/crosspoint-reader/pull/3891)と[ルビ #4204](https://github.com/crosspoint-reader/crosspoint-reader/pull/4204)を個別に調査し、Yomukaの既存の日本語組版に合わせて実装したものです。SDフォントのメモリ改善は上流の[一覧枠の削減 #4094](https://github.com/crosspoint-reader/crosspoint-reader/pull/4094)と[文字範囲表の共有 #4145](https://github.com/crosspoint-reader/crosspoint-reader/pull/4145)を参考にしています。FreeInk SDKの更新は含みません。

### 確認範囲と制限

- X3・通常X4で、EPUBの本文・表・ルビ・改ページ、半角閉じ括弧の改行、SDフォントでの表示を確認しました。
- X3・通常X4で、SDフォント選択後のEPUB表示と、選択した本のキャッシュ一括生成を確認しました。
- ホストでレイアウトとCSSの回帰テストを実行しました。SDフォント共有によるメモリ節約量とキャッシュ生成時間の変更前後比較は未測定です。
- TXTの新しい読書処理とBluetoothページターナーは、この版には含めません。
- X4 ClassicはExperimentalで、実機動作は未確認です。通常X3/X4用ファームウェアをX4 Classicへ使用しないでください。

### 更新方法

1. 公開されたv0.8.4の `firmware.bin` をSDカードへ置きます。
2. **設定 → 管理 → SDカードファームウェア更新**を実行します。
3. 更新中は電源を切ったりSDカードを抜いたりしないでください。

通常のSD更新には `firmware.bin` を使用します。`bootloader.bin` と `partitions.bin` は初回書き込み・復旧用です。
