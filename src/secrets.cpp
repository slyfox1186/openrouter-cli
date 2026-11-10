#include "secrets.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orcli {
namespace {

// Hand-written matchers instead of std::regex: libstdc++'s regex executor recurses once per
// repeated character, so a long token-like line (minified code, base64 data) can overflow the
// stack.

constexpr std::size_t kNotFound = std::string_view::npos;

constexpr std::string_view kUpper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::string_view kLower = "abcdefghijklmnopqrstuvwxyz";
constexpr std::string_view kHex = "0123456789abcdefABCDEF";
constexpr std::string_view kUpperDigits = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
constexpr std::string_view kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr std::string_view kAlnum =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
constexpr std::string_view kWordChars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";
constexpr std::string_view kTokenChars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
constexpr std::string_view kSlackChars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-";
constexpr std::string_view kNameChars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.";
constexpr std::string_view kKeyLabelChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ ";
constexpr std::string_view kSchemeChars = "abcdefghijklmnopqrstuvwxyz0123456789+.-";
constexpr std::string_view kSpace = " \t\n\r\f\v";
constexpr std::string_view kBlanks = " \t";
constexpr std::string_view kQuotes = "\"'`";
constexpr std::string_view kUserStop = ":@/ \t\n\r\f\v";
constexpr std::string_view kPasswordStop = "@/ \t\n\r\f\v";
// Code punctuation ends an unquoted value, which separates `KEY=value` lines from expressions
// such as `api_key=os.environ["KEY"]`.
constexpr std::string_view kEnvValueStop = " \t\n\r\f\v\"'`()[]{}<>;,";

constexpr std::size_t kMinUrlPassword = 3;
constexpr std::size_t kMinQuotedSecret = 12;
constexpr std::size_t kMinUnquotedSecret = 16;
constexpr std::size_t kMinJwtSegment = 10;
constexpr std::string_view kRedacted = "[redacted]";
constexpr std::string_view kExport = "export";

constexpr unsigned char kFirstPrintable = 0x20;
constexpr unsigned char kDelete = 0x7F;
constexpr unsigned char kUtf8ContinuationMask = 0xC0;
constexpr unsigned char kUtf8ContinuationBits = 0x80;

// Half-open byte range of a secret value within the scanned text.
struct Span {
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct Match {
    std::string_view pattern;
    Span span;
};

constexpr std::size_t kMaxPrefixes = 5;

// A literal prefix followed by a run of `body` characters, with optional ECMAScript-style \b
// before the prefix and after the run.
struct TokenRule {
    std::string_view name;
    std::array<std::string_view, kMaxPrefixes> prefixes{};
    std::string_view body;
    std::size_t min_length = 0;
    bool exact = false;
    bool lead_boundary = false;
    bool trail_boundary = false;
};

constexpr std::array kTokenRules{
    TokenRule{.name = "openrouter-key", .prefixes = {"sk-or-v1-"}, .body = kHex, .min_length = 64},
    TokenRule{
        .name = "anthropic-key",
        .prefixes = {"sk-ant-"},
        .body = kTokenChars,
        .min_length = 20,
    },
    TokenRule{
        .name = "openai-key",
        .prefixes = {"sk-proj-", "sk-svcacct-"},
        .body = kTokenChars,
        .min_length = 20,
    },
    TokenRule{
        .name = "aws-access-key",
        .prefixes = {"AKIA", "ASIA"},
        .body = kUpperDigits,
        .min_length = 16,
        .exact = true,
        .lead_boundary = true,
        .trail_boundary = true,
    },
    TokenRule{
        .name = "github-token",
        .prefixes = {"ghp_", "gho_", "ghu_", "ghs_", "ghr_"},
        .body = kAlnum,
        .min_length = 36,
        .lead_boundary = true,
        .trail_boundary = true,
    },
    TokenRule{
        .name = "github-token",
        .prefixes = {"github_pat_"},
        .body = kWordChars,
        .min_length = 50,
        .lead_boundary = true,
    },
    TokenRule{
        .name = "gitlab-token",
        .prefixes = {"glpat-"},
        .body = kTokenChars,
        .min_length = 20,
        .lead_boundary = true,
    },
    TokenRule{
        .name = "slack-token",
        .prefixes = {"xoxa-", "xoxb-", "xoxp-", "xoxr-", "xoxs-"},
        .body = kSlackChars,
        .min_length = 10,
        .lead_boundary = true,
    },
    TokenRule{
        .name = "google-api-key",
        .prefixes = {"AIza"},
        .body = kTokenChars,
        .min_length = 35,
        .exact = true,
        .lead_boundary = true,
        .trail_boundary = true,
    },
    TokenRule{
        .name = "stripe-key",
        .prefixes = {"sk_live_", "rk_live_"},
        .body = kAlnum,
        .min_length = 20,
        .lead_boundary = true,
    },
    TokenRule{
        .name = "huggingface-token",
        .prefixes = {"hf_"},
        .body = kAlpha,
        .min_length = 30,
        .lead_boundary = true,
        .trail_boundary = true,
    },
};

constexpr std::array<std::string_view, 8> kCredentialKeywords{
    "password", "passwd",       "secret",     "api_key",
    "apikey",   "access_token", "auth_token", "private_key",
};
constexpr std::array<std::string_view, 5> kPlaceholderMarkers{
    "example", "placeholder", "changeme", "xxxx", "your_",
};
constexpr std::string_view kPlaceholderStarts = "$<{";

constexpr std::array<std::string_view, 3> kEnvTemplates{
    ".env.example",
    ".env.sample",
    ".env.template",
};
constexpr std::array<std::string_view, 6> kCredentialNames{
    ".netrc", ".npmrc", ".pypirc", ".git-credentials", "credentials", "credentials.json",
};
constexpr std::array<std::string_view, 4> kSshKeyPrefixes{
    "id_rsa",
    "id_dsa",
    "id_ecdsa",
    "id_ed25519",
};
constexpr std::array<std::string_view, 6> kKeyFileSuffixes{
    ".pem", ".key", ".p12", ".pfx", ".jks", ".keystore",
};
constexpr std::array<std::string_view, 4> kCredentialDirs{".ssh", ".gnupg", ".aws", ".azure"};

struct CredentialLocation {
    std::array<std::string_view, 2> components;
    std::string_view description;
};

// Matched as component sequences anywhere in the path rather than under $HOME, so other users'
// homes and a $HOME that differs from the resolved path are covered too.
constexpr std::array kCredentialLocations{
    CredentialLocation{
        .components = {".config", "gcloud"},
        .description = "Google Cloud credential directory",
    },
    CredentialLocation{
        .components = {".config", "openrouter"},
        .description = "OpenRouter credential directory",
    },
    CredentialLocation{
        .components = {".docker", "config.json"},
        .description = "Docker credential file",
    },
    CredentialLocation{
        .components = {".kube", "config"},
        .description = "Kubernetes credential file",
    },
};

bool is_in(std::string_view set, char symbol) {
    return set.find(symbol) != kNotFound;
}

char to_lower_char(char symbol) {
    const std::size_t index = kUpper.find(symbol);
    return index == kNotFound ? symbol : kLower.at(index);
}

std::string to_lower(std::string_view text) {
    return text | std::views::transform(to_lower_char) | std::ranges::to<std::string>();
}

// Length of the run of characters from `set` starting at pos (pos <= text.size()).
std::size_t run_length(std::string_view text, std::size_t pos, std::string_view set) {
    const std::size_t stop = text.find_first_not_of(set, pos);
    return (stop == kNotFound ? text.size() : stop) - pos;
}

std::size_t skip_blanks(std::string_view text, std::size_t pos) {
    return pos + run_length(text, pos, kBlanks);
}

bool is_word_at(std::string_view text, std::size_t pos) {
    return pos < text.size() && is_in(kWordChars, text.at(pos));
}

// ECMAScript \b: a word character on exactly one side of pos.
bool word_boundary(std::string_view text, std::size_t pos) {
    return (pos > 0 && is_word_at(text, pos - 1)) != is_word_at(text, pos);
}

std::optional<Span> token_at(std::string_view text, Span prefix, const TokenRule& rule) {
    if (rule.lead_boundary && !word_boundary(text, prefix.begin)) {
        return std::nullopt;
    }
    const std::size_t run = run_length(text, prefix.end, rule.body);
    if (run < rule.min_length) {
        return std::nullopt;
    }
    const std::size_t end = prefix.end + (rule.exact ? rule.min_length : run);
    if (rule.trail_boundary && !word_boundary(text, end)) {
        return std::nullopt;
    }
    return Span{.begin = prefix.begin, .end = end};
}

std::optional<Span> find_with_prefix(std::string_view text, std::size_t from,
                                     std::string_view prefix, const TokenRule& rule) {
    for (std::size_t pos = text.find(prefix, from); pos != kNotFound;
         pos = text.find(prefix, pos + 1)) {
        if (const auto span =
                token_at(text, Span{.begin = pos, .end = pos + prefix.size()}, rule)) {
            return span;
        }
    }
    return std::nullopt;
}

// Earliest match over all prefixes, so iterating from the end of each match skips none.
std::optional<Span> find_token(const TokenRule& rule, std::string_view text, std::size_t from) {
    std::optional<Span> earliest;
    for (const std::string_view prefix : rule.prefixes) {
        if (prefix.empty()) {
            continue;
        }
        const auto span = find_with_prefix(text, from, prefix, rule);
        if (span.has_value() && (!earliest.has_value() || span->begin < earliest->begin)) {
            earliest = span;
        }
    }
    return earliest;
}

// Redaction covers the whole block because the key material sits on the lines after the
// BEGIN marker; without an END marker everything after it is treated as key material.
std::size_t key_block_end(std::string_view text, std::size_t body) {
    constexpr std::string_view end_marker = "-----END ";
    constexpr std::string_view end_tail = "PRIVATE KEY-----";
    const std::size_t marker = text.find(end_marker, body);
    if (marker == kNotFound) {
        return text.size();
    }
    const std::size_t tail = text.find(end_tail, marker);
    return tail == kNotFound ? text.size() : tail + end_tail.size();
}

std::optional<Span> find_private_key(std::string_view text, std::size_t from) {
    constexpr std::string_view begin_marker = "-----BEGIN ";
    constexpr std::string_view label_tail = "PRIVATE KEY";
    constexpr std::string_view dashes = "-----";
    for (std::size_t pos = text.find(begin_marker, from); pos != kNotFound;
         pos = text.find(begin_marker, pos + 1)) {
        const std::size_t label_begin = pos + begin_marker.size();
        const std::size_t label_size = run_length(text, label_begin, kKeyLabelChars);
        const std::size_t label_end = label_begin + label_size;
        if (text.substr(label_begin, label_size).ends_with(label_tail) &&
            text.substr(label_end).starts_with(dashes)) {
            return Span{.begin = pos, .end = key_block_end(text, label_end + dashes.size())};
        }
    }
    return std::nullopt;
}

// Header and payload are base64url JSON objects, so both start with "eyJ" ('{"').
std::optional<std::size_t> jwt_end(std::string_view text, std::size_t start) {
    constexpr std::array<std::string_view, 3> segment_prefixes{"eyJ", ".eyJ", "."};
    if (!word_boundary(text, start)) {
        return std::nullopt;
    }
    std::size_t cursor = start;
    for (const std::string_view prefix : segment_prefixes) {
        if (!text.substr(cursor).starts_with(prefix)) {
            return std::nullopt;
        }
        cursor += prefix.size();
        const std::size_t run = run_length(text, cursor, kTokenChars);
        if (run < kMinJwtSegment) {
            return std::nullopt;
        }
        cursor += run;
    }
    return cursor;
}

std::optional<Span> find_jwt(std::string_view text, std::size_t from) {
    constexpr std::string_view head = "eyJ";
    for (std::size_t pos = text.find(head, from); pos != kNotFound;
         pos = text.find(head, pos + 1)) {
        if (const auto end = jwt_end(text, pos)) {
            return Span{.begin = pos, .end = *end};
        }
    }
    return std::nullopt;
}

bool is_placeholder(std::string_view value) {
    if (value.empty() || value.find_first_not_of(value.front()) == kNotFound ||
        is_in(kPlaceholderStarts, value.front())) {
        return true;
    }
    const std::string lowered = to_lower(value);
    return lowered == "password" ||
           std::ranges::any_of(kPlaceholderMarkers, [&lowered](std::string_view marker) {
               return lowered.contains(marker);
           });
}

// The scheme is the run of scheme characters ending at "://"; it needs a lowercase letter.
bool has_scheme_before(std::string_view text, std::size_t separator) {
    const std::string_view before = text.substr(0, separator);
    const std::size_t last_other = before.find_last_not_of(kSchemeChars);
    const std::string_view scheme =
        last_other == kNotFound ? before : before.substr(last_other + 1);
    return scheme.find_first_of(kLower) != kNotFound;
}

std::optional<Span> url_password(std::string_view text, std::size_t authority) {
    const std::size_t colon = text.find_first_of(kUserStop, authority);
    if (colon == kNotFound || text.at(colon) != ':') {
        return std::nullopt;
    }
    const std::size_t password_begin = colon + 1;
    const std::size_t at_sign = text.find_first_of(kPasswordStop, password_begin);
    if (at_sign == kNotFound || text.at(at_sign) != '@') {
        return std::nullopt;
    }
    const std::string_view password = text.substr(password_begin, at_sign - password_begin);
    if (password.size() < kMinUrlPassword || is_placeholder(password)) {
        return std::nullopt;
    }
    return Span{.begin = password_begin, .end = at_sign};
}

std::optional<Span> find_url_credentials(std::string_view text, std::size_t from) {
    constexpr std::string_view separator = "://";
    for (std::size_t pos = text.find(separator, from); pos != kNotFound;
         pos = text.find(separator, pos + 1)) {
        if (!has_scheme_before(text, pos)) {
            continue;
        }
        if (const auto span = url_password(text, pos + separator.size())) {
            return span;
        }
    }
    return std::nullopt;
}

// Where each credential keyword next occurs at or after a moving offset. A keyword's search
// only ever moves forward, and an absent keyword is searched once, so walking every candidate
// in a text costs one pass per keyword however many candidates are rejected.
class KeywordCursor {
public:
    explicit KeywordCursor(std::string_view lowered) : lowered_(lowered) {
        for (auto&& [next, keyword] : std::views::zip(next_, kCredentialKeywords)) {
            next = lowered_.find(keyword);
        }
    }

