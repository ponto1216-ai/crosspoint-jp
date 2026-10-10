## CrossPoint Yomuka v0.8.2

> CrossPoint Yomuka は CrossPoint Reader / CrossPoint JP を基にした非公式コミュニティフォークです。

### 複数冊の読書データ共有

Homeの **共有・転送 → 読書データの共有** で、読書データがあるEPUBを複数選べます。選んだ本の共有JSONをSDカードへ1冊ずつ書き出すか、同じWi-Fi上の端末へ直接送信できます。受信側が一時ホットスポットを作成する方法にも対応しました。ホットスポットの接続パスワードは端末内に保存され、次回も使えます。

受信側では共有ファイルとSDカード上のEPUBをBookIDで照合し、一致した本の内容を確認してから取り込めます。送受信や取り込みで一部の本が失敗しても、成功済みの本は保持し、失敗した本を再試行できます。共有ファイルを受け取るだけでは読書データは変更しません。

共有する本の一覧は保存済みの場所を先に確認し、必要なときだけSD全体を検索できます。削除済みの本などを検索対象から外しても、読書記録は残ります。詳しい操作と置き換わる範囲は[読書データ共有マニュアル](https://github.com/ponto1216-ai/crosspoint-jp/blob/yomuka-v0.8.2/docs/reading-data-sharing-ja.md)を参照してください。

### CrossPoint本家の修正をYomukaへ適応

以下は上流CrossPoint Readerの変更を参考に、Yomukaの既存機能に合わせて取り込んだ修正です。各PRをそのまま全面的に取り込んだものではありません。

- [#3847 電源モードの競合](https://github.com/crosspoint-reader/crosspoint-reader/pull/3847)：描画中にCPUが低速へ切り替わり得る競合を修正しました。Yomuka独自のWi-Fi使用中の電源制御は維持しています。
- [#3830 最近の本のメモリ再確保](https://github.com/crosspoint-reader/crosspoint-reader/pull/3830)：同じ本を開き直す際に既存項目を再利用します。YomukaのBookIDによる移動・改名判定にも対応しました。
- [#3732 Webアップロードの中断](https://github.com/crosspoint-reader/crosspoint-reader/pull/3732)：通信が遅いときも戻る操作で中断できるよう、YomukaのWeb画面と共有ファイル受信処理に合わせて修正しました。
- [#3875 段落の途中で組版を区切った後の一字下げ](https://github.com/crosspoint-reader/crosspoint-reader/pull/3875)：Yomukaの組版で必要な段落インデント処理を適応しました。上流PR全体の移植ではありません。
- [#3737 低電力状態でのUSB診断ログ](https://github.com/crosspoint-reader/crosspoint-reader/pull/3737)：低速動作時にログ行が欠け得る処理を修正しました。
- [#3831 SDフォントのメモリ再確保](https://github.com/crosspoint-reader/crosspoint-reader/pull/3831)：字幅キャッシュの拡張部分を取り込みました。上流PRに含まれるほかのテーブルの逐次読み込みは対象外です。

### Yomuka独自の改善

- 縦書きで「、」「。」が行末の直前に来る場合、文字に重ならず同じ行に収まるよう改善しました。ほかの連続記号の扱いは継続課題です。
- 大きなEPUBの章サイズ確認に使うメモリを抑えました。
- 受信した本を選んで取り込む画面で、描画中の短いボタン操作が抜けにくいようにしました。

### 確認範囲と制限

- X3 / 通常X4で、読書、スリープ復帰、複数冊の直接共有と中断、受信後の照合・取り込み、同じWi-Fiと一時ホットスポットでの共有を確認しました。検索中表示と共有一覧の見た目、取り込み一覧での短いボタン操作も両機種で確認しました。
- 100/300/500冊を同条件で比較した速度測定は未実施です。検索速度の数値的な改善は主張しません。
- X4 ClassicはExperimentalビルドのみで、実機未確認です。通常X4用ファームウェアをX4 Classicへ使用しないでください。
- EPUB本体、フォント、端末全体の設定、Wi-Fi情報は読書データ共有に含みません。取り込み時は同じBookIDのEPUBが必要です。
- X3のバッテリー残量に関するIssue #40は継続調査中です。今回の更新で解消したとは判断していません。

### 更新方法

1. 公開されたv0.8.2の `firmware.bin` をSDカードへ置きます。
2. **設定 → 管理 → SDカードファームウェア更新**を実行します。
3. 更新中は電源を切ったりSDカードを抜いたりしないでください。

通常のSD更新には `firmware.bin` を使用します。`bootloader.bin` と `partitions.bin` は初回書き込み・復旧用です。
