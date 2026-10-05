#include "BookSyncActivity.h"

#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "ReadingStatusHelper.h"
#include "SdCardFontGlobals.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "sync/ReviewNavigation.h"
#include "sync/SnapshotPositionProjector.h"
#include "sync/SnapshotPreview.h"
#include "util/BookDataPath.h"
using namespace yomuka::sync;
namespace {
constexpr int rowsTop = 58;
constexpr int rowHeight = 32;
constexpr int pageNumberBottom = 60;
constexpr int footerGap = 12;
int rowsPerPage(int screenHeight) {
  // The page number has its own band above the button hints.
  return std::max(1, (screenHeight - pageNumberBottom - footerGap - rowsTop) / rowHeight);
}
const char* names[] = {"progress", "bookmarks", "readerSettings", "history"};
const char* labels[] = {"読書位置（近似）", "しおり（全体置換）", "本の設定（全体置換）", "履歴（本別の要約）"};
// Small outline icons do not depend on the font's symbol coverage.
void drawUnitIcon(GfxRenderer& r, int unit, int x, int y, bool ink) {
  if (unit == 0) {
    r.drawLine(x, y + 8, x + 16, y + 8, ink);
    r.drawLine(x + 10, y + 2, x + 16, y + 8, ink);
    r.drawLine(x + 10, y + 14, x + 16, y + 8, ink);
  } else if (unit == 1) {
    r.drawLine(x + 2, y, x + 14, y, ink);
    r.drawLine(x + 2, y, x + 2, y + 16, ink);
    r.drawLine(x + 14, y, x + 14, y + 16, ink);
    r.drawLine(x + 2, y + 16, x + 8, y + 11, ink);
    r.drawLine(x + 8, y + 11, x + 14, y + 16, ink);
  } else if (unit == 2) {
    for (int i = 0; i < 3; ++i) {
      const int yy = y + 3 + i * 5, xx = x + 3 + i * 4;
      r.drawLine(x, yy, x + 16, yy, ink);
      r.drawRect(xx, yy - 2, 4, 5, ink);
    }
  } else {
    r.drawRect(x, y, 17, 17, ink);
    r.drawLine(x + 8, y + 3, x + 8, y + 8, ink);
    r.drawLine(x + 8, y + 8, x + 13, y + 11, ink);
  }
}
}  // namespace
void BookSyncActivity::fail(const std::string& text) {
  importComplete = false;
  message = text;
  screen = Screen::Message;
  cursor = 0;
  requestUpdate();
}
void BookSyncActivity::onEnter() {
  Activity::onEnter();
  uint64_t id = 0;
  if (!epub || !epub->getSourceFingerprint(&id) || !id || !READING_HISTORY.endSession()) {
    blocked = true;
    fail("保存を確認できません。取り込み中止");
    return;
  }
  book = {id, static_cast<uint32_t>(epub->getSpineItemsCount()), epub->getPath(), epub->getTitle(), epub->getAuthor()};
  if (!SETTINGS.loadFromFile()) {
    blocked = true;
    fail("全体設定を読めません。再起動してください");
    return;
  }
  sdFontSystem.releaseLoadedFamily(renderer);
  requestUpdate();
}
bool BookSyncActivity::fontAvailable(const char* family, const char* sdName, void*) {
  return sdName && *sdName ? sdFontSystem.registry().findFamily(sdName) != nullptr
                           : family && std::strcmp(family, "noto-sans") == 0;
}
void BookSyncActivity::loadImport(const std::string& path) {
  // Full size/read check before allocating the bounded snapshot buffer.
  HalFile file;
  if (!Storage.openFileForRead("SYNC", path, file)) {
    fail("ファイルを読めません");
    return;
  }
  const size_t size = file.size();
  if (!file.close() || !size || size > 65536) {
    fail("ファイルサイズが不正です");
    return;
  }
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < size + 4096 ||
      heap_caps_get_free_size(MALLOC_CAP_8BIT) < size * 3 + 32768) {
    fail("メモリが不足しています。再起動後に再試行");
    return;
  }
  std::vector<uint8_t> raw(size);
  size_t read = 0;
  if (readBytes(path, raw.data(), raw.size(), read) != ReadStatus::Present ||
      parseSnapshot(raw.data(), read, snapshot) != SnapshotError::None) {
    fail("不正な同期ファイルです。変更なし");
    return;
  }
  const auto expected = exchangeFilePath(book.id);
  const auto id = expected.substr(std::string("/YomukaSync/").size(), 16);
  if (validateSnapshotTarget(snapshot, id.c_str(), book.spines) != SnapshotError::None) {
    snapshot.clear();
    fail("EPUBが一致しないか、位置が範囲外です");
    return;
  }
  available = 0;
  for (unsigned i = 0; i < 4; ++i)
    if (!snapshot["units"][names[i]].isUnbound()) available |= 1 << i;
  if (!snapshotSettingsAvailable(snapshot, fontAvailable, nullptr)) available &= ~ReaderOverrides;
  selected = available;
  if (!available) {
    fail("必要なフォントがありません。変更なし");
    return;
  }
  cursor = 0;
  screen = Screen::Units;
  requestUpdate();
}
void BookSyncActivity::makePreview() {
  if (!selected) {
    requestUpdate();
    return;
  }
  if (!importing && exportSnapshot(book, selected, READING_HISTORY, snapshot) != ExchangeError::None) {
    fail("書き出し元を読めません。変更なし");
    return;
  }
  lines.clear();
  const auto title = renderer.truncatedText(UI_10_FONT_ID, book.title.c_str(), renderer.getScreenWidth() - 30);
  lines.push_back(title);
  lines.push_back(std::string("BookId: ") + snapshot["bookId"].as<const char*>());
  lines.push_back(importing ? "選んだ種類だけ置き換えます" : "同名の同期ファイルは置き換えます");
  JsonDocument local;
  if (importing && exportSnapshot(book, selected, READING_HISTORY, local) != ExchangeError::None) {
    fail("現在の保存データを確認できません");
    return;
  }
  for (unsigned i = 0; i < 4; ++i) {
    if (!(selected & (1 << i))) continue;
    auto incoming = snapshot["units"][names[i]];
    lines.push_back(labels[i]);
    const auto details = previewUnit(i, local["units"][names[i]], incoming, book.spines, importing);
    lines.insert(lines.end(), details.begin(), details.end());
  }
  lines.push_back("選択していない種類は変更しません");
  // Wrap at UTF-8 boundaries so removal and setting values remain readable.
  std::vector<std::string> wrapped;
  lineKinds.clear();
  const int width = renderer.getScreenWidth() - 30;
  for (const auto& original : lines) {
    int kind = 0;
    for (unsigned unit = 0; unit < 4; ++unit)
      if (original == labels[unit]) kind = unit + 1;
    const bool warning =
        original.find("すべて削除") != std::string::npos || original.find("設定をすべて解除") != std::string::npos ||
        original.find("受信にない設定") != std::string::npos || original.find("注意:") != std::string::npos;
    if (warning) kind = -1;
    const auto line = warning && original.find("注意:") == std::string::npos ? "注意: " + original : original;
    const int lineWidth = width - (kind > 0 ? 24 : 0);
    std::string row;
    for (size_t at = 0; at < line.size();) {
      size_t end = at + 1;
      while (end < line.size() && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80) ++end;
      const auto glyph = line.substr(at, end - at);
      if (!row.empty() && renderer.getTextWidth(UI_10_FONT_ID, (row + glyph).c_str()) > lineWidth) {
        wrapped.push_back(row);
        lineKinds.push_back(kind);
        if (kind > 0) kind = 0;
        row.clear();
      }
      row += glyph;
      at = end;
    }
    wrapped.push_back(row);
    lineKinds.push_back(kind);
  }
  lines = std::move(wrapped);
  cursor = 0;
  screen = Screen::Confirm;
  requestUpdate();
}
bool BookSyncActivity::project(const Progress& source, const BookReaderSettings::Override* incoming, Progress& output,
                               void* context) {
  auto& self = *static_cast<BookSyncActivity*>(context);
  SnapshotPositionProjector projector{self.renderer, self.epub, self.book};
  return SnapshotPositionProjector::project(source, incoming, output, &projector);
}
void BookSyncActivity::execute() {
  if (!importing) {
    if (!Storage.ensureDirectoryExists("/YomukaSync") || !writeJson(exchangeFilePath(book.id), snapshot)) {
      fail("書き出し失敗。元データは変更なし");
      return;
    }
    fail("書き出しました: " + exchangeFilePath(book.id));
    return;
  }
  message = "位置を計算して取り込み中...";
  screen = Screen::Message;
  requestUpdateAndWait();
  std::vector<PreparedFile> prepared;
  if (prepareSnapshotImport(book, snapshot, selected, READING_HISTORY, project, this, prepared) !=
          ExchangeError::None ||
      !BookDataPath::ensureDirectory(book.id)) {
    fail("準備に失敗しました。保存データは変更なし");
    return;
  }
  const auto result = applyPreparedFiles(prepared);
  if (result == TransactionResult::RecoveryRequired) {
    blocked = true;
    fail("復旧が必要です。SDを確認して再起動");
    return;
  }
  if (result != TransactionResult::Committed && result != TransactionResult::CleanupPending) {
    fail("取り込み失敗。元データへ復旧しました");
    return;
  }
  if (!READING_HISTORY.reloadAfterSync()) {
    blocked = true;
    fail("取り込み済み。再起動してください");
    return;
  }
  invalidateBookListStatusIndexEntry(book.path, "/.crosspoint");
  if (result == TransactionResult::CleanupPending) {
    blocked = true;
    fail("取り込み済み。後片付けのため再起動");
    return;
  }
  fail("取り込み完了");
  importComplete = true;
}
void BookSyncActivity::finishImport() {
  if (selected & ReaderOverrides)
    activityManager.goHome();
  else
    activityManager.goToReader(book.path);
}
void BookSyncActivity::loop() {
  if (ignoreRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Confirm))
      ignoreRelease = false;
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (blocked) return;
    if (screen == Screen::Message && importComplete) {
      finishImport();
      return;
    }
    if (screen == Screen::Menu) {
      activityManager.goToReader(book.path);
      return;
    }
    if (screen == Screen::Confirm) {
      screen = Screen::Units;
      cursor = 4;
    } else if (screen == Screen::Units && importing) {
      screen = Screen::Files;
      cursor = 0;
      snapshot.clear();
    } else {
      snapshot.clear();
      screen = Screen::Menu;
      cursor = 0;
    }
    requestUpdate();
    return;
  }
  if (screen == Screen::Confirm) {
    const int finalPage = finalReviewPage(lines.size(), rowsPerPage(renderer.getScreenHeight()));
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up))
      cursor = reviewInput(cursor, finalPage, ReviewButton::Previous).page;
    else if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
             mappedInput.wasReleased(MappedInputManager::Button::Down))
      cursor = reviewInput(cursor, finalPage, ReviewButton::Next).page;
    else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (reviewInput(cursor, finalPage, ReviewButton::Confirm).execute) execute();
    } else
      return;
    requestUpdate();
    return;
  }
  int count = screen == Screen::Menu ? 2 : screen == Screen::Files ? files.size() : screen == Screen::Units ? 5 : 1;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    cursor = (cursor + 1) % std::max(1, count);
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    cursor = (cursor + std::max(1, count) - 1) % std::max(1, count);
    requestUpdate();
  }
  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;
  if (screen == Screen::Menu) {
    importing = cursor == 1;
    cursor = 0;
    selected = available = All;
    if (!importing) {
      screen = Screen::Units;
      requestUpdate();
      return;
    }
    files.clear();
    message = "この本の共有ファイルを確認中...";
    screen = Screen::Message;
    requestUpdateAndWait();
    bool incomplete = false;
    for (const auto& value : Storage.listFiles("/YomukaSync", 64)) {
      std::string name = value.c_str();
      const auto slash = name.find_last_of('/');
      if (slash != std::string::npos) name = name.substr(slash + 1);
      if (name.size() > 5 && name.substr(name.size() - 5) == ".json" && name.find('\\') == std::string::npos) {
        const std::string path = "/YomukaSync/" + name;
        JsonDocument candidate;
        const auto result = readSnapshotForBook(path, book, candidate);
        if (result == ExchangeError::None) files.push_back(path);
        if (result == ExchangeError::StorageFailure) incomplete = true;
      }
    }
    if (files.empty()) {
      fail(incomplete ? "確認できないファイルがあります。SDを確認して再試行" : "この本の共有ファイルが見つかりません");
      return;
    }
    std::sort(files.begin(), files.end());
    screen = Screen::Files;
  } else if (screen == Screen::Files)
    loadImport(files[cursor]);
  else if (screen == Screen::Units) {
    if (cursor == 4)
      makePreview();
    else if (available & (1 << cursor))
      selected ^= 1 << cursor;
  } else if (screen == Screen::Message && !blocked) {
    if (importComplete) {
      finishImport();
      return;
    }
    screen = Screen::Menu;
    cursor = 0;
    snapshot.clear();
  }
  requestUpdate();
}
void BookSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto heading = screen == Screen::Files ? renderer.truncatedText(UI_12_FONT_ID, ("共有: " + book.title).c_str(),
                                                                        renderer.getScreenWidth() - 30)
                                               : std::string("SDで読書データを共有");
  renderer.drawCenteredText(UI_12_FONT_ID, 15, heading.c_str());
  const int perPage = rowsPerPage(renderer.getScreenHeight());
  const int finalPage = finalReviewPage(lines.size(), perPage);
  const bool reviewing = screen == Screen::Confirm;
  const bool final = reviewing && cursor == finalPage;
  std::vector<std::string> rows;
  std::vector<int> kinds;
  if (screen == Screen::Menu)
    rows = {"この本のデータを書き出す", "この本のデータを取り込む"};
  else if (screen == Screen::Files)
    for (const auto& path : files) rows.push_back(path.substr(path.find_last_of('/') + 1));
  else if (screen == Screen::Units) {
    for (unsigned i = 0; i < 4; ++i)
      rows.push_back(
          std::string(!(available & (1 << i))
                          ? (importing && i == 2 && !snapshot["units"]["readerSettings"].isUnbound() ? "[フォント不足] "
                                                                                                     : "[対象外] ")
                      : selected & (1 << i) ? "[選択] "
                                            : "[  ] ") +
          labels[i]);
    rows.push_back(selected ? "変更内容を確認する" : "1種類以上選んでください");
  } else if (reviewing) {
    if (final) {
      rows.push_back("実行するデータ");
      kinds.push_back(0);
      for (unsigned i = 0; i < 4; ++i)
        if (selected & (1 << i)) {
          rows.push_back(labels[i]);
          kinds.push_back(i + 1);
        }
      rows.push_back("選んでいない種類は変更しません");
      kinds.push_back(0);
      rows.push_back(importing ? "確認して取り込む" : "確認して書き出す");
      kinds.push_back(0);
    } else {
      const int first = cursor * perPage, end = std::min<int>(lines.size(), first + perPage);
      rows.assign(lines.begin() + first, lines.begin() + end);
      kinds.assign(lineKinds.begin() + first, lineKinds.begin() + end);
    }
  } else if (importComplete) {
    rows = {message, selected & ReaderOverrides ? "設定反映のためHomeへ戻ります" : "取り込んだデータで本を開き直します",
            "決定で進みます"};
  } else
    rows = {message};
  const int first = reviewing ? 0 : (cursor / perPage) * perPage;
  for (int i = first; i < std::min<int>(rows.size(), first + perPage); ++i) {
    const int y = rowsTop + (i - first) * rowHeight;
    const bool active =
        reviewing ? final && i == static_cast<int>(rows.size()) - 1 : screen != Screen::Message && i == cursor;
    const int kind = reviewing ? kinds[i] : 0;
    if (active) renderer.fillRect(8, y, renderer.getScreenWidth() - 16, 30, true);
    if (kind == -1) renderer.drawRect(8, y, renderer.getScreenWidth() - 16, 30);
    if (kind > 0) {
      drawUnitIcon(renderer, kind - 1, 15, y + 5, !active);
      renderer.drawLine(12, y + 30, renderer.getScreenWidth() - 12, y + 30);
    }
    const int x = kind > 0 ? 39 : 15;
    const auto text = renderer.truncatedText(UI_10_FONT_ID, rows[i].c_str(), renderer.getScreenWidth() - x - 15);
    renderer.drawText(UI_10_FONT_ID, x, y + 2, text.c_str(), !active);
  }
  const auto page =
      std::to_string(reviewing ? cursor + 1 : cursor / perPage + 1) + " / " +
      std::to_string(reviewing ? finalPage + 1 : std::max(1, (static_cast<int>(rows.size()) + perPage - 1) / perPage));
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() - pageNumberBottom, page.c_str());
  const char* confirm = reviewing                               ? (final ? "実行" : "")
                        : screen == Screen::Units && cursor < 4 ? "選択切替"
                        : blocked                               ? ""
                                                                : "決定";
  const auto hints = mappedInput.mapLabels("戻る", confirm, reviewing ? "前頁" : "前", reviewing ? "次頁" : "次");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
