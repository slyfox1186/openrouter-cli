#pragma once

#include "common.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orcli {

struct FileSnapshot {
    std::string display_path; // as given on the command line
    std::filesystem::path resolved;
    std::string content;
};

// Reads a file once. Exit::refused when the resolved path is not a regular file, is a
// credential file, holds a NUL byte in its first 8 KiB, or exceeds max_bytes.
Result<FileSnapshot> read_snapshot(const std::filesystem::path& path, std::size_t max_bytes);

// Shortest backtick fence (at least three) longer than any backtick run in content.
std::string fence_for(std::string_view content);

// Each file as a heading with its path, then a fenced block whose lines are prefixed with
// right-aligned line numbers and " | ", so reviewers can cite file:line.
std::string pack_files(const std::vector<FileSnapshot>& files);

} // namespace orcli
