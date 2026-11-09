#include "costlog.hpp"

#include "common.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using orcli::Exit;

namespace {

constexpr std::size_t call_id_length = 32;
constexpr std::size_t iso_length = 20;
constexpr std::size_t iso_date_separator = 10;
constexpr double quarter = 0.25;
constexpr double half = 0.5;
constexpr double eighth = 0.125;
constexpr double sample_cost = 0.0412;
constexpr double tiny_cost = 0.0000123;
constexpr long long sample_prompt = 18204;
constexpr long long sample_completion = 3911;
constexpr long long sample_reasoning = 2870;
constexpr long long million_and_more = 1234567;
constexpr long long generation_prompt = 1200;
constexpr long long generation_completion = 300;
constexpr long long generation_cached = 64;

class TempDir {
public:
    TempDir() {
        std::random_device device;
        std::uniform_int_distribution<std::uint32_t> distribution;
        path_ = fs::temp_directory_path() / std::format("orcli-costlog-{:08x}{:08x}",
                                                        distribution(device), distribution(device));
        fs::create_directory(path_);
        fs::permissions(path_, fs::perms::owner_all);
    }
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

bool lowercase_hex(char letter) {
    return (letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'f');
}

constexpr int compact_dump = -1;

std::string record_line(const json& record) {
    return record.dump(compact_dump, ' ', false, json::error_handler_t::replace) + "\n";
}

orcli::PendingInfo pending_info(const std::string& call_id, const std::string& time,
                                const std::string& model_id) {
    orcli::PendingInfo info;
    info.call_id = call_id;
    info.time = time;
    info.cwd = "/work";
    info.requested_model = "alias";
    info.model_id = model_id;
    info.privacy = "zdr";
    info.answer_path = std::format("/state/answers/{}.md", call_id);
    return info;
}

std::string pending_line(const std::string& call_id, const std::string& time,
                         const std::string& model_id) {
    return record_line(orcli::pending_record(pending_info(call_id, time, model_id)));
}

std::string result_line(const std::string& call_id, std::optional<double> cost) {
    orcli::ResultInfo info;
    info.call_id = call_id;
    info.outcome = "complete";
    info.provider = "Provider A";
    info.usage.cost = cost;
    return record_line(orcli::result_record(info));
}

std::string generation_line(const std::string& call_id, const std::string& generation_id) {
    return record_line(orcli::generation_record(call_id, generation_id));
}

std::string resolved_line(const std::string& call_id, std::optional<double> cost) {
    orcli::GenerationStats stats;
    stats.total_cost = cost;
    stats.provider = "Provider B";
    return record_line(orcli::resolved_record(call_id, stats));
}

std::string read_file(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("new_call_id is 32 lowercase hex characters and unique") {
    const std::string first = orcli::new_call_id();
    CHECK(first.size() == call_id_length);
    CHECK(std::ranges::all_of(first, lowercase_hex));
    CHECK(first != orcli::new_call_id());
}

TEST_CASE("utc_now_iso has second precision and a Z suffix") {
    const std::string now = orcli::utc_now_iso();
    REQUIRE(now.size() == iso_length);
    CHECK(now.at(iso_date_separator) == 'T');
    CHECK(now.back() == 'Z');
}

TEST_CASE("record builders write unknown values as null and zero as zero") {
    orcli::PendingInfo info = pending_info("abc", "2026-11-09T10:00:00Z", "vendor/model");
    const json pending = orcli::pending_record(info);
    CHECK(pending.at("v") == 1);
    CHECK(pending.at("type") == "pending");
    CHECK(pending.at("call_id") == "abc");
    CHECK(pending.at("time") == "2026-11-09T10:00:00Z");
    CHECK(pending.at("model_id") == "vendor/model");
    CHECK(pending.at("effort").is_null());
    CHECK(pending.at("session").is_null());
    info.effort = "high";
    info.session = "review";
    const json with_options = orcli::pending_record(info);
    CHECK(with_options.at("effort") == "high");
    CHECK(with_options.at("session") == "review");

    orcli::ResultInfo result;
    result.call_id = "abc";
    result.outcome = "complete";
    const json unknown = orcli::result_record(result);
    CHECK(unknown.at("type") == "result");
    CHECK(unknown.at("cost").is_null());
    CHECK(unknown.at("prompt_tokens").is_null());
    CHECK(unknown.at("cache_write_tokens").is_null());
    CHECK(unknown.at("provider").is_null());
    CHECK(unknown.at("time").is_string());
    result.usage.cost = 0.0;
    result.usage.prompt_tokens = 0;
    const json zero = orcli::result_record(result);
    CHECK(zero.at("cost").is_number());
    CHECK(zero.at("cost") == 0.0);
    CHECK(zero.at("prompt_tokens") == 0);

    const json generation = orcli::generation_record("abc", "gen-1");
    CHECK(generation.at("type") == "generation");
    CHECK(generation.at("generation_id") == "gen-1");

    orcli::GenerationStats stats;
    stats.total_cost = quarter;
    const json resolved = orcli::resolved_record("abc", stats);
    CHECK(resolved.at("type") == "resolved");
    CHECK(resolved.at("total_cost") == quarter);
    CHECK(resolved.at("provider").is_null());
}

TEST_CASE("parse_generation maps the data fields") {
    const auto stats = orcli::parse_generation(
        R"({"data":{"total_cost":0.25,"provider_name":"Google Vertex","model":"vendor/model",)"
        R"("finish_reason":"stop","native_tokens_prompt":1200,"native_tokens_completion":300,)"
        R"("native_tokens_cached":64,"native_tokens_reasoning":null}})");
    REQUIRE(stats.has_value());
    CHECK(stats->total_cost == quarter);
    CHECK(stats->provider == "Google Vertex");
    CHECK(stats->model == "vendor/model");
    CHECK(stats->finish_reason == "stop");
    CHECK(stats->usage.prompt_tokens == generation_prompt);
    CHECK(stats->usage.completion_tokens == generation_completion);
    CHECK(stats->usage.cached_tokens == generation_cached);
    CHECK_FALSE(stats->usage.reasoning_tokens.has_value());
    CHECK_FALSE(stats->usage.cost.has_value());
}

TEST_CASE("parse_generation keeps a null cost unknown and rejects bodies without data") {
    const auto null_cost = orcli::parse_generation(R"({"data":{"total_cost":null}})");
    REQUIRE(null_cost.has_value());
    CHECK_FALSE(null_cost->total_cost.has_value());

    const auto no_data = orcli::parse_generation(R"({"error":{"code":404}})");
    REQUIRE_FALSE(no_data.has_value());
    CHECK(no_data.error().code == Exit::api);

    const auto not_json = orcli::parse_generation("<html>");
    REQUIRE_FALSE(not_json.has_value());
    CHECK(not_json.error().code == Exit::api);
}

TEST_CASE("fold_log leaves a call with only a pending record unresolved") {
    const auto report =
        orcli::fold_log(pending_line("c1", "2026-11-09T10:00:00Z", "vendor/model"), "");
    REQUIRE(report.calls.size() == 1);
    CHECK(report.calls.at(0).state == "pending");
    CHECK_FALSE(report.calls.at(0).cost.has_value());
    REQUIRE(report.unresolved.size() == 1);
    CHECK(report.unresolved.at(0).call_id == "c1");
    CHECK(report.known_total == 0.0);
}

TEST_CASE("fold_log uses the stream cost when the result has one") {
    const std::string log =
        pending_line("c1", "2026-11-09T10:00:00Z", "vendor/model") + result_line("c1", quarter);
    const auto report = orcli::fold_log(log, "");
    REQUIRE(report.calls.size() == 1);
    CHECK(report.calls.at(0).state == "complete");
    CHECK(report.calls.at(0).cost == quarter);
    CHECK(report.calls.at(0).provider == "Provider A");
    CHECK(report.calls.at(0).model == "vendor/model");
    CHECK(report.known_total == quarter);
    CHECK(report.unresolved.empty());
}

TEST_CASE("fold_log counts a resolved charge once and never adds it to a stream cost") {
    const std::string resolved_twice = pending_line("c1", "2026-11-09T10:00:00Z", "vendor/model") +
                                       result_line("c1", std::nullopt) + resolved_line("c1", half) +
                                       resolved_line("c1", half);
    const auto resolved = orcli::fold_log(resolved_twice, "");
    REQUIRE(resolved.calls.size() == 1);
    CHECK(resolved.calls.at(0).cost == half);
    CHECK(resolved.calls.at(0).provider == "Provider A");
    CHECK(resolved.known_total == half);
    CHECK(resolved.unresolved.empty());

    const std::string both = pending_line("c2", "2026-11-09T10:00:00Z", "vendor/model") +
                             result_line("c2", quarter) + resolved_line("c2", half);
    const auto stream_wins = orcli::fold_log(both, "");
    CHECK(stream_wins.known_total == quarter);

    const std::string still_unknown = pending_line("c3", "2026-11-09T10:00:00Z", "vendor/model") +
                                      result_line("c3", std::nullopt);
    const auto unknown = orcli::fold_log(still_unknown, "");
    REQUIRE(unknown.unresolved.size() == 1);
    CHECK(unknown.unresolved.at(0).state == "complete");
}

TEST_CASE("fold_log reports corrupt lines by number and keeps the valid records") {
    const std::string log = pending_line("c1", "2026-11-09T10:00:00Z", "vendor/model") +
                            "not json\n" + "\n" + R"({"v":1,"type":"result"})" + "\n" +
                            result_line("c1", quarter) + R"({"v":1,"type":"pend)";
    const std::vector<std::size_t> expected_corrupt{2, 4, 6};
    const auto report = orcli::fold_log(log, "");
    CHECK(report.corrupt_lines == expected_corrupt);
    REQUIRE(report.calls.size() == 1);
    CHECK(report.known_total == quarter);
}

TEST_CASE("fold_log uses the earliest record time when the pending record is missing") {
    const std::string log =
        R"({"v":1,"type":"result","call_id":"c9","time":"2026-11-08T09:00:00Z",)"
        R"("outcome":"error","cost":null})"
        "\n"
        R"({"v":1,"type":"generation","call_id":"c9","time":"2026-11-08T08:00:00Z",)"
        R"("generation_id":"g"})"
        "\n";
    const auto report = orcli::fold_log(log, "");
    REQUIRE(report.calls.size() == 1);
    CHECK(report.calls.at(0).time == "2026-11-08T08:00:00Z");
    CHECK(report.calls.at(0).state == "error");
}

TEST_CASE("fold_log filters by since and totals by day and by model") {
    const std::string log =
        pending_line("old", "2026-11-01T10:00:00Z", "vendor/a") + result_line("old", half) +
        pending_line("day1", "2026-11-08T10:00:00Z", "vendor/a") + result_line("day1", quarter) +
        pending_line("day2a", "2026-11-09T10:00:00Z", "vendor/a") + result_line("day2a", eighth) +
        pending_line("day2b", "2026-11-09T11:00:00Z", "vendor/b") + result_line("day2b", quarter);
    const auto report = orcli::fold_log(log, "2026-11-08T10:00:00Z");
    REQUIRE(report.calls.size() == 3);
    CHECK(report.calls.at(0).call_id == "day1");
    CHECK(report.calls.at(1).call_id == "day2a");
    CHECK(report.calls.at(2).call_id == "day2b");
    CHECK(report.known_total == quarter + eighth + quarter);
    REQUIRE(report.cost_by_day.size() == 2);
    CHECK(report.cost_by_day.at("2026-11-08") == quarter);
    CHECK(report.cost_by_day.at("2026-11-09") == eighth + quarter);
    REQUIRE(report.cost_by_model.size() == 2);
    CHECK(report.cost_by_model.at("vendor/a") == quarter + eighth);
    CHECK(report.cost_by_model.at("vendor/b") == quarter);

    CHECK(orcli::fold_log(log, "").calls.size() == 4);
}

TEST_CASE("format_spend lists totals, unresolved calls and corrupt lines") {
    const std::string log = pending_line("c1", "2026-11-09T10:00:00Z", "vendor/a") +
                            result_line("c1", quarter) +
                            pending_line("c2", "2026-11-09T11:00:00Z", "vendor/b") + "junk\n";
    const std::string text = orcli::format_spend(orcli::fold_log(log, ""));
    CHECK(text.find("$0.2500") != std::string::npos);
    CHECK(text.find("2026-11-09") != std::string::npos);
    CHECK(text.find("vendor/a") != std::string::npos);
    CHECK(text.find("Unresolved") != std::string::npos);
    CHECK(text.find("c2") != std::string::npos);
    CHECK(text.find("not counted): 4\n") != std::string::npos);
}

TEST_CASE("format_cost_line prints known values with thousands separators") {
    orcli::ResultInfo result;
    result.served_model = "google/gemini-4-argon";
    result.provider = "Google Vertex";
    result.usage.cost = sample_cost;
    result.usage.prompt_tokens = sample_prompt;
    result.usage.cached_tokens = 0;
    result.usage.completion_tokens = sample_completion;
    result.usage.reasoning_tokens = sample_reasoning;
    CHECK(orcli::format_cost_line(result, "requested/model", "/state/answers/a.md") ==
          "cost $0.0412 | google/gemini-4-argon via Google Vertex | in 18,204 (cached 0) | "
          "out 3,911 (reasoning 2,870) | answer /state/answers/a.md");

    result.usage.cost = tiny_cost;
    result.usage.prompt_tokens = million_and_more;
    const std::string tiny = orcli::format_cost_line(result, "requested/model", "a.md");
    CHECK(tiny.starts_with("cost $0.00001 |"));
    CHECK(tiny.find("in 1,234,567 ") != std::string::npos);
}

TEST_CASE("format_cost_line prints unknown values as unknown") {
    const orcli::ResultInfo result{};
    CHECK(orcli::format_cost_line(result, "vendor/model", "a.md") ==
          "cost unknown | vendor/model via unknown provider | in unknown (cached unknown) | "
          "out unknown (reasoning unknown) | answer a.md");
}

TEST_CASE("CostLog::append writes one private JSON line per record") {
    const TempDir temp;
    const fs::path dir = temp.path() / "log";
    const orcli::CostLog log(dir / "costs.jsonl", dir / "costs.lock");
    REQUIRE(log.append(orcli::generation_record("c1", "g1")).has_value());
    REQUIRE(log.append(orcli::generation_record("c1", "g2")).has_value());
    CHECK(fs::symlink_status(dir).permissions() == fs::perms::owner_all);
    CHECK(fs::symlink_status(log.file()).permissions() ==
          (fs::perms::owner_read | fs::perms::owner_write));
    const auto report = orcli::fold_log(read_file(log.file()), "");
    CHECK(report.corrupt_lines.empty());
    REQUIRE(report.calls.size() == 1);
    CHECK(report.calls.at(0).call_id == "c1");
    CHECK(std::ranges::count(read_file(log.file()), '\n') == 2);
}

TEST_CASE("CostLog::append serializes a record holding invalid UTF-8 without throwing") {
    const TempDir temp;
    const fs::path dir = temp.path() / "log";
    const orcli::CostLog log(dir / "costs.jsonl", dir / "costs.lock");
    orcli::PendingInfo info = pending_info("c1", "2026-11-09T10:00:00Z", "vendor/model");
    info.cwd = "/work/bad\xff/dir";
    const json record = orcli::pending_record(info);
    CHECK_NOTHROW(record_line(record));
    REQUIRE(log.append(record).has_value());
    const auto report = orcli::fold_log(read_file(log.file()), "");
    CHECK(report.corrupt_lines.empty());
    REQUIRE(report.calls.size() == 1);
    CHECK(report.calls.at(0).call_id == "c1");
}

TEST_CASE("fold_log carries the generation id onto the call") {
    const std::string log = pending_line("c1", "2026-11-09T10:00:00Z", "vendor/model") +
                            generation_line("c1", "gen-1") + result_line("c1", quarter);
    const auto report = orcli::fold_log(log, "");
    REQUIRE(report.calls.size() == 1);
    CHECK(report.calls.at(0).generation_id == "gen-1");
    CHECK(report.unresolved.empty());
}

TEST_CASE("a call killed before its result is unresolved with its generation id") {
    const std::string log =
        pending_line("c1", "2026-11-09T10:00:00Z", "vendor/model") + generation_line("c1", "gen-1");
    const auto report = orcli::fold_log(log, "");
    REQUIRE(report.unresolved.size() == 1);
    CHECK(report.unresolved.at(0).call_id == "c1");
    CHECK(report.unresolved.at(0).state == "pending");
    CHECK(report.unresolved.at(0).generation_id == "gen-1");
    CHECK_FALSE(report.unresolved.at(0).cost.has_value());
}

TEST_CASE("fold_log keeps the first non-empty generation id") {
    const std::string log = pending_line("c1", "2026-11-09T10:00:00Z", "vendor/model") +
                            generation_line("c1", "") + generation_line("c1", "gen-first") +
                            generation_line("c1", "gen-second");
    const auto report = orcli::fold_log(log, "");
    REQUIRE(report.calls.size() == 1);
    CHECK(report.calls.at(0).generation_id == "gen-first");

    const auto without = orcli::fold_log(pending_line("c2", "2026-11-09T10:00:00Z", "m"), "");
    REQUIRE(without.calls.size() == 1);
    CHECK_FALSE(without.calls.at(0).generation_id.has_value());
}

TEST_CASE("a generation id does not change how a resolved charge is counted") {
    const std::string log = pending_line("c1", "2026-11-09T10:00:00Z", "vendor/model") +
                            generation_line("c1", "gen-1") + result_line("c1", std::nullopt) +
                            resolved_line("c1", half) + resolved_line("c1", half);
    const auto report = orcli::fold_log(log, "");
    REQUIRE(report.calls.size() == 1);
    CHECK(report.calls.at(0).generation_id == "gen-1");
    CHECK(report.calls.at(0).cost == half);
    CHECK(report.known_total == half);
    CHECK(report.unresolved.empty());
}
