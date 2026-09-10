# CrossPoint Yomuka

> 使い方は[ユーザーマニュアル](docs/manual-ja.md)、導入は[はじめて使う](docs/getting-started-ja.md)を参照してください。

CrossPoint Yomuka は、Xteink X3 / X4 向けの日本語読書に重点を置くオープンソースファームウェアです。本家 CrossPoint Reader と CrossPoint JP を基にした、非公式のコミュニティフォークです。

日本語EPUBの縦書き、ルビ、縦中横、SDカードの日本語フォント、青空文庫、EPUB/TXT/XTCの読書を扱いやすくすることを目指しています。

> [!WARNING]
> CrossPoint Yomuka は非公式ファームウェアです。端末メーカー、本家CrossPoint Reader、CrossPoint JPによる承認・サポート・保証はありません。導入・更新は自己責任で行い、事前にSDカード内の必要なデータをバックアップしてください。

## 対応端末

| 端末 | 状況 |
|---|---|
| Xteink X3 | 対応。UC8253 / UC8279の表示コントローラを判別し、X3固有の入力・スリープ・表示を確認対象とします。 |
| Xteink X4 | 対応。X3と同じファームウェアで動作します。 |

端末の個体差、SDカード、書籍の構造により動作や表示は変わる場合があります。

## 対応フォーマットと制限

| 形式 | 対応状況 |
|---|---|
| EPUB | 縦書き・横書き、ルビ、画像、書籍スタイルに対応 |
| TXT | 横書きに対応 |
| XTC / XTCH | 対応 |
| PDF | 非対応 |

EPUBのCSS・固定レイアウト・段組み・特殊な位置指定を完全に再現するものではありません。画像の多い書籍、複雑なCSS、特殊な画像形式では表示が簡略化されたり、初回解析に時間がかかったりする場合があります。

## はじめる

- [はじめて使う](docs/getting-started-ja.md)
- [ファームウェアの導入と更新](docs/firmware-update-ja.md)
- [日本語フォントの導入](docs/cjk-fonts.md)

## 使い方

[ユーザーマニュアル](docs/manual-ja.md)から、読書、表示設定、本の管理、Wi-Fi、診断の各ページへ進めます。

## リリースと不具合報告

変更内容と更新時の注意は[Releases](https://github.com/ponto1216-ai/crosspoint-jp/releases)に掲載しています。不具合は[報告ガイド](docs/reporting-bugs-ja.md)を参照し、[GitHub Issues](https://github.com/ponto1216-ai/crosspoint-jp/issues)へお知らせください。

## 開発と今後の予定

- [開発者ガイド](docs/contributing/README.md)
- [ロードマップ](docs/roadmap.md)

## ライセンスと謝辞

ライセンスはリポジトリ内のライセンスファイルを確認してください。フォント、ライブラリ、画像には個別のライセンスが適用される場合があります。

CrossPoint Yomuka は、本家CrossPoint Reader、CrossPoint JP、関連するオープンソースプロジェクトの成果を基にしています。開発者、貢献者、ライブラリ作者、フォント作者の皆さまに感謝します。
