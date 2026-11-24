#pragma once

#include <ostream>
#include <string>
#include <string_view>

namespace juno::sdk::detail {

/** Display-width estimate for UTF-8 text. The terminal handles wide glyphs and long words. */
std::size_t terminal_text_width(std::string_view text);
std::string terminal_text(std::string_view text, bool multiline = false);

/** Streams the supported Markdown subset one complete line at a time. */
class TerminalMarkdown {
public:
  TerminalMarkdown(std::ostream &output, bool color, std::size_t width);
  void append(std::string_view delta);
  void finish();

private:
  void write_line(std::string_view line);

  std::ostream &output_;
  bool color_;
  std::size_t width_;
  std::string pending_;
  char fence_{};
  std::size_t fence_length_{};
};

} // namespace juno::sdk::detail
