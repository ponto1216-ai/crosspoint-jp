# EPUBキャッシュの保存形式

[開発者ガイド](../contributing/README.md)

確認基準: v0.7.3、2026-09-10。内部形式の概要と実装への索引です。書籍の対応形式は[マニュアル](../manual-ja.md)を参照してください。

## book.bin

`BOOK_CACHE_VERSION` は **7** です。[BookMetadataCache.cpp](../../lib/Epub/Epub/BookMetadataCache.cpp)が生成・検証・読み込みを担当します。

ヘッダーは版、参照表オフセット、spine数、目次数です。その後にタイトル・作者・言語・表紙参照・本文参照・ページ進行方向を保存し、spineと目次の位置を参照表で引きます。一時ファイルで構築・検証した後に公開します。

## Sectionのキャッシュ

`SECTION_FILE_VERSION` は **113** です。[Section.cpp](../../lib/Epub/Epub/Section.cpp)の書き込みと検証を正本とします。

ヘッダーには版、フォント、行間、段落設定、表示領域、ハイフネーション、字下げ、書籍スタイル、画像設定、縦書き、字間等を記録します。ページ数、ページ参照表とアンカーマップの位置も保持します。形式とレイアウト設定を検証して再利用します。

ページ・ブロックの保存順序は同じコミットの書き手と読み手を照合してください。旧Version 8のサンプルコードでは読み込めません。

## 画像・完了判定・読書データ

- PNG/JPEG画像は[PixelCache.h](../../lib/Epub/Epub/converters/PixelCache.h)と[画像キャッシュ検証](../../lib/Epub/Epub/converters/ImageCacheValidation.h)を参照します。
- ソース指紋と全キャッシュ完了マーカーは[Epub.cpp](../../lib/Epub/Epub.cpp)が検証します。存在だけで生成完了とは判定しません。
- 読書位置、しおり、本ごとの設定はキャッシュと区別します。[アーキテクチャ](../contributing/architecture.md#state-and-persistence)を参照してください。

## 形式を変えるとき

シリアライズ対象を変えたら対応する形式番号と検証条件を更新し、旧キャッシュの再生成、中断・再開、読書位置・しおりの保持をX3/X4で確認します。構造体のサイズや順序を旧資料から推測しないでください。

旧Version 3/8の説明は[過去資料](../archive/upstream/file-formats.md)に保存しています。
