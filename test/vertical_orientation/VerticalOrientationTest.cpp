#include <cassert>

#include <VerticalOrientationData.h>

using VerticalTextUtils::UaxVerticalOrientation;

int main() {
  // UAX #50 examples that are easy to mistake for a rendering bug.
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x2190) == UaxVerticalOrientation::Rotated);  // ←
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x03B1) == UaxVerticalOrientation::Rotated);  // α
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x0410) == UaxVerticalOrientation::Rotated);  // А
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x00C5) == UaxVerticalOrientation::Rotated);  // Å

  assert(VerticalTextUtils::isUaxUprightInVertical(0x2460));  // ①
  assert(VerticalTextUtils::isUaxUprightInVertical(0x2160));  // Ⅰ
  assert(VerticalTextUtils::isUaxUprightInVertical(0x2103));  // ℃
  assert(VerticalTextUtils::isUaxUprightInVertical(0x2113));  // ℓ
  assert(VerticalTextUtils::isUaxUprightInVertical(0x2116));  // №

  assert(VerticalTextUtils::getUaxVerticalOrientation(0x3001) == UaxVerticalOrientation::TransformedUpright);  // 、
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x30FC) == UaxVerticalOrientation::TransformedRotated);  // ー
  assert(VerticalTextUtils::getUaxVerticalOrientation(0xFF70) == UaxVerticalOrientation::Rotated);              // ｰ
}
