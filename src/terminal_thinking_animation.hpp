#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace juno::sdk::detail {

class TerminalThinkingAnimation {
public:
  explicit TerminalThinkingAnimation(std::size_t word = 0) : word_(word) {}

  std::string_view label() const { return kLabels[word_]; }

  std::string_view suffix() const { return kDots[dots_]; }

  void restart_word() { dots_ = 0; }

  void advance() {
    dots_ = (dots_ + 1) % kDots.size();
    if (dots_ == 0)
      word_ = next_word();
  }

  std::size_t next_word() const { return (word_ + 1) % kLabels.size(); }

private:
  static constexpr std::array<std::string_view, 4> kDots{"", ".", "..", "..."};
  // Includes non-English entries with at least 50 million total speakers in Ethnologue's 2026
  // ranking.
  static constexpr auto kLabels = std::to_array<std::string_view>({
      "Thinking",                      // English
      "Pensando",                      // Spanish and Portuguese
      "正在思考",                      // Mandarin Chinese
      "सोच रहा हूँ",                     // Hindi
      "أفكّر",                          // Modern Standard Arabic
      "Je réfléchis",                  // French
      "ভাবছি",                         // Bengali
      "Sedang berpikir",               // Indonesian
      "سوچ رہا ہوں",                   // Urdu
      "Думаю",                         // Russian
      "Denke nach",                    // German
      "考え中",                        // Japanese
      "I dey think",                   // Nigerian Pidgin
      "بفكّر",                          // Egyptian Arabic
      "विचार करत आहे",                  // Marathi
      "Đang suy nghĩ",                 // Vietnamese
      "ఆలోచిస్తున్నాను",                    // Telugu
      "Ninafikiria",                   // Swahili
      "Ina tunani",                    // Hausa
      "Düşünüyorum",                   // Turkish
      "سوچ ریا واں",                   // Western Punjabi
      "Nag-iisip",                     // Tagalog
      "யோசிக்கிறேன்",                    // Tamil
      "諗緊",                          // Yue Chinese (Cantonese)
      "辣海想",                        // Wu Chinese (Shanghainese)
      "دارم فکر می‌کنم", // Iranian Persian
      "생각 중",                       // Korean
      "እያሰብኩ ነው",                      // Amharic
      "กำลังคิด",                        // Thai
      "Lagi mikir",                    // Javanese
      "Sto pensando",                  // Italian
      "વિચારી રહ્યો છું",                 // Gujarati
      "ಯೋಚಿಸುತ್ತಿದ್ದೇನೆ",                   // Kannada
      "عم فكّر",                        // Levantine Arabic
      "قاعد أفكّر",                     // Sudanese Arabic
      "Mo ń ronú",                     // Yoruba
      "सोचत बानी",                     // Bhojpuri
  });

  std::size_t word_;
  std::size_t dots_{};
};

} // namespace juno::sdk::detail
