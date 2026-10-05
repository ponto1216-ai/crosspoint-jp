#include "SnapshotPreview.h"

#include <cstring>
#include <ctime>
namespace yomuka::sync {
std::string previewDate(JsonVariantConst value) {
  const time_t date = value.as<uint32_t>();
  if (!date) return "日時不明";
  struct tm local{};
  localtime_r(&date, &local);
  char text[32];
  strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
  return text;
}
namespace {
struct Field {
  const char* key;
  const char* label;
};
constexpr Field globals[] = {{"writingMode", "書字方向"},
                             {"orientation", "画面の向き"},
                             {"bookStyle", "本のスタイル"},
                             {"imageRendering", "画像表示"},
                             {"invertImages", "画像の白黒反転"}};
constexpr Field directionFields[] = {
    {"font", "フォント"},          {"fontSize", "文字サイズ"},           {"lineSpacing", "行間"},
    {"charSpacing", "字間"},       {"paragraphAlignment", "段落の配置"}, {"extraParagraphSpacing", "段落間隔"},
    {"screenMargin", "余白"},      {"firstLineIndent", "字下げ"},        {"rubyEnabled", "ルビ"},
    {"rubyOffsetX", "ルビ横位置"}, {"rubyOffsetY", "ルビ縦位置"},        {"tateChuYokoMaxDigits", "縦中横の桁数"}};
std::string valueLabel(JsonVariantConst value) {
  if (value.isUnbound()) return "全体設定を継承";
  if (value.isNull()) return "不明";
  if (value.is<bool>()) return value.as<bool>() ? "有効" : "無効";
  if (value.is<JsonObjectConst>()) {
    const char* name = value["sdFamilyName"].as<const char*>();
    if (name && *name) return std::string("SD: ") + name + "（" + valueLabel(value["family"]) + "）";
    return std::string("組み込み: ") + valueLabel(value["family"]);
  }
  if (value.is<const char*>()) {
    const char* key = value.as<const char*>();
    constexpr Field values[] = {{"auto", "自動"},
                                {"horizontal", "横書き"},
                                {"vertical", "縦書き"},
                                {"portrait", "縦向き"},
                                {"landscape-cw", "横向き（右回転）"},
                                {"landscape-ccw", "横向き（左回転）"},
                                {"inverted", "上下反転"},
                                {"crosspoint", "CrossPoint優先"},
                                {"epub", "EPUB優先"},
                                {"balanced", "バランス"},
                                {"display", "表示"},
                                {"placeholder", "枠のみ"},
                                {"suppress", "非表示"},
                                {"small", "小"},
                                {"medium", "中"},
                                {"large", "大"},
                                {"extra-large", "特大"},
                                {"justified", "両端揃え"},
                                {"left", "左揃え"},
                                {"center", "中央揃え"},
                                {"right", "右揃え"},
                                {"noto-sans", "Noto Sans"},
                                {"noto-serif", "Noto Serif"},
                                {"open-dyslexic", "OpenDyslexic"}};
    for (const auto& entry : values)
      if (std::strcmp(key, entry.key) == 0) return entry.label;
    return key;
  }
  std::string text;
  serializeJson(value, text);
  return text;
}
std::string position(JsonVariantConst p, uint32_t spines) {
  if (p["spineIndex"].as<uint32_t>() == spines) return "本の終端";
  return "章 " + std::to_string(p["spineIndex"].as<unsigned>() + 1) + " / ページ " +
         std::to_string(p["chapterPage"].as<unsigned>() + 1);
}
}  // namespace
std::vector<std::string> previewUnit(unsigned unit, JsonVariantConst current, JsonVariantConst incoming,
                                     uint32_t spines, bool importing) {
  std::vector<std::string> lines;
  const auto old = current["data"], next = incoming["data"];
  auto pair = [&](const char* label, const std::string& before, const std::string& after) {
    lines.push_back(std::string(label) + ": " + (importing ? before + " → " : "") + after);
  };
  if (unit == 0) {
    pair("読書位置", current.isUnbound() ? "なし" : position(old, spines), position(next, spines));
    pair("進捗率", valueLabel(old["percent"]), valueLabel(next["percent"]));
    pair("完読", valueLabel(old["finished"]), valueLabel(next["finished"]));
    lines.emplace_back("受信側の表示設定に合わせて位置を近似");
  } else if (unit == 1) {
    pair("しおりの件数", std::to_string(old.size()), std::to_string(next.size()));
    if (importing) lines.emplace_back(next.size() ? "現在のしおり一覧を受信一覧で置換" : "現在のしおりをすべて削除");
    auto bookmarks = [&](JsonVariantConst data, const char* heading) {
      if (!data.size()) return;
      lines.emplace_back(heading);
      for (JsonVariantConst item : data.as<JsonArrayConst>()) {
        lines.push_back(position(item, spines));
        const std::string summary = item["summary"].as<const char*>();
        size_t end = 0, chars = 0;
        while (end < summary.size() && chars < 64) {
          ++end;
          while (end < summary.size() && (static_cast<unsigned char>(summary[end]) & 0xc0) == 0x80) ++end;
          ++chars;
        }
        lines.push_back(summary.substr(0, end) + (end < summary.size() ? "…" : ""));
      }
    };
    if (importing) bookmarks(old, "現在のしおり（置換対象・本文は抜粋）");
    bookmarks(next, importing ? "受信のしおり（本文は抜粋）" : "送信のしおり（本文は抜粋）");
  } else if (unit == 2) {
    if (importing)
      lines.emplace_back(next.size() ? "受信にない設定は解除して全体設定を継承"
                                     : "本の設定をすべて解除（全体設定を継承）");
    size_t changes = 0;
    auto field = [&](const Field& item, JsonVariantConst before, JsonVariantConst after, const char* prefix) {
      // Font objects compare by their values, independent of JSON member order.
      const auto b = valueLabel(before[item.key]), a = valueLabel(after[item.key]);
      if ((importing && b == a) || (!importing && after[item.key].isUnbound())) return;
      ++changes;
      const auto label = std::string(prefix) + item.label;
      pair(label.c_str(), b, a);
    };
    for (const auto& item : globals) field(item, old, next, "");
    for (const char* d : {"horizontal", "vertical"})
      for (const auto& item : directionFields)
        field(item, old[d], next[d], std::strcmp(d, "vertical") == 0 ? "縦書き / " : "横書き / ");
    if (!changes) lines.emplace_back(importing ? "設定値の変更なし" : "本固有の設定なし（全体設定を継承）");
  } else if (unit == 3) {
    pair("読書時間（秒）", valueLabel(old["seconds"]), valueLabel(next["seconds"]));
    pair("読書回数", valueLabel(old["sessionCount"]), valueLabel(next["sessionCount"]));
    pair("最終読書日時", previewDate(old["lastReadAt"]), previewDate(next["lastReadAt"]));
    pair("完読", valueLabel(old["finished"]), valueLabel(next["finished"]));
    pair("完読日時", previewDate(old["finishedAt"]), previewDate(next["finishedAt"]));
    lines.emplace_back("本別の要約を置換（加算なし）");
    lines.emplace_back("端末全体・日別の累計は変えません");
  }
  if (importing) {
    lines.push_back("現在の更新: " + previewDate(current["updatedAt"]));
    const uint32_t a = current["updatedAt"], b = incoming["updatedAt"];
    if (!a || !b)
      lines.emplace_back("日時不明を含むため新旧は判定しません");
    else if (b < a)
      lines.emplace_back("注意: 受信の更新日時が古いデータです");
  }
  lines.push_back(std::string(importing ? "受信の更新: " : "送信の更新: ") + previewDate(incoming["updatedAt"]));
  return lines;
}
}  // namespace yomuka::sync
