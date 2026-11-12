#include "group.hpp"

#include "common.hpp"
#include "session.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

using orcli::ChildEvents;
using orcli::ChildResult;
using orcli::ChildSpec;
using orcli::Exit;
using orcli::MemberOutcome;
using orcli::MemberState;
using orcli::SuperviseOptions;

namespace {

constexpr std::size_t session_limit = 64;
constexpr std::size_t pipe_overflow = 300000; // well past the 64 KiB pipe capacity

ChildSpec shell(std::string script) {
    return ChildSpec{.argv = {"/bin/sh", "-c", std::move(script)}};
}

} // namespace

TEST_CASE("a model list splits on commas and trims spaces") {
    CHECK(orcli::parse_model_list("a/b").value() == std::vector<std::string>{"a/b"});
    CHECK(orcli::parse_model_list(" a/b , c/d:tag,\te/f ").value() ==
          std::vector<std::string>{"a/b", "c/d:tag", "e/f"});
    for (const std::string_view bad : {"", "a/b,", ",a/b", "a/b,,c/d", " , "}) {
        CAPTURE(bad);
        const auto parsed = orcli::parse_model_list(bad);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == Exit::usage);
    }
}

TEST_CASE("member session names are readable, bounded and tell normalized ids apart") {
    CHECK(orcli::fnv1a_hex("") == "811c9dc5");
    CHECK(orcli::fnv1a_hex("a") == "e40c292c");
    const std::string carbon = orcli::member_session_name("plan", "google/gemini-4-carbon").value();
    CHECK(carbon.starts_with("plan.google-gemini-4-carbon."));
    CHECK(orcli::valid_session_name(carbon));
    // Both normalize to a-b-c, so only the digest keeps them apart.
    CHECK(orcli::member_session_name("s", "a/b-c").value() !=
          orcli::member_session_name("s", "a-b/c").value());
    const std::string longest = std::string(53, 'n');
    const std::string fitted =
        orcli::member_session_name(longest, "google/gemini-4-carbon").value();
    CHECK(fitted.size() <= session_limit);
    CHECK(orcli::valid_session_name(fitted));
    CHECK(orcli::member_session_name(longest + "n", "a/b").error().code == Exit::usage);
    CHECK(orcli::member_session_name("a/b", "a/b").error().code == Exit::usage);
}

TEST_CASE("an fnv collision with a truncated prefix gives equal names, which preflight refuses") {
    // A known collision: the digests match, and a long name truncates both prefixes to test-model.
    CHECK(orcli::fnv1a_hex("test/model-8f5dc93b") == orcli::fnv1a_hex("test/model-2e6cef45"));
    const std::string name(43, 'n');
    CHECK(orcli::member_session_name(name, "test/model-8f5dc93b").value() ==
          orcli::member_session_name(name, "test/model-2e6cef45").value());
}

TEST_CASE("the group exit code follows cancellation, crashes, then group order") {
    const MemberOutcome done{.state = MemberState::complete, .code = 0};
    const MemberOutcome partial{.state = MemberState::incomplete, .code = 6};
    const MemberOutcome api{.state = MemberState::failed, .code = 5};
    const MemberOutcome stopped{.state = MemberState::interrupted, .code = 8};
    const MemberOutcome crash{.state = MemberState::crashed, .code = 9};
    CHECK(orcli::group_exit_code({done, done}, false) == 0);
    CHECK(orcli::group_exit_code({done, partial, api}, false) == 6);
    CHECK(orcli::group_exit_code({api, partial}, false) == 5);
    CHECK(orcli::group_exit_code({done, crash, api}, false) == 1);
    CHECK(orcli::group_exit_code({api, stopped}, false) == 8);
    CHECK(orcli::group_exit_code({done}, true) == 8);
    CHECK(orcli::outcome_of_exit(0).state == MemberState::complete);
    CHECK(orcli::outcome_of_exit(6).state == MemberState::incomplete);
    CHECK(orcli::outcome_of_exit(8).state == MemberState::interrupted);
    CHECK(orcli::outcome_of_exit(127).state == MemberState::crashed);
    CHECK(orcli::outcome_of_exit(4).state == MemberState::failed);
}

