#pragma once

#include <HalStorage.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CssStyle.h"

class CssSelectorUsage;

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */
class CssParser {
 public:
  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  static constexpr uint8_t CSS_CACHE_VERSION = 12;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() { abortCacheWrite(); }

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return true if parsing completed (even if no rules found). Returns false
   *         if parsing stopped early due to low heap; rules collected so far
   *         remain loaded, but must not be saved as a complete cache.
   */
  bool loadFromStream(FsFile& source);

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(const std::string& tagName, const std::string& classAttr) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(const std::string& styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const { return rulesBySelector_.empty(); }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const { return rulesBySelector_.size(); }

  /**
   * Clear all loaded rules
   */
  void clear() { rulesBySelector_.clear(); nextRuleOrder_ = 0; }

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache() const;

  // Stream normalized rule records in source order. Repeated selectors are
  // merged when loading, so no whole-book rule map is needed during parsing.
  bool beginCacheWrite();
  bool finishCacheWrite();
  void abortCacheWrite();

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return true if cache was loaded successfully
   */
  bool loadFromCache(size_t minFreeHeapAfterLoad = 0, const CssSelectorUsage* usage = nullptr,
                     bool requireComplete = false);

  // Check cache identity without materializing its rules in RAM. Rules are
  // loaded only while building the section that needs them.
  bool validateCache() const;

  // The EPUB archive fingerprint binds this serialized cache to its source.
  void setCacheSourceFingerprint(uint64_t fingerprint) { cacheSourceFingerprint_ = fingerprint; }

 private:
  // Keep each declaration block: merging equal selectors loses their order
  // relative to intervening rules with the same specificity.
  struct OrderedRule { CssStyle style; uint32_t order; };
  std::unordered_map<std::string, std::vector<OrderedRule>> rulesBySelector_;
  uint32_t nextRuleOrder_ = 0;

  std::string cachePath;
  uint64_t cacheSourceFingerprint_ = 0;
  FsFile cacheWriteFile_;
  bool cacheWriteActive_ = false;
  bool cacheWriteFailed_ = false;
  uint16_t cacheWriteCount_ = 0;
  static bool writeCachedRule(FsFile& file, const std::string& selector, const CssStyle& style);

  // Internal parsing helpers
  void processRuleBlockWithStyle(const std::string& selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(const std::string& declBlock);
  static void parseDeclarationIntoStyle(const std::string& decl, CssStyle& style, std::string& propNameBuf,
                                        std::string& propValueBuf);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(const std::string& val);
  static CssFontStyle interpretFontStyle(const std::string& val);
  static CssFontWeight interpretFontWeight(const std::string& val);
  static CssTextDecoration interpretDecoration(const std::string& val);
  static CssLength interpretLength(const std::string& val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(const std::string& val, CssLength& out);

  // String utilities
  static std::string normalized(const std::string& s);
  static void normalizedInto(const std::string& s, std::string& out);
  static std::vector<std::string> splitOnChar(const std::string& s, char delimiter);
  static std::vector<std::string> splitWhitespace(const std::string& s);
};
