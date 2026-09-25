#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

namespace {
constexpr int kTitleY = 15;
constexpr int kListY = 60;
constexpr int kLineHeight = 30;
}  // namespace

int XtcReaderChapterSelectionActivity::getPageItems() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const bool portraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int topHintGutter = portraitInverted ? metrics.buttonHintsHeight + metrics.verticalSpacing : 0;
  const int listTop = layout.content.y + kListY + topHintGutter;
  const int bottomHints = layout.landscape ? 0 : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int listBottom = layout.content.y + layout.content.height - bottomHints;
  return std::max(1, (listBottom - listTop) / kLineHeight);
}

int XtcReaderChapterSelectionActivity::findChapterIndexForPage(uint32_t page) const {
  if (!xtc) {
    return 0;
  }

  const auto& chapters = xtc->getChapters();
  for (size_t i = 0; i < chapters.size(); i++) {
    if (page >= chapters[i].startPage && page <= chapters[i].endPage) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void XtcReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  renderer.setOrientation(readerOrientation);
  mappedInput.setEffectiveOrientation(OrientationHelper::toInputOrientation(readerOrientation));

  if (!xtc) {
    return;
  }

  selectorIndex = findChapterIndexForPage(currentPage);

  requestUpdate();
}

void XtcReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void XtcReaderChapterSelectionActivity::loop() {
  const int pageItems = getPageItems();
  const int totalItems = static_cast<int>(xtc->getChapters().size());

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& chapters = xtc->getChapters();
    if (!chapters.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(chapters.size())) {
      setResult(PageResult{chapters[selectorIndex].startPage});
      finish();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }

  buttonNavigator.onNextRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void XtcReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const bool portraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int topHintGutter = portraitInverted ? metrics.buttonHintsHeight + metrics.verticalSpacing : 0;
  const int titleY = layout.content.y + kTitleY + topHintGutter;
  const int listY = layout.content.y + kListY + topHintGutter;
  const int pageItems = getPageItems();
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  renderer.drawCenteredTextOffset(UI_12_FONT_ID, titleY, tr(STR_SELECT_CHAPTER), true, centerOffset,
                                  EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  const auto& chapters = xtc->getChapters();
  if (chapters.empty()) {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, listY + 60, tr(STR_NO_CHAPTERS), true, centerOffset);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(layout.content.x, listY + (selectorIndex % pageItems) * kLineHeight - 2, layout.content.width - 1,
                    kLineHeight);
  for (int i = pageStartIndex; i < static_cast<int>(chapters.size()) && i < pageStartIndex + pageItems; i++) {
    const auto& chapter = chapters[i];
    const char* title = chapter.name.empty() ? tr(STR_UNNAMED) : chapter.name.c_str();
    const std::string chapterName = renderer.truncatedText(UI_10_FONT_ID, title, layout.content.width - 40);
    renderer.drawText(UI_10_FONT_ID, layout.content.x + 20, listY + (i % pageItems) * kLineHeight, chapterName.c_str(),
                      i != selectorIndex);
  }

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
