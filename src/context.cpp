#include "context.hpp"

#include "common.hpp"
#include "fsutil.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace orcli {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBinaryProbeBytes = 8192;
constexpr std::size_t kMinFenceLength = 3;

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
        // Right-aligned by hand: a nested width ("{:>{}}") is rejected by Clang with
        // libstdc++ 15 (llvm/llvm-project#151371).
        const std::string digits = std::to_string(number);
        out.append(width > digits.size() ? width - digits.size() : 0, ' ');
        out += digits;
        out += " | ";
        out += content.substr(start, stop - start);
        out += '\n';
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
    const auto size = fs::file_size(resolved, error);
    if (error) {
        return fail(Exit::usage, std::format("cannot read {}: {}", shown, error.message()));
    }
    if (size > max_bytes) {
        return fail(Exit::refused,
                    std::format("{} is {} bytes, over the {}-byte limit", shown, size, max_bytes));
    }
    auto content = read_regular_file(resolved, max_bytes);
    if (!content.has_value()) {
        return fail(content.error().code, std::format("{} {}", shown, content.error().message));
    }
    if (std::string_view(*content).substr(0, kBinaryProbeBytes).contains('\0')) {
        return fail(Exit::refused, std::format("{} looks binary (NUL byte in its first {} bytes)",
                                               shown, kBinaryProbeBytes));
    }
    return FileSnapshot{
        .display_path = shown,
        .resolved = resolved,
        .content = std::move(*content),
    };
}

std::string fence_for(std::string_view content) {
    std::size_t longest = 0;
    std::size_t run = 0;
    for (const char symbol : content) {
        run = symbol == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    std::string fence;
    fence.assign(std::max(kMinFenceLength, longest + 1), '`');
    return fence;
}

Result<std::string> pack_files(const std::vector<FileSnapshot>& files, std::size_t max_bytes) {
    std::size_t content_bytes = 0;
    for (const FileSnapshot& file : files) {
        content_bytes = orcli::saturating_add(content_bytes, file.content.size());
    }
    std::string out;
    // A lower bound of the packed size (headings and line numbers come on top), never above the
    // limit that the loop enforces.
    out.reserve(std::min(content_bytes, max_bytes));
    for (const FileSnapshot& file : files) {
        const std::string fence = fence_for(file.content);
        const std::size_t lines = count_lines(file.content);
        const std::size_t width = std::formatted_size("{}", lines);
        std::size_t remaining = max_bytes - out.size();
        const auto consume = [&remaining](std::size_t bytes) {
            if (bytes > remaining) {
                return false;
            }
            remaining -= bytes;
            return true;
        };
        constexpr std::size_t heading_overhead = 9;
        constexpr std::size_t line_prefix_overhead = 3;
        const bool has_final_newline = file.content.ends_with('\n');
        if (!consume(out.empty() ? 0 : 1) || !consume(heading_overhead) ||
            !consume(file.display_path.size()) || !consume(fence.size()) ||
            !consume(fence.size()) || !consume(file.content.size()) ||
            (!file.content.empty() && !has_final_newline && !consume(1)) ||
            (lines > 0 && (width + line_prefix_overhead > remaining / lines ||
                           !consume(lines * (width + line_prefix_overhead))))) {
            return fail(Exit::refused, "packed file context exceeds the aggregate input limit");
        }
        if (!out.empty()) {
            out += '\n';
        }
        out += std::format("File: {}\n{}\n", file.display_path, fence);
        append_numbered_lines(out, file.content);
        out += fence;
        out += '\n';
    }
    return out;
}

std::string pack_files(const std::vector<FileSnapshot>& files) {
    return pack_files(files, std::numeric_limits<std::size_t>::max()).value();
}

} // namespace orcli
