#include "ConfirmationActivity.h"

#include <I18n.h>

#include "../../components/UITheme.h"
#include "../../components/UiLayout.h"
#include "../ActivityResult.h"
#include "HalDisplay.h"
#include "Utf8.h"

namespace {

bool isPreferredWrapBreak(const std::string& character) {
  return character == " " || character == "\t" || character == "、" || character == "。" || character == "！" ||
         character == "？";
}

void trimLeadingSpaces(std::string& text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.erase(text.begin());
}

std::vector<std::string> wrapConfirmationText(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                              const int maxWidth, const int maxLines) {
  std::vector<std::string> lines;
  std::string current;
  size_t preferredBreak = std::string::npos;
  const auto* cursor = reinterpret_cast<const unsigned char*>(text.c_str());

  while (*cursor != '\0') {
    const auto* characterStart = cursor;
    utf8NextCodepoint(&cursor);
    const std::string character(reinterpret_cast<const char*>(characterStart),
                                static_cast<size_t>(cursor - characterStart));
    if (character == "\n") {
      if (!current.empty()) lines.push_back(current);
      current.clear();
      preferredBreak = std::string::npos;
      continue;
    }
    const std::string candidate = current + character;
    if (!current.empty() && renderer.getTextWidth(fontId, candidate.c_str(), EpdFontFamily::REGULAR) > maxWidth) {
      if (preferredBreak != std::string::npos) {
        lines.push_back(current.substr(0, preferredBreak));
        current.erase(0, preferredBreak);
        trimLeadingSpaces(current);
      } else {
        lines.push_back(current);
        current.clear();
      }
      if (static_cast<int>(lines.size()) == maxLines - 1) {
        const std::string remainder = current + character + reinterpret_cast<const char*>(cursor);
        lines.push_back(renderer.truncatedText(fontId, remainder.c_str(), maxWidth, EpdFontFamily::REGULAR));
        return lines;
      }
      preferredBreak = std::string::npos;
    }
    current += character;
    if (isPreferredWrapBreak(character)) preferredBreak = current.size();
  }

  if (!current.empty() && static_cast<int>(lines.size()) < maxLines) lines.push_back(current);
  return lines;
}

}  // namespace

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           const std::string& neverLabel, const std::string& confirmLabel,
                                           const std::string& backLabel, const std::string& confirmMiddleLabel)
    : Activity("Confirmation", renderer, mappedInput),
      heading(heading),
      body(body),
      neverLabel(neverLabel),
      confirmLabel(confirmLabel),
      backLabel(backLabel),
      confirmMiddleLabel(confirmMiddleLabel) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const auto layout = UiLayout::from(renderer);
  const int maxWidth = layout.content.width - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  safeBodyLines.clear();
  if (!body.empty()) {
    safeBodyLines = wrapConfirmationText(renderer, fontId, body, maxWidth, 6);
  }

  int totalHeight = 0;
  if (!safeHeading.empty()) totalHeight += lineHeight;
  if (!safeBodyLines.empty()) totalHeight += lineHeight * safeBodyLines.size();
  if (!safeHeading.empty() && !safeBodyLines.empty()) totalHeight += spacing;

  startY = layout.content.y + (layout.content.height - totalHeight) / 2;

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  const auto layout = UiLayout::from(renderer);
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredTextOffset(fontId, currentY, safeHeading.c_str(), true, centerOffset, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  for (const auto& line : safeBodyLines) {
    renderer.drawCenteredTextOffset(fontId, currentY, line.c_str(), true, centerOffset, EpdFontFamily::REGULAR);
    currentY += lineHeight;
  }

  // Draw UI Elements
  const char* confirmText = confirmLabel.empty() ? I18N.get(StrId::STR_CONFIRM) : confirmLabel.c_str();
  const char* backText =
      backLabel.empty() ? (neverLabel.empty() ? "" : I18N.get(StrId::STR_CLOSE_BOOK)) : backLabel.c_str();
  const char* middleText = confirmMiddleLabel.empty() ? "" : confirmMiddleLabel.c_str();
  const auto labels = neverLabel.empty()
                          ? mappedInput.mapLabels(backText, middleText, I18N.get(StrId::STR_CANCEL), confirmText)
                          : mappedInput.mapLabels(backText, middleText, neverLabel.c_str(), confirmText);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (!confirmMiddleLabel.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    ActivityResult res;
    res.isCancelled = true;
    res.data = MenuResult{RESULT_MIDDLE};
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (!neverLabel.empty()) {
      // "Never" option (don't ask again)
      ActivityResult res;
      res.isCancelled = true;
      res.data = MenuResult{RESULT_NEVER};
      setResult(std::move(res));
    } else {
      // Standard cancel
      ActivityResult res;
      res.isCancelled = true;
      setResult(std::move(res));
    }
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Close / cancel
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}
