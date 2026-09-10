# サイドボタンレイアウト設定の改善と傾き連動

> 過去の設計・計画または記録です。現在の仕様や実装完了を示すものではありません。
> 保存元: `docs/superpowers/specs/2026-04-11-remove-side-button-layout-design.md`（v0.7.3時点）。


**日付**: 2026-04-11

## Context

サイドボタンレイアウト設定の「Prev/Next」「Next/Prev」ラベルが紛らわしい。
また、傾きによるページ送りがこの設定に連動していない。

## 変更内容

1. 設定ラベルを「標準 / 反転」（英語: Standard / Reversed）に変更
2. 傾きページ送りの方向を sideButtonLayout 設定に連動させる
