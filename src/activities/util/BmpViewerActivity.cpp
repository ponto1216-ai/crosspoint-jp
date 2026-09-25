#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>

#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();
  // Removed the redundant initial renderer.clearScreen()

  FsFile file;

  const auto layout = UiLayout::from(renderer);
  const auto pageWidth = layout.content.width;
  const auto pageHeight = layout.content.height;
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      float scale = std::min(static_cast<float>(pageWidth) / bitmap.getWidth(),
                             static_cast<float>(pageHeight) / bitmap.getHeight());
      scale = std::min(scale, 2.0f);
      const int renderedWidth = static_cast<int>(std::floor(bitmap.getWidth() * scale));
      const int renderedHeight = static_cast<int>(std::floor(bitmap.getHeight() * scale));
      const int x = layout.content.x + (pageWidth - renderedWidth) / 2;
      const int y = layout.content.y + (pageHeight - renderedHeight) / 2;

      // 4. Prepare Rendering
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      // Assuming drawBitmap defaults to 0,0 crop if omitted, or pass explicitly: drawBitmap(bitmap, x, y, pageWidth,
      // pageHeight, 0, 0)
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

      // Draw UI hints on the base layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      // Single pass for non-grayscale images

      renderer.displayBuffer(HalDisplay::HALF_REFRESH);

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredTextOffset(UI_10_FONT_ID, layout.content.y + pageHeight / 2, "Invalid BMP File", true,
                                      centerOffset);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, layout.content.y + pageHeight / 2, "Could not open file", true,
                                    centerOffset);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }
}
