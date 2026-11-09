#include "context.hpp"

#include "secrets.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace orcli {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBinaryProbeBytes = 8192;
constexpr std::size_t kReadChunkBytes = 65536;
constexpr std::size_t kMinFenceLength = 3;

// Reads in chunks and enforces the limit while reading, so a file that grows after the size
// check still cannot exceed it. Messages omit the path; the caller adds it.
Result<std::string> read_limited(const fs::path& resolved, std::size_t max_bytes) {
    std::ifstream input(resolved, std::ios::binary);
    if (!input.is_open()) {
        return fail(Exit::usage, "cannot be opened");
    }
    std::string content;
    std::array<char, kReadChunkBytes> buffer{};
    while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           input.gcount() > 0) {
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > max_bytes - content.size()) {
            return fail(Exit::refused, std::format("grew past the {}-byte limit", max_bytes));
        }
        content.append(buffer.data(), count);
    }
    if (input.bad()) {
        return fail(Exit::usage, "could not be read");
    }
    return content;
}

std::size_t count_lines(std::string_view content) {
    const auto newlines = static_cast<std::size_t>(std::ranges::count(content, '\n'));
    return (content.empty() || content.ends_with('\n')) ? newlines : newlines + 1;
}

void append_numbered_lines(std::string& out, std::string_view content) {
    const std::size_t width = std::formatted_size("{}", count_lines(content));
    std::size_t number = 0;
    for (std::size_t start = 0; start < content.size();) {
        const std::size_t newline = content.find('\n', start);
        const std::size_t stop = newline == std::string_view::npos ? content.size() : newline;
        ++number;
        out += std::format("{:>{}} | {}\n", number, width, content.substr(start, stop - start));
        start = stop + 1;
    }
}

} // namespace

Result<FileSnapshot> read_snapshot(const std::filesystem::path& path, std::size_t max_bytes) {
    const std::string shown = path.string();
    std::error_code error;
    const fs::path resolved = fs::canonical(path, error);
    if (error) {
        return fail(Exit::usage, std::format("cannot read {}: {}", shown, error.message()));
    }
    if (!fs::is_regular_file(resolved, error)) {
        return fail(Exit::refused, std::format("{} is not a regular file", shown));
    }
    if (const auto reason = credential_path_reason(resolved)) {
        return fail(Exit::refused, std::format("{} is not sent: {} is a credential path ({})",
                                               shown, resolved.string(), *reason));
    }
    const auto size = fs::file_size(resolved, error);
    if (error) {
        return fail(Exit::usage, std::format("cannot read {}: {}", shown, error.message()));
    }
    if (size > max_bytes) {
        return fail(Exit::refused,
                    std::format("{} is {} bytes, over the {}-byte limit", shown, size, max_bytes));
    }
    auto content = read_limited(resolved, max_bytes);
    if (!content.has_value()) {
        return fail(content.error().code, std::format("{} {}", shown, content.error().message));
    }
    if (std::string_view(*content).substr(0, kBinaryProbeBytes).contains('\0')) {
        return fail(Exit::refused, std::format("{} looks binary (NUL byte in its first {} bytes)",
                                               shown, kBinaryProbeBytes));
    }
    return FileSnapshot{
        .display_path = shown, .resolved = resolved, .content = std::move(*content)};
}

std::string fence_for(std::string_view content) {
    std::size_t longest = 0;
    std::size_t run = 0;
    for (const char symbol : content) {
        run = symbol == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return std::string(std::max(kMinFenceLength, longest + 1), '`');
}

std::string pack_files(const std::vector<FileSnapshot>& files) {
    std::string out;
    for (const FileSnapshot& file : files) {
        if (!out.empty()) {
            out += '\n';
        }
        const std::string fence = fence_for(file.content);
        out += std::format("File: {}\n{}\n", file.display_path, fence);
        append_numbered_lines(out, file.content);
        out += fence;
        out += '\n';
    }
    return out;
}

} // namespace orcli
