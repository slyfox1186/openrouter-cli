#include "display.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace orcli {
namespace {

constexpr unsigned char kFirstPrintable = 0x20;
constexpr unsigned char kDelete = 0x7F;
constexpr unsigned char kUtf8ContinuationMask = 0xC0;
constexpr unsigned char kUtf8ContinuationBits = 0x80;

bool is_unsafe_control(char symbol) {
    const auto code = static_cast<unsigned char>(symbol);
    return (code < kFirstPrintable && symbol != '\n' && symbol != '\t') || code == kDelete;
}

bool is_utf8_continuation(char symbol) {
    return (static_cast<unsigned char>(symbol) & kUtf8ContinuationMask) == kUtf8ContinuationBits;
}

std::size_t utf8_sequence_length(unsigned char byte) {
    constexpr unsigned char first_two_byte = 0xc2;
    constexpr unsigned char first_three_byte = 0xe0;
    constexpr unsigned char first_four_byte = 0xf0;
    constexpr unsigned char last_four_byte = 0xf4;
    constexpr std::size_t two_bytes = 2;
    constexpr std::size_t three_bytes = 3;
    constexpr std::size_t four_bytes = 4;
    if (byte >= first_four_byte && byte <= last_four_byte) {
        return four_bytes;
    }
    if (byte >= first_three_byte && byte < first_four_byte) {
        return three_bytes;
    }
    if (byte >= first_two_byte && byte < first_three_byte) {
        return two_bytes;
    }
    return 1;
}

bool valid_utf8_sequence(std::string_view sequence, std::size_t length) {
    if (sequence.size() != length ||
        !std::ranges::all_of(sequence.substr(1), is_utf8_continuation)) {
        return false;
    }
    constexpr unsigned char shortest_three_byte = 0xe0;
    constexpr unsigned char surrogate_lead = 0xed;
    constexpr unsigned char shortest_four_byte = 0xf0;
    constexpr unsigned char longest_four_byte = 0xf4;
    constexpr unsigned char three_byte_min_second = 0xa0;
    constexpr unsigned char surrogate_min_second = 0xa0;
    constexpr unsigned char four_byte_min_second = 0x90;
    const auto lead = static_cast<unsigned char>(sequence.front());
    const auto second = static_cast<unsigned char>(sequence.at(1));
    return (lead != shortest_three_byte || second >= three_byte_min_second) &&
           (lead != surrogate_lead || second < surrogate_min_second) &&
           (lead != shortest_four_byte || second >= four_byte_min_second) &&
           (lead != longest_four_byte || second < four_byte_min_second);
}

std::string truncate_utf8(std::string text, std::size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    std::size_t cut = max_chars;
    while (cut > 0 && is_utf8_continuation(text.at(cut))) {
        --cut;
    }
    text.resize(cut);
    text += "...";
    return text;
}

} // namespace

std::string sanitize_for_display(std::string_view text, std::size_t max_chars) {
    std::string clean(text);
    std::ranges::replace_if(clean, is_unsafe_control, ' ');
    return truncate_utf8(std::move(clean), max_chars);
}

std::string display_metadata(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    constexpr unsigned char first_c1 = 0x80;
    constexpr unsigned char last_c1 = 0x9f;
    constexpr unsigned char c1_lead = 0xc2;
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text.at(index));
        if (byte < kFirstPrintable || byte == kDelete || (byte >= first_c1 && byte <= last_c1)) {
            clean += ' ';
            ++index;
            continue;
        }
        if (byte == c1_lead && index + 1 < text.size()) {
            const auto next = static_cast<unsigned char>(text.at(index + 1));
            if (next >= first_c1 && next <= last_c1) {
                clean += ' ';
                index += 2;
                continue;
            }
        }
        const std::size_t length = utf8_sequence_length(byte);
        const auto sequence = text.substr(index, length);
        if (length > 1 && valid_utf8_sequence(sequence, length)) {
            clean += sequence;
            index += length;
        } else {
            clean += byte >= first_c1 ? ' ' : text.at(index);
            ++index;
        }
    }
    return clean;
}

} // namespace orcli