    // Start of the earliest keyword at or after `from`, or kNotFound.
    std::size_t at_or_after(std::size_t from) {
        std::size_t earliest = kNotFound;
        for (auto&& [next, keyword] : std::views::zip(next_, kCredentialKeywords)) {
            if (next < from) {
                next = lowered_.find(keyword, from);
            }
            earliest = std::min(earliest, next);
        }
        return earliest;
    }

private:
    std::string_view lowered_;
    std::array<std::size_t, kCredentialKeywords.size()> next_{};
};

// ":=" and "=>" assign; "==" and "===" compare, so they are not assignments.
std::size_t assignment_operator_size(std::string_view rest) {
    if (rest.starts_with(":=") || rest.starts_with("=>")) {
        return 2;
    }
    if (rest.starts_with("==")) {
        return 0;
    }
    return rest.starts_with(':') || rest.starts_with('=') ? 1 : 0;
}

// Values with whitespace are prose (prompts, messages), not credentials.
std::optional<Span> quoted_value(std::string_view text, std::size_t open) {
    if (open >= text.size() || !is_in(kQuotes, text.at(open))) {
        return std::nullopt;
    }
    // One search stops at the closing quote or at the end of the line, whichever comes first.
    const std::array<char, 2> stops{text.at(open), '\n'};
    const std::size_t close =
        text.find_first_of(std::string_view(stops.data(), stops.size()), open + 1);
    if (close == kNotFound || text.at(close) == '\n') {
        return std::nullopt;
    }
    const std::string_view value = text.substr(open + 1, close - open - 1);
    if (value.size() < kMinQuotedSecret || value.find_first_of(kSpace) != kNotFound ||
        is_placeholder(value)) {
        return std::nullopt;
    }
    return Span{.begin = open + 1, .end = close};
}

std::optional<Span> quoted_assignment_at(std::string_view text, std::size_t keyword) {
    std::size_t cursor = keyword + run_length(text, keyword, kNameChars);
    if (cursor < text.size() && is_in(kQuotes, text.at(cursor))) {
        ++cursor;
    }
    cursor = skip_blanks(text, cursor);
    const std::size_t operator_size = assignment_operator_size(text.substr(cursor));
    if (operator_size == 0) {
        return std::nullopt;
    }
    return quoted_value(text, skip_blanks(text, cursor + operator_size));
}

std::string_view without_trailing_blanks(std::string_view text) {
    const std::size_t last = text.find_last_not_of(kBlanks);
    return text.substr(0, last == kNotFound ? 0 : last + 1);
}

// True when only blanks, optionally around "export", precede name_begin on its line. Read
// backwards from the name, so the cost does not grow with the length of the line.
bool starts_line(std::string_view text, std::size_t name_begin) {
    const std::string_view before = text.substr(0, name_begin);
    std::string_view lead = without_trailing_blanks(before);
    if (lead.size() < before.size() && lead.ends_with(kExport)) {
        lead = without_trailing_blanks(lead.substr(0, lead.size() - kExport.size()));
    }
    return lead.empty() || lead.ends_with('\n');
}

std::optional<Span> unquoted_value(std::string_view text, std::size_t value_begin) {
    const std::size_t stop = text.find_first_of(kEnvValueStop, value_begin);
    const std::size_t value_end = stop == kNotFound ? text.size() : stop;
    if (value_end < text.size() && !is_in(kSpace, text.at(value_end))) {
        return std::nullopt;
    }
    const std::string_view value = text.substr(value_begin, value_end - value_begin);
    if (value.size() < kMinUnquotedSecret || is_placeholder(value)) {
        return std::nullopt;
    }
    return Span{.begin = value_begin, .end = value_end};
}

std::optional<Span> env_assignment_at(std::string_view text, std::size_t keyword) {
    const std::size_t before_name = text.substr(0, keyword).find_last_not_of(kWordChars);
    const std::size_t name_begin = before_name == kNotFound ? 0 : before_name + 1;
    const std::size_t name_end = keyword + run_length(text, keyword, kWordChars);
    const std::string_view rest = text.substr(name_end);
    if (!rest.starts_with('=') || rest.starts_with("==") || !starts_line(text, name_begin)) {
        return std::nullopt;
    }
    return unquoted_value(text, name_end + 1);
}

using AssignmentParser = std::optional<Span> (*)(std::string_view text, std::size_t keyword);

struct AssignmentRule {
    AssignmentParser parse = nullptr;
    // The characters of the name a keyword sits in. The parser's verdict is the same for every
    // keyword inside one run of them, so a rejected run is skipped whole.
    std::string_view name_chars;
};

constexpr std::array kAssignmentRules{
    AssignmentRule{.parse = quoted_assignment_at, .name_chars = kNameChars},
    AssignmentRule{.parse = env_assignment_at, .name_chars = kWordChars},
};

constexpr std::string_view kAssignmentPattern = "credential-assignment";

// Every assignment a rule finds, each search resuming after the previous value; only the first
// when first_only is set.
void find_assignments(const AssignmentRule& rule, std::string_view text, KeywordCursor cursor,
                      bool first_only, std::vector<Match>& found) {
    std::size_t keyword = cursor.at_or_after(0);
    while (keyword != kNotFound) {
        const std::optional<Span> span = rule.parse(text, keyword);
        if (span.has_value()) {
            found.push_back(Match{.pattern = kAssignmentPattern, .span = *span});
            if (first_only) {
                return;
            }
        }
        keyword = cursor.at_or_after(
            span.has_value() ? span->end : keyword + run_length(text, keyword, rule.name_chars));
    }
}

using Finder = std::optional<Span> (*)(std::string_view text, std::size_t from);

struct FinderRule {
    std::string_view name;
    Finder find = nullptr;
};

constexpr std::array kFinderRules{
    FinderRule{.name = "private-key-block", .find = find_private_key},
    FinderRule{.name = "jwt", .find = find_jwt},
    FinderRule{.name = "url-credentials", .find = find_url_credentials},
};

std::vector<Match> find_all(std::string_view text, bool first_only = false) {
    std::vector<Match> found;
    for (const TokenRule& rule : kTokenRules) {
        if (first_only && std::ranges::any_of(found, [&rule](const Match& match) {
                return match.pattern == rule.name;
            })) {
            continue;
        }
        std::size_t from = 0;
        while (const auto span = find_token(rule, text, from)) {
            found.push_back(Match{.pattern = rule.name, .span = *span});
            if (first_only) {
                break;
            }
            from = span->end;
        }
    }
    for (const FinderRule& rule : kFinderRules) {
        std::size_t from = 0;
        while (const auto span = rule.find(text, from)) {
            found.push_back(Match{.pattern = rule.name, .span = *span});
            if (first_only) {
                break;
            }
            from = span->end;
        }
    }
    const std::string lowered = to_lower(text);
    const KeywordCursor keywords(lowered);
    for (const AssignmentRule& rule : kAssignmentRules) {
        if (first_only && std::ranges::any_of(found, [](const Match& match) {
                return match.pattern == kAssignmentPattern;
            })) {
            break;
        }
        find_assignments(rule, text, keywords, first_only, found);
    }
    return found;
}

std::string redact(std::string_view text) {
    std::vector<Match> matches = find_all(text);
    std::ranges::sort(matches, {}, [](const Match& match) { return match.span.begin; });
    std::string out;
    std::size_t copied = 0;
    for (const Match& match : matches) {
        if (match.span.begin >= copied) {
            out.append(text.substr(copied, match.span.begin - copied));
            out.append(kRedacted);
        }
        copied = std::max(copied, match.span.end);
    }
    out.append(text.substr(copied));
    return out;
}

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

std::optional<std::string> credential_name_reason(std::string_view name) {
    if ((name == ".env" || name.starts_with(".env.")) &&
        !std::ranges::contains(kEnvTemplates, name)) {
        return "environment file";
    }
    if (std::ranges::contains(kCredentialNames, name)) {
        return "credential file";
    }
    if (!name.ends_with(".pub") &&
        std::ranges::any_of(kSshKeyPrefixes,
                            [name](std::string_view prefix) { return name.starts_with(prefix); })) {
        return "SSH private key";
    }
    if (std::ranges::any_of(kKeyFileSuffixes,
                            [name](std::string_view suffix) { return name.ends_with(suffix); })) {
        return "private key or keystore file";
    }
    if (name.starts_with("service-account") && name.ends_with(".json")) {
        return "cloud service account key";
    }
    return std::nullopt;
}

std::optional<std::string> credential_location_reason(const std::filesystem::path& resolved) {
    std::vector<std::string> parts;
    for (const std::filesystem::path& part : resolved) {
        parts.push_back(part.string());
    }
    for (const std::string_view directory : kCredentialDirs) {
        if (std::ranges::contains(parts, directory)) {
            return std::format("inside a {} directory", directory);
        }
    }
    for (const CredentialLocation& location : kCredentialLocations) {
        if (!std::ranges::search(parts, location.components).empty()) {
            return std::string(location.description);
        }
    }
    return std::nullopt;
}

} // namespace

std::vector<SecretFinding> scan_text(const SecretScan& input) {
    std::vector<SecretFinding> findings;
    std::size_t line_number = 0;
    for (const auto part : std::views::split(input.text, '\n')) {
        ++line_number;
        std::vector<std::string_view> reported;
        for (const Match& match : find_all(std::string_view(part.begin(), part.end()), true)) {
            if (std::ranges::contains(reported, match.pattern)) {
                continue;
            }
            reported.push_back(match.pattern);
            findings.push_back(SecretFinding{
                .source = std::string(input.source),
                .line = line_number,
                .pattern = std::string(match.pattern),
            });
        }
    }
    return findings;
}

std::optional<std::string> credential_path_reason(const std::filesystem::path& resolved) {
    return credential_name_reason(to_lower(resolved.filename().string())).or_else([&resolved] {
        return credential_location_reason(resolved);
    });
}

std::string sanitize_for_display(std::string_view text, std::size_t max_chars) {
    std::string clean = redact(text);
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