TEST_CASE("children run at once, read the shared input and keep their own output") {
    // Each child waits for the other's marker file, so the run finishes only if both overlap.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / std::format("orcli-group-{}", ::getpid());
    std::filesystem::create_directories(dir);
    const std::vector<ChildSpec> children{
        shell(std::format("touch {0}/one; while [ ! -e {0}/two ]; do sleep 0.01; done; cat; "
                          "printf 'err-one\\npartial' >&2",
                          dir.string())),
        shell(std::format("touch {0}/two; while [ ! -e {0}/one ]; do sleep 0.01; done; wc -c; "
                          "exit 5",
                          dir.string())),
    };
    const std::string input(pipe_overflow, 'q');
    std::vector<std::string> lines;
    std::vector<std::size_t> finished;
    const ChildEvents events{
        .stderr_line =
            [&](std::size_t index, std::string_view line) {
                lines.push_back(std::format("{}:{}", index, line));
            },
        .finished = [&](std::size_t index, const ChildResult&) { finished.push_back(index); },
    };
    bool cancelled = true;
    const auto results =
        orcli::run_children(children, SuperviseOptions{.input = input}, events, cancelled);
    std::filesystem::remove_all(dir);
    CHECK_FALSE(cancelled);
    REQUIRE(results.size() == 2);
    CHECK(results.at(0).started);
    CHECK(results.at(0).exit_code == 0);
    CHECK(results.at(0).out == input);
    CHECK(results.at(1).exit_code == 5);
    CHECK(results.at(1).out.contains(std::to_string(pipe_overflow)));
    CHECK(lines == std::vector<std::string>{"0:err-one", "0:partial"});
    CHECK(finished.size() == 2);
}

TEST_CASE("large output from many children is drained without deadlock") {
    std::vector<ChildSpec> children;
    for (int index = 0; index < 4; ++index) {
        children.push_back(shell(std::format(
            "head -c {0} /dev/zero | tr '\\0' o; head -c {0} /dev/zero | tr '\\0' e >&2",
            pipe_overflow)));
    }
    std::size_t stderr_bytes = 0;
    const ChildEvents events{
        .stderr_line = [&](std::size_t, std::string_view line) { stderr_bytes += line.size(); },
        .finished = {},
    };
    bool cancelled = false;
    const auto results =
        orcli::run_children(children, SuperviseOptions{.input = {}}, events, cancelled);
    for (const ChildResult& result : results) {
        CHECK(result.exit_code == 0);
        CHECK(result.out.size() == pipe_overflow);
    }
    CHECK(stderr_bytes == 4 * pipe_overflow);
}

TEST_CASE("a child that cannot start stops later ones but lets earlier ones finish") {
    const std::vector<ChildSpec> children{
        shell("sleep 0.2; echo first"),
        ChildSpec{.argv = {"/nonexistent/openrouter-cli"}},
        shell("echo never"),
    };
    bool cancelled = false;
    const auto results =
        orcli::run_children(children, SuperviseOptions{.input = {}}, ChildEvents{}, cancelled);
    REQUIRE(results.size() == 3);
    CHECK(results.at(0).started);
    CHECK(results.at(0).out == "first\n");
    CHECK_FALSE(results.at(1).started);
    CHECK(results.at(1).spawn_error.code == Exit::config);
    CHECK(results.at(1).spawn_error.message.contains("/nonexistent/openrouter-cli"));
    CHECK_FALSE(results.at(2).started);
}

TEST_CASE("a child killed by a signal is reported as such") {
    bool cancelled = false;
    const auto results = orcli::run_children({shell("kill -9 $$")}, SuperviseOptions{.input = {}},
                                             ChildEvents{}, cancelled);
    REQUIRE(results.size() == 1);
    CHECK(results.at(0).term_signal == 9);
}
