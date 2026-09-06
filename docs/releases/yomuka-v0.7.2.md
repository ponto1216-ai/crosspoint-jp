## CrossPoint Yomuka v0.7.2

> CrossPoint Yomuka は CrossPoint Reader / CrossPoint JP を基にした非公式コミュニティフォークです。

### 主な変更

- 既存の **読書メーター** の表示を調整しました。
  - X3は概要と詳細を2ページに分け、X4は1画面にまとめて表示します。
  - よく読んだ本から、表示上 `0分` になる本を除外しました。
- X3の読書メーター詳細ページで、見出しと「直近7日間」が重ならないよう配置を調整しました。
- 縦書きの小書き仮名（ぁ・ぃ・ぅ・ぇ・ぉ・っ・ゃ・ゅ・ょ・ゎなど）の位置を整えました。

### 更新時の注意

この更新では、既存の読書位置、しおり、本ごとの読書設定、読書履歴は保持されます。

### 更新方法

1. [v0.7.2 リリース](https://github.com/ponto1216-ai/crosspoint-jp/releases/tag/yomuka-v0.7.2) から `firmware.bin` をダウンロードします。
2. 更新前に書籍、設定、SDフォントをバックアップします。
3. `firmware.bin` を名前を変えずにSDカードへ置き、端末で **設定 → 本体 → SDカードファームウェア更新** を実行します。
4. 更新中は電源を切ったり、SDカードを抜いたりしません。

`bootloader.bin` と `partitions.bin` は初回書き込み・復旧用、`SHA256SUMS.txt` はダウンロードした配布物の検証用です。通常のSDカード更新では `firmware.bin` を使用します。

詳しい操作は [基本操作・設定・不具合の確認](https://github.com/ponto1216-ai/crosspoint-jp/blob/main/docs/basic-operations-ja.md)、フォント導入は [日本語フォントの導入](https://github.com/ponto1216-ai/crosspoint-jp/blob/main/docs/cjk-fonts.md) を参照してください。
