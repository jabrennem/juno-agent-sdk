#include "terminal_markdown.hpp"

#include <algorithm>
#include <vector>

namespace juno::sdk::detail {
namespace {

enum class Style { Plain, Bold, Code };

struct StyledText {
  std::string text;
  std::vector<Style> styles;

  void append(std::string_view part, Style style) {
    text.append(part);
    styles.insert(styles.end(), part.size(), style);
  }

  void write(std::ostream &output, std::size_t first, std::size_t last, bool color) const {
    if (!color) {
      output << std::string_view(text).substr(first, last - first);
      return;
    }
    while (first < last) {
      const auto style = styles[first];
      auto end = first + 1;
      while (end < last && styles[end] == style)
        ++end;
      if (style == Style::Bold)
        output << "\033[1m";
      else if (style == Style::Code)
        output << "\033[36m";
      output << std::string_view(text).substr(first, end - first);
      if (style != Style::Plain)
        output << "\033[0m";
      first = end;
    }
  }
};

std::size_t marker_count(std::string_view text, char marker) {
  const auto end = text.find_first_not_of(marker);
  return end == text.npos ? text.size() : end;
}

std::size_t inline_code_end(std::string_view text, std::size_t start, std::size_t count) {
  while ((start = text.find('`', start)) != text.npos) {
    const auto run = marker_count(text.substr(start), '`');
    if (run == count)
      return start;
    start += run;
  }
  return text.npos;
}

std::size_t bold_marker_end(std::string_view text, std::size_t start) {
  for (auto index = start; index < text.size(); ++index) {
    const auto rest = text.substr(index);
    if (rest.front() == '\\' && rest.size() > 1) {
      ++index;
    } else if (rest.front() == '`') {
      const auto count = marker_count(rest, '`');
      const auto end = inline_code_end(text, index + count, count);
      index = end == text.npos ? index + count - 1 : end + count - 1;
    } else if (rest.starts_with("**") && index > start && text[index - 1] != ' ') {
      return index;
    }
  }
  return text.npos;
}

StyledText inline_text(std::string_view line, Style base) {
  StyledText result;
  auto bold_end = line.npos;
  for (std::size_t index = 0; index < line.size();) {
    const auto rest = line.substr(index);
    if (index == bold_end) {
      bold_end = line.npos;
      index += 2;
      continue;
    }
    const auto style = bold_end == line.npos ? base : Style::Bold;
    if (rest.size() > 1 && rest.front() == '\\' &&
        std::string_view("\\*`").find(rest[1]) != line.npos) {
      result.append(rest.substr(1, 1), style);
      index += 2;
      continue;
    }
    if (rest.front() == '`') {
      const auto count = marker_count(rest, '`');
      const auto end = inline_code_end(line.substr(0, bold_end), index + count, count);
      if (end != line.npos) {
        result.append(line.substr(index + count, end - index - count), Style::Code);
        index = end + count;
        continue;
      }
      result.append(rest.substr(0, count), style);
      index += count;
      continue;
    }
    if (bold_end == line.npos && rest.starts_with("**") && rest.size() > 2 && rest[2] != ' ' &&
        rest[2] != '*') {
      const auto end = bold_marker_end(line, index + 2);
      if (end != line.npos) {
        bold_end = end;
        index += 2;
        continue;
      }
    }
    result.append(rest.substr(0, 1), style);
    ++index;
  }
  return result;
}

void write_wrapped(std::ostream &output, const StyledText &line, std::string_view prefix,
                   bool color, std::size_t width) {
  const auto prefix_width = terminal_text_width(prefix);
  output << prefix;
  if (width <= prefix_width || prefix.find('\t') != prefix.npos ||
      line.text.find('\t') != line.text.npos) {
    line.write(output, 0, line.text.size(), color);
    return;
  }

  std::size_t column = prefix_width;
  const auto text = std::string_view(line.text);
  std::size_t segment_start = 0;
  std::size_t first = 0;
  while (first < text.size()) {
    const auto word = text.find_first_not_of(' ', first);
    if (word == text.npos)
      break;
    auto end = word;
    while (end < text.size() && (text[end] != ' ' || line.styles[end] == Style::Code))
      ++end;
    const auto word_width = terminal_text_width(text.substr(word, end - word));
    if (column > prefix_width && column + (word - first) + word_width > width) {
      line.write(output, segment_start, first, color);
      output << '\n' << std::string(prefix_width, ' ');
      segment_start = word;
      first = word;
      column = prefix_width;
    }
    column += word - first + word_width;
    first = end;
  }
  line.write(output, segment_start, first, color);
}

} // namespace

std::size_t terminal_text_width(std::string_view text) {
  return std::count_if(text.begin(), text.end(),
                       [](unsigned char byte) { return (byte & 0xc0) != 0x80; });
}

std::string terminal_text(std::string_view text, bool multiline) {
  std::string result;
  result.reserve(text.size());
  for (const unsigned char byte : text) {
    if (byte >= 0x20 && byte != 0x7f)
      result.push_back(static_cast<char>(byte));
    else if (byte == '\r' || byte == '\n' || byte == '\t') {
      if (multiline && byte != '\r')
        result.push_back(static_cast<char>(byte));
      else if (!multiline)
        result.push_back(' ');
    }
  }
  return result;
}

TerminalMarkdown::TerminalMarkdown(std::ostream &output, bool color, std::size_t width)
    : output_(output), color_(color), width_(width) {}

void TerminalMarkdown::append(std::string_view delta) {
  for (auto end = delta.find('\n'); end != delta.npos; end = delta.find('\n')) {
    pending_.append(delta.substr(0, end));
    write_line(pending_);
    pending_.clear();
    output_ << std::flush;
    delta.remove_prefix(end + 1);
  }
  pending_.append(delta);
}

void TerminalMarkdown::finish() {
  if (!pending_.empty())
    write_line(pending_);
  pending_.clear();
  fence_ = {};
  fence_length_ = 0;
}

void TerminalMarkdown::write_line(std::string_view raw) {
  const auto clean = terminal_text(raw, true);
  auto line = std::string_view(clean);
  auto indent = line.find_first_not_of(' ');
  if (indent == line.npos)
    indent = line.size();
  const auto content = line.substr(indent);
  if (indent <= 3 && !content.empty() && (content.front() == '`' || content.front() == '~')) {
    const auto marker = content.front();
    const auto count = marker_count(content, marker);
    if (!fence_ && count >= 3) {
      fence_ = marker;
      fence_length_ = count;
      return;
    }
    if (marker == fence_ && count >= fence_length_ &&
        content.substr(count).find_first_not_of(" \t") == line.npos) {
      fence_ = {};
      return;
    }
  }
  if (fence_ || indent >= 4 || line.starts_with('\t')) {
    output_ << line << '\n';
    return;
  }

  std::string prefix(line.substr(0, indent));
  line = content;
  auto base = Style::Plain;
  const auto hashes = marker_count(line, '#');
  if (hashes > 0 && hashes <= 6 && hashes < line.size() && line[hashes] == ' ') {
    line.remove_prefix(hashes + 1);
    base = Style::Bold;
  } else if (line.size() >= 2 && line[1] == ' ' &&
             std::string_view("-*+").find(line.front()) != line.npos) {
    prefix += "• ";
    line.remove_prefix(2);
  } else {
    const auto digits = line.find_first_not_of("0123456789");
    if (digits != line.npos && digits > 0 && digits + 1 < line.size() &&
        (line[digits] == '.' || line[digits] == ')') && line[digits + 1] == ' ') {
      prefix += line.substr(0, digits + 2);
      line.remove_prefix(digits + 2);
    }
  }
  write_wrapped(output_, inline_text(line, base), prefix, color_, width_);
  output_ << '\n';
}

} // namespace juno::sdk::detail
