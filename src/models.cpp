#include "models.hpp"

#include "common.hpp"
#include "fsutil.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace orcli {
namespace {

using json = nlohmann::json;

constexpr std::string_view catalog_file_name = "catalog.json";
constexpr std::string_view aliases_file_name = "models.json";
constexpr std::string_view locks_dir_name = "locks";
constexpr std::string_view aliases_lock_name = "models.lock";
constexpr std::string_view batch_suffix = ":batch";
constexpr char latest_prefix = '~';
constexpr int json_indent = 2;

constexpr std::string_view not_available = "n/a";
// OpenRouter publishes "-1" for routers, whose charge is that of the model they pick.
constexpr std::string_view variable_price = "variable";
constexpr std::size_t per_million_shift = 6;
constexpr std::size_t per_request_shift = 0;
constexpr std::size_t max_price_decimals = 4;
constexpr std::size_t min_price_decimals = 2;
constexpr std::size_t digits_per_group = 3;
constexpr std::size_t column_gap = 2;
constexpr std::size_t label_width = 20;

// ---------------------------------------------------------------------------------------------
// JSON access that tolerates missing and wrong-typed fields.
// ---------------------------------------------------------------------------------------------

const json* member(const json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

std::optional<std::string> string_member(const json& object, std::string_view key) {
    const json* value = member(object, key);
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    return value->get<std::string>();
}

std::optional<long long> integer_member(const json& object, std::string_view key) {
    const json* value = member(object, key);
    if (value == nullptr || !value->is_number_integer()) {
        return std::nullopt;
    }
    return value->get<long long>();
}

std::vector<std::string> string_array(const json* value) {
    std::vector<std::string> items;
    if (value == nullptr || !value->is_array()) {
        return items;
    }
    for (const json& item : *value) {
        if (item.is_string()) {
            items.push_back(item.get<std::string>());
        }
    }
    return items;
}

// ---------------------------------------------------------------------------------------------
// Catalog parsing.
// ---------------------------------------------------------------------------------------------

Pricing parse_pricing(const json& object) {
    Pricing pricing;
    pricing.prompt = string_member(object, "prompt");
    pricing.completion = string_member(object, "completion");
    pricing.input_cache_read = string_member(object, "input_cache_read");
    pricing.input_cache_write = string_member(object, "input_cache_write");
    pricing.internal_reasoning = string_member(object, "internal_reasoning");
    pricing.request = string_member(object, "request");
    const json* overrides = member(object, "overrides");
    pricing.has_overrides = overrides != nullptr && overrides->is_array() && !overrides->empty();
    return pricing;
}

void apply_reasoning(const json& reasoning, ModelInfo& model) {
    model.has_reasoning = true;
    const json* mandatory = member(reasoning, "mandatory");
    model.reasoning_mandatory =
        mandatory != nullptr && mandatory->is_boolean() && mandatory->get<bool>();
    const json* efforts = member(reasoning, "supported_efforts");
    if (efforts != nullptr && efforts->is_null()) {
        model.effort_support = EffortSupport::any;
    } else if (efforts != nullptr && efforts->is_array()) {
        model.effort_support = EffortSupport::listed;
        model.supported_efforts = string_array(efforts);
    }
    model.default_effort = string_member(reasoning, "default_effort");
}

std::optional<ModelInfo> parse_model(const json& entry) {
    auto model_id = string_member(entry, "id");
    if (!model_id.has_value()) {
        return std::nullopt;
    }
    ModelInfo model;
    model.id = std::move(*model_id);
    model.name = string_member(entry, "name").value_or(std::string{});
    model.canonical_slug = string_member(entry, "canonical_slug").value_or(std::string{});
    model.created = integer_member(entry, "created").value_or(0);
    model.context_length = integer_member(entry, "context_length").value_or(0);
    if (const json* top = member(entry, "top_provider"); top != nullptr) {
        model.max_completion_tokens = integer_member(*top, "max_completion_tokens");
    }
    if (const json* target = member(entry, "alias_target"); target != nullptr) {
        model.alias_target = string_member(*target, "slug");
    }
    model.supported_parameters = string_array(member(entry, "supported_parameters"));
    model.expiration_date = string_member(entry, "expiration_date");
    if (const json* pricing = member(entry, "pricing"); pricing != nullptr) {
        model.pricing = parse_pricing(*pricing);
    }
    if (const json* reasoning = member(entry, "reasoning");
        reasoning != nullptr && reasoning->is_object()) {
        apply_reasoning(*reasoning, model);
    }
    return model;
}

Result<std::vector<ModelInfo>> parse_catalog_document(const json& document) {
    const json* data = member(document, "data");
    if (data == nullptr || !data->is_array()) {
        return fail(Exit::api, "the model catalog response has no data array");
    }
    std::vector<ModelInfo> models;
    models.reserve(data->size());
    for (const json& entry : *data) {
        auto model = parse_model(entry);
        // ":batch" rows duplicate a model for the Batch API, which this tool never calls.
        if (model.has_value() && !model->id.ends_with(batch_suffix)) {
            models.push_back(std::move(*model));
        }
    }
    return models;
}

// OpenRouter's status codes are small integers; a value outside int is malformed and read as
// normal (0).
int status_member(const json& object) {
    const std::optional<long long> status = integer_member(object, "status");
    if (!status.has_value() || !std::in_range<int>(*status)) {
        return 0;
    }
    return static_cast<int>(*status);
}

std::optional<Endpoint> parse_endpoint(const json& entry) {
    auto tag = string_member(entry, "tag");
    if (!tag.has_value()) {
        return std::nullopt;
    }
    Endpoint endpoint;
    endpoint.tag = std::move(*tag);
    endpoint.provider_name = string_member(entry, "provider_name").value_or(std::string{});
    if (const json* pricing = member(entry, "pricing"); pricing != nullptr) {
        endpoint.prompt_price = string_member(*pricing, "prompt");
        endpoint.completion_price = string_member(*pricing, "completion");
    }
    endpoint.context_length = integer_member(entry, "context_length").value_or(0);
    endpoint.max_completion_tokens = integer_member(entry, "max_completion_tokens");
    endpoint.service_tier = string_member(entry, "service_tier");
    endpoint.status = status_member(entry);
    return endpoint;
}

// ---------------------------------------------------------------------------------------------
// Search and resolution.
// ---------------------------------------------------------------------------------------------

enum class CharClass : std::uint8_t { letter, digit, other };

constexpr char to_lower_ascii(char raw) {
    if (raw >= 'A' && raw <= 'Z') {
        return static_cast<char>(raw - 'A' + 'a');
    }
    return raw;
}

constexpr bool is_digit(char raw) {
    return raw >= '0' && raw <= '9';
}

constexpr bool is_lower_letter(char raw) {
    return raw >= 'a' && raw <= 'z';
}

CharClass class_at(std::string_view text, std::size_t index, CharClass previous) {
    const char lower = to_lower_ascii(text.at(index));
    if (is_lower_letter(lower)) {
        return CharClass::letter;
    }
    if (is_digit(lower)) {
        return CharClass::digit;
    }
    // A dot between digits stays inside the number, so "5.6" is one version token and a
    // query for "6" cannot match it.
    const std::string_view rest = text.substr(index + 1);
    if (lower == '.' && previous == CharClass::digit && !rest.empty() && is_digit(rest.front())) {
        return CharClass::digit;
    }
    return CharClass::other;
}

// Lowercase runs of letters and of numbers; a letter/digit boundary starts a new token, so
// "gpt6" gives "gpt" and "6".
std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    CharClass current_class = CharClass::other;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const CharClass kind = class_at(text, index, current_class);
        if (kind != current_class && !current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
        if (kind != CharClass::other) {
            current.push_back(to_lower_ascii(text.at(index)));
        }
        current_class = kind;
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

// A number in the query also matches a longer version it begins: "6" matches "6.1".
bool token_matches(const std::string& wanted, const std::string& candidate) {
    if (candidate == wanted) {
        return true;
    }
    return is_digit(wanted.front()) && candidate.starts_with(wanted + '.');
}

bool matches_all(const std::vector<std::string>& wanted, const ModelInfo& model) {
    std::vector<std::string> tokens = tokenize(model.id);
    tokens.append_range(tokenize(model.name));
    return std::ranges::all_of(wanted, [&tokens](const std::string& token) {
        return std::ranges::any_of(tokens, [&token](const std::string& candidate) {
            return token_matches(token, candidate);
        });
    });
}

const ModelInfo* find_by_id(const std::vector<ModelInfo>& catalog, std::string_view model_id) {
    const auto found = std::ranges::find(catalog, model_id, &ModelInfo::id);
    return found == catalog.end() ? nullptr : &*found;
}

// A "~...-latest" row whose target is in the catalog would only duplicate that target.
bool shadowed_latest(const ModelInfo& model, const std::vector<ModelInfo>& catalog) {
    return model.id.starts_with(latest_prefix) && model.alias_target.has_value() &&
           find_by_id(catalog, *model.alias_target) != nullptr;
}

Resolution concrete_resolution(const ModelInfo& model, const std::vector<ModelInfo>& catalog) {
    Resolution resolution;
    const ModelInfo* target = &model;
    if (model.alias_target.has_value()) {
        target = find_by_id(catalog, *model.alias_target);
    }
    if (target != nullptr) {
        resolution.kind = Resolution::Kind::found;
        resolution.model = *target;
    }
    return resolution;
}

// ---------------------------------------------------------------------------------------------
// Display.
// ---------------------------------------------------------------------------------------------

std::string with_commas(long long value) {
    const std::string digits = std::to_string(value);
    const std::size_t sign = digits.starts_with('-') ? std::size_t{1} : std::size_t{0};
    std::string grouped;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index > sign && (digits.size() - index) % digits_per_group == 0) {
            grouped.push_back(',');
        }
        grouped.push_back(digits.at(index));
    }
    return grouped;
}

std::string optional_count(const std::optional<long long>& count) {
    return count.has_value() ? with_commas(*count) : std::string(not_available);
}

bool all_digits(std::string_view text) {
    return std::ranges::all_of(text, is_digit);
}

void increment_digits(std::string& digits) {
    for (char& digit : digits | std::views::reverse) {
        if (digit != '9') {
            ++digit;
            return;
        }
        digit = '0';
    }
    digits.insert(0, 1, '1');
}

// Multiplies a published decimal string by 10^shift with string arithmetic, so a price such
// as "0.0000001875" prints exactly; rounds half up to max_price_decimals.
std::optional<std::string> scale_decimal(std::string_view text, std::size_t shift) {
    const std::size_t point = text.find('.');
    const std::string_view whole = text.substr(0, point);
    const std::string_view fraction =
        point == std::string_view::npos ? std::string_view{} : text.substr(point + 1);
    if ((whole.empty() && fraction.empty()) || !all_digits(whole) || !all_digits(fraction)) {
        return std::nullopt;
    }
    std::string digits = std::format("{}{}", whole, fraction);
    std::size_t decimals = fraction.size();
    if (decimals >= shift) {
        decimals -= shift;
    } else {
        digits.append(shift - decimals, '0');
        decimals = 0;
    }
    if (decimals > max_price_decimals) {
        const std::size_t keep = digits.size() - (decimals - max_price_decimals);
        const bool round_up = digits.at(keep) >= '5';
        digits.resize(keep);
        decimals = max_price_decimals;
        if (round_up) {
            increment_digits(digits);
        }
    }
    if (digits.size() <= decimals) {
        digits.insert(0, decimals + 1 - digits.size(), '0');
    }
    std::string integer = digits.substr(0, digits.size() - decimals);
    std::string fractional = digits.substr(digits.size() - decimals);
    const std::size_t significant = integer.find_first_not_of('0');
    integer = significant == std::string::npos ? std::string("0") : integer.substr(significant);
    while (fractional.size() > min_price_decimals && fractional.ends_with('0')) {
        fractional.pop_back();
    }
    if (fractional.size() < min_price_decimals) {
        fractional.append(min_price_decimals - fractional.size(), '0');
    }
    return std::format("{}.{}", integer, fractional);
}

std::string price_text(const std::optional<std::string>& price, std::size_t shift,
                       std::string_view unit) {
    if (!price.has_value()) {
        return std::string(not_available);
    }
    if (price->starts_with('-')) {
        return std::string(variable_price);
    }
    const std::optional<std::string> amount = scale_decimal(*price, shift);
    if (!amount.has_value()) {
        return std::string(not_available);
    }
    return std::format("${}{}", *amount, unit);
}

std::string per_million(const std::optional<std::string>& price) {
    return price_text(price, per_million_shift, "/M");
}

std::string per_request(const std::optional<std::string>& price) {
    return price_text(price, per_request_shift, " per request");
}

template <std::size_t Columns>
std::string render_row(const std::array<std::string, Columns>& row,
                       const std::array<std::size_t, Columns>& widths) {
    std::string line;
    for (const auto& [cell, width] : std::views::zip(row, widths)) {
        line += std::format("{:<{}}", cell, width + column_gap);
    }
    while (line.ends_with(' ')) {
        line.pop_back();
    }
    line.push_back('\n');
    return line;
}

// Left-aligned columns sized to their widest cell; the first row is the header.
template <std::size_t Columns>
std::string render_table(const std::vector<std::array<std::string, Columns>>& rows) {
    std::array<std::size_t, Columns> widths{};
    for (const auto& row : rows) {
        for (auto&& [cell, width] : std::views::zip(row, widths)) {
            width = std::max(width, cell.size());
        }
    }
    std::string table;
    for (const auto& row : rows) {
        table += render_row(row, widths);
    }
    return table;
}

constexpr std::size_t candidate_columns = 6;
using CandidateRow = std::array<std::string, candidate_columns>;

CandidateRow candidate_row(const ModelInfo& model) {
    return CandidateRow{model.id,
                        model.name,
                        with_commas(model.context_length),
                        optional_count(model.max_completion_tokens),
                        per_million(model.pricing.prompt),
                        per_million(model.pricing.completion)};
}

constexpr std::size_t endpoint_columns = 7;
using EndpointRow = std::array<std::string, endpoint_columns>;

std::string endpoint_notes(const Endpoint& endpoint) {
    std::string notes = endpoint.service_tier.value_or(std::string{});
    if (endpoint.status != 0) {
        notes += std::format("{}status {}", notes.empty() ? "" : "; ", endpoint.status);
    }
    return notes;
}

EndpointRow endpoint_row(const Endpoint& endpoint) {
    return EndpointRow{endpoint.tag,
                       endpoint.provider_name,
                       per_million(endpoint.prompt_price),
                       per_million(endpoint.completion_price),
                       with_commas(endpoint.context_length),
                       optional_count(endpoint.max_completion_tokens),
                       endpoint_notes(endpoint)};
}

std::string join(const std::vector<std::string>& items, std::string_view separator) {
    std::string joined;
    for (const std::string& item : items) {
        if (!joined.empty()) {
            joined += separator;
        }
        joined += item;
    }
    return joined;
}

std::string describe_reasoning(const ModelInfo& model) {
    if (!model.has_reasoning) {
        return "not supported";
    }
    std::string text = model.reasoning_mandatory ? "mandatory" : "optional";
    switch (model.effort_support) {
    case EffortSupport::none:
        text += "; effort not selectable";
        break;
    case EffortSupport::any:
        text += "; any effort";
        break;
    case EffortSupport::listed:
        text += std::format("; efforts {}", join(model.supported_efforts, ", "));
        break;
    }
    if (model.default_effort.has_value()) {
        text += std::format("; default {}", *model.default_effort);
    }
    return text;
}

struct Field {
    std::string_view label;
    std::string value;
};

std::string render_fields(const std::vector<Field>& fields) {
    std::string text;
    for (const Field& field : fields) {
        text +=
            std::format("{:<{}}{}\n", std::format("{}:", field.label), label_width, field.value);
    }
    return text;
}

// ---------------------------------------------------------------------------------------------
// State files.
// ---------------------------------------------------------------------------------------------

std::string utc_now() {
    return std::format("{:%FT%TZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::optional<std::chrono::sys_seconds> parse_utc(std::string_view text) {
    std::istringstream input{std::string(text)};
    std::chrono::sys_seconds when{};
    input >> std::chrono::parse("%FT%TZ", when);
    if (input.fail()) {
        return std::nullopt;
    }
    return when;
}

Result<bool> file_present(const std::filesystem::path& file) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(file, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return false;
    }
    if (error) {
        return fail(Exit::config,
                    std::format("cannot inspect {}: {}", file.string(), error.message()));
    }
    return true;
}

Result<std::string> read_private_file(const std::filesystem::path& file) {
    if (const auto checked = check_private_file(file); !checked) {
        return std::unexpected(checked.error());
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        return fail(Exit::config, std::format("cannot open {}", file.string()));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad() || buffer.bad()) {
        return fail(Exit::config, std::format("cannot read {}", file.string()));
    }
    return std::move(buffer).str();
}

struct CachedCatalog {
    Catalog catalog;
    std::chrono::sys_seconds fetched;
};

std::optional<CachedCatalog> decode_catalog_cache(std::string_view text) {
    const json document = json::parse(text, nullptr, false);
    const std::optional<std::string> fetched_at = string_member(document, "fetched_at");
    if (!fetched_at.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::chrono::sys_seconds> fetched = parse_utc(*fetched_at);
    const json* body = member(document, "body");
    if (!fetched.has_value() || body == nullptr) {
        return std::nullopt;
    }
    auto models = parse_catalog_document(*body);
    if (!models) {
        return std::nullopt;
    }
    return CachedCatalog{.catalog =
                             Catalog{.models = std::move(*models), .fetched_at = *fetched_at},
                         .fetched = *fetched};
}

// An unreadable or corrupt cache is not an error: the catalog is fetched again.
Result<std::optional<CachedCatalog>> read_catalog_cache(const std::filesystem::path& file) {
    const auto present = file_present(file);
    if (!present) {
        return std::unexpected(present.error());
    }
    if (!*present) {
        return std::optional<CachedCatalog>{};
    }
    const auto text = read_private_file(file);
    if (!text) {
        return std::unexpected(text.error());
    }
    return decode_catalog_cache(*text);
}

bool is_fresh(std::chrono::sys_seconds fetched, std::chrono::hours ttl) {
    const auto age = std::chrono::system_clock::now() - fetched;
    return age >= std::chrono::system_clock::duration::zero() && age < ttl;
}

Result<Catalog> stale_or(const std::optional<CachedCatalog>& cached, const Error& error) {
    if (cached.has_value()) {
        return cached->catalog;
    }
    return std::unexpected(error);
}

Result<Catalog> store_catalog(const std::filesystem::path& state_dir, std::string_view body,
                              std::vector<ModelInfo> models) {
    if (const auto made = ensure_private_dir(state_dir); !made) {
        return std::unexpected(made.error());
    }
    std::string fetched_at = utc_now();
    // The body was validated by parse_catalog, so it is embedded verbatim rather than
    // re-serialized.
    const std::string document =
        std::format("{{\"fetched_at\": \"{}\", \"body\": {}}}\n", fetched_at, body);
    if (const auto written = atomic_write(state_dir / catalog_file_name, document); !written) {
        return std::unexpected(written.error());
    }
    return Catalog{.models = std::move(models), .fetched_at = std::move(fetched_at)};
}

std::optional<std::map<std::string, AliasEntry>> decode_aliases(std::string_view text) {
    const json document = json::parse(text, nullptr, false);
    const json* entries = member(document, "aliases");
    if (entries == nullptr || !entries->is_object()) {
        return std::nullopt;
    }
    std::map<std::string, AliasEntry> aliases;
    for (const auto& item : entries->items()) {
        auto model_id = string_member(item.value(), "model_id");
        if (!model_id.has_value()) {
            return std::nullopt;
        }
        aliases.insert_or_assign(
            item.key(),
            AliasEntry{.model_id = std::move(*model_id),
                       .saved = string_member(item.value(), "saved").value_or(std::string{})});
    }
    return aliases;
}

std::string encode_aliases(const std::map<std::string, AliasEntry>& aliases) {
    json entries = json::object();
    for (const auto& [alias, entry] : aliases) {
        entries.emplace(alias, json{{"model_id", entry.model_id}, {"saved", entry.saved}});
    }
    const json document{{"aliases", std::move(entries)}};
    // Replacing invalid UTF-8 keeps dump() from throwing on a malformed command-line alias.
    return document.dump(json_indent, ' ', false, json::error_handler_t::replace) + "\n";
}

} // namespace

Result<std::vector<ModelInfo>> parse_catalog(std::string_view json_text) {
    const json document = json::parse(json_text, nullptr, false);
    if (document.is_discarded()) {
        return fail(Exit::api, "the model catalog response is not valid JSON");
    }
    return parse_catalog_document(document);
}

Result<std::vector<Endpoint>> parse_endpoints(std::string_view json_text) {
    const json document = json::parse(json_text, nullptr, false);
    const json* data = member(document, "data");
    const json* entries = data == nullptr ? nullptr : member(*data, "endpoints");
    if (entries == nullptr || !entries->is_array()) {
        return fail(Exit::api, "the model endpoints response has no data.endpoints array");
    }
    std::vector<Endpoint> endpoints;
    endpoints.reserve(entries->size());
    for (const json& entry : *entries) {
        if (auto endpoint = parse_endpoint(entry); endpoint.has_value()) {
            endpoints.push_back(std::move(*endpoint));
        }
    }
    return endpoints;
}

std::string normalize_alias(std::string_view name) {
    std::string alias;
    bool separator_pending = false;
    for (const char raw : name) {
        const char lower = to_lower_ascii(raw);
        if (!is_lower_letter(lower) && !is_digit(lower) && lower != '.') {
            separator_pending = true;
            continue;
        }
        if (separator_pending && !alias.empty()) {
            alias.push_back('-');
        }
        separator_pending = false;
        alias.push_back(lower);
    }
    return alias;
}

std::vector<ModelInfo> search(std::string_view query, const std::vector<ModelInfo>& catalog) {
    const std::vector<std::string> wanted = tokenize(query);
    if (wanted.empty()) {
        return {};
    }
    auto results = catalog | std::views::filter([&](const ModelInfo& model) {
                       return !shadowed_latest(model, catalog) && matches_all(wanted, model);
                   }) |
                   std::ranges::to<std::vector>();
    std::ranges::stable_sort(results, std::ranges::greater{}, &ModelInfo::created);
    return results;
}

Resolution resolve(std::string_view name, const std::vector<ModelInfo>& catalog,
                   const std::map<std::string, AliasEntry>& aliases) {
    const std::string alias = normalize_alias(name);
    for (const std::string& key : {alias, std::string(name)}) {
        const auto entry = aliases.find(key);
        if (entry == aliases.end()) {
            continue;
        }
        // An alias whose model left the catalog falls through to the other lookups.
        if (const ModelInfo* saved = find_by_id(catalog, entry->second.model_id);
            saved != nullptr) {
            return concrete_resolution(*saved, catalog);
        }
    }
    if (const ModelInfo* exact = find_by_id(catalog, name); exact != nullptr) {
        return concrete_resolution(*exact, catalog);
    }
    std::vector<ModelInfo> matches = search(name, catalog);
    if (matches.size() == 1) {
        Resolution resolution = concrete_resolution(matches.front(), catalog);
        if (resolution.kind == Resolution::Kind::found) {
            resolution.new_alias = alias;
        }
        return resolution;
    }
    Resolution resolution;
    if (matches.size() > 1) {
        resolution.kind = Resolution::Kind::ambiguous;
        resolution.candidates = std::move(matches);
    }
    return resolution;
}

std::string format_candidates(const std::vector<ModelInfo>& models) {
    std::vector<CandidateRow> rows{
        CandidateRow{"ID", "NAME", "CONTEXT", "MAX OUTPUT", "INPUT", "OUTPUT"}};
    rows.append_range(models | std::views::transform(candidate_row));
    return render_table(rows);
}

std::string format_endpoints(const std::vector<Endpoint>& endpoints) {
    std::vector<EndpointRow> rows{
        EndpointRow{"TAG", "PROVIDER", "INPUT", "OUTPUT", "CONTEXT", "MAX OUTPUT", "NOTES"}};
    rows.append_range(endpoints | std::views::transform(endpoint_row));
    return render_table(rows) + "Pass a tag (or its provider prefix, e.g. google-vertex) to "
                                "--provider to use only those endpoints.\n";
}

std::string format_model(const ModelInfo& model, std::string_view catalog_fetched_at) {
    std::vector<Field> fields;
    fields.push_back({.label = "ID", .value = model.id});
    fields.push_back({.label = "Name", .value = model.name});
    fields.push_back({.label = "Canonical slug",
                      .value = model.canonical_slug.empty() ? std::string(not_available)
                                                            : model.canonical_slug});
    if (model.alias_target.has_value()) {
        fields.push_back({.label = "Alias for", .value = *model.alias_target});
    }
    fields.push_back(
        {.label = "Context", .value = std::format("{} tokens", with_commas(model.context_length))});
    fields.push_back({.label = "Max output", .value = optional_count(model.max_completion_tokens)});
    fields.push_back({.label = "Reasoning", .value = describe_reasoning(model)});
    fields.push_back({.label = "Input", .value = per_million(model.pricing.prompt)});
    fields.push_back(
        {.label = "Cached input", .value = per_million(model.pricing.input_cache_read)});
    fields.push_back(
        {.label = "Cache write", .value = per_million(model.pricing.input_cache_write)});
    fields.push_back({.label = "Output", .value = per_million(model.pricing.completion)});
    fields.push_back(
        {.label = "Internal reasoning", .value = per_million(model.pricing.internal_reasoning)});
    fields.push_back({.label = "Request", .value = per_request(model.pricing.request)});
    if (model.expiration_date.has_value()) {
        fields.push_back({.label = "Expires", .value = *model.expiration_date});
    }
    std::string text = render_fields(fields);
    if (model.pricing.has_overrides) {
        text += "Some requests, such as very long prompts, are billed at different catalog "
                "rates than these.\n";
    }
    text += std::format("Prices are OpenRouter catalog rates fetched {}; the serving provider's "
                        "rate can differ.\n",
                        catalog_fetched_at);
    return text;
}

Result<Catalog> load_catalog(const std::filesystem::path& state_dir, std::chrono::hours ttl,
                             const FetchText& fetch, bool force_refresh) {
    const auto previous = read_catalog_cache(state_dir / catalog_file_name);
    if (!previous) {
        return std::unexpected(previous.error());
    }
    const std::optional<CachedCatalog>& cached = *previous;
    if (cached.has_value() && !force_refresh && is_fresh(cached->fetched, ttl)) {
        return cached->catalog;
    }
    const auto text = fetch();
    if (!text) {
        return stale_or(cached, text.error());
    }
    auto models = parse_catalog(*text);
    if (!models) {
        return stale_or(cached, models.error());
    }
    return store_catalog(state_dir, *text, std::move(*models));
}

Result<std::map<std::string, AliasEntry>> load_aliases(const std::filesystem::path& state_dir) {
    const std::filesystem::path file = state_dir / aliases_file_name;
    const auto present = file_present(file);
    if (!present) {
        return std::unexpected(present.error());
    }
    if (!*present) {
        return std::map<std::string, AliasEntry>{};
    }
    const auto text = read_private_file(file);
    if (!text) {
        return std::unexpected(text.error());
    }
    auto aliases = decode_aliases(*text);
    if (!aliases.has_value()) {
        return fail(Exit::config,
                    std::format("model alias registry {} is corrupt; fix or remove the bad entry",
                                file.string()));
    }
    return std::move(*aliases);
}

Result<void> save_alias(const std::filesystem::path& state_dir, const std::string& alias,
                        const std::string& model_id) {
    if (const auto made = ensure_private_dir(state_dir); !made) {
        return made;
    }
    const std::filesystem::path locks = state_dir / locks_dir_name;
    if (const auto made = ensure_private_dir(locks); !made) {
        return made;
    }
    const auto lock = FileLock::acquire(locks / aliases_lock_name, true);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto aliases = load_aliases(state_dir);
    if (!aliases) {
        return std::unexpected(aliases.error());
    }
    aliases->insert_or_assign(alias, AliasEntry{.model_id = model_id, .saved = utc_now()});
    return atomic_write(state_dir / aliases_file_name, encode_aliases(*aliases));
}

} // namespace orcli
