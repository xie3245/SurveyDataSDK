#include "survey/exchange.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace survey {
namespace {

using Json = nlohmann::json;

// Upper bound for our v1 schema, nlohmann/json 3.12 and dump(2).
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53 &&
                  std::numeric_limits<double>::max_exponent == 1024,
              "The JSON size budget assumes IEEE-754 binary64 doubles");

constexpr std::size_t escaped_id_bytes = 6U * BatchId::max_bytes;

constexpr std::size_t number_bytes = 32;

constexpr std::size_t observation_json_bytes = escaped_id_bytes + 3U * number_bytes + 128U;

// Four metadata IDs: origin, batch, device and frame.
// The remaining allowance covers field names, revision, state,
// units, punctuation, indentation and the final newline.
constexpr std::size_t envelope_json_bytes = 4U * escaped_id_bytes + 1024U;

constexpr std::size_t maximum_encoded_bytes =
    envelope_json_bytes + MeasurementBatch::max_observations * observation_json_bytes;

static_assert(maximum_encoded_bytes <= max_exchange_bytes,
              "The maximum permitted batch must fit in one exchange document");

void fields(const Json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_object() || value.size() != names.size()) {
        throw ExchangeError("Object has missing or unsupported fields");
    }

    for (const auto name : names) {
        if (!value.contains(std::string{name})) {
            throw ExchangeError("Missing field: " + std::string{name});
        }
    }
}

std::string text(const Json& value, const char* name) {
    const auto& item = value.at(name);

    if (!item.is_string()) {
        throw ExchangeError(std::string{name} + " must be a string");
    }

    return item.get<std::string>();
}

std::int64_t revision(const Json& document) {
    const auto number   = text(document, "base_revision");
    std::int64_t result = 0;

    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), result);

    if (number.empty() || number.front() < '1' || number.front() > '9' || parsed.ec != std::errc{} ||
        parsed.ptr != number.data() + number.size() || result < 1) {
        throw ExchangeError(
            "base_revision must be a positive int64 "
            "written as a decimal string");
    }

    return result;
}

double coordinate(const Json& value, const char* name) {
    const auto& item = value.at(name);

    if (!item.is_number()) {
        throw ExchangeError(std::string{name} + " must be a number");
    }

    return item.get<double>();
}

void valid_revision(const ExchangeDocument& document) {
    if (document.base_revision < 1) {
        throw ExchangeError("base_revision must be positive");
    }
}

}  // namespace

std::string encode_exchange(const ExchangeDocument& document) {
    valid_revision(document);

    try {
        Json observations = Json::array();

        for (const auto& observation : document.batch.observations()) {
            observations.push_back({{"id", observation.id().value()},
                                    {"x", observation.point().x()},
                                    {"y", observation.point().y()},
                                    {"z", observation.point().z()}});
        }

        const Json result{{"exchange_version", 1},
                          {"origin", document.origin.value()},
                          {"base_revision", std::to_string(document.base_revision)},
                          {"batch",
                           {{"id", document.batch.id().value()},
                            {"device_id", document.batch.device().value()},
                            {"frame", document.batch.frame().value()},
                            {"units", "m"},
                            {"state", document.batch.state() == BatchState::Open ? "open" : "sealed"},
                            {"observations", std::move(observations)}}}};

        auto encoded = result.dump(2) + '\n';

        // Retain the runtime check in case the format changes.
        if (encoded.size() > max_exchange_bytes) {
            throw ExchangeError("Exchange document is too large");
        }

        return encoded;
    } catch (const Json::exception& error) {
        throw ExchangeError(std::string{"Cannot encode exchange document: "} + error.what());
    }
}

ExchangeDocument decode_exchange(std::string_view input) {
    if (input.size() > max_exchange_bytes) {
        throw ExchangeError("Exchange document exceeds 8 MiB");
    }

    try {
        std::vector<std::unordered_set<std::string>> object_keys;

        const auto callback = [&object_keys](int depth, Json::parse_event_t event, Json& parsed) {
            if (depth > 16) {
                throw ExchangeError("JSON nesting limit exceeded");
            }

            if (event == Json::parse_event_t::object_start) {
                object_keys.emplace_back();
            }

            if (event == Json::parse_event_t::key && !object_keys.back().insert(parsed.get<std::string>()).second) {
                throw ExchangeError("Duplicate JSON object field");
            }

            if (event == Json::parse_event_t::object_end) {
                object_keys.pop_back();
            }

            return true;
        };

        const auto document = Json::parse(input.begin(), input.end(), callback);

        fields(document, {"exchange_version", "origin", "base_revision", "batch"});

        if (!document.at("exchange_version").is_number_integer() || document.at("exchange_version") != 1) {
            throw ExchangeError(
                "Unsupported exchange_version; "
                "this reader supports version 1");
        }

        const auto base  = revision(document);
        const auto& data = document.at("batch");

        fields(data, {"id", "device_id", "frame", "units", "state", "observations"});

        if (text(data, "units") != "m") {
            throw ExchangeError("Only metre coordinates are supported");
        }

        MeasurementBatch batch{BatchId{text(data, "id")}, DeviceId{text(data, "device_id")},
                               FrameId{text(data, "frame")}};

        const auto state = text(data, "state");

        if (state != "open" && state != "sealed") {
            throw ExchangeError("Unknown batch state");
        }

        const auto& observations = data.at("observations");

        if (!observations.is_array() || observations.size() > MeasurementBatch::max_observations) {
            throw ExchangeError(
                "observations must be an array "
                "within the batch size limit");
        }

        for (const auto& item : observations) {
            fields(item, {"id", "x", "y", "z"});

            batch.add(Observation{ObservationId{text(item, "id")},
                                  Point3D{coordinate(item, "x"), coordinate(item, "y"), coordinate(item, "z")}});
        }

        if (state == "sealed") {
            batch.seal();
        }

        return {OriginId{text(document, "origin")}, base, std::move(batch)};
    } catch (const Json::exception& error) {
        throw ExchangeError(std::string{"Invalid exchange JSON: "} + error.what());
    } catch (const DomainError& error) {
        throw ExchangeError(std::string{"Invalid domain data: "} + error.what());
    }
}

ExchangeDocument read_exchange(const std::filesystem::path& path) {
    std::error_code error;
    const auto length = std::filesystem::file_size(path, error);

    if (error) {
        throw ExchangeError("Cannot determine exchange file size");
    }

    if (length > max_exchange_bytes) {
        throw ExchangeError("Exchange file exceeds 8 MiB");
    }

    std::ifstream input{path, std::ios::binary};
    std::string bytes(static_cast<std::size_t>(length), '\0');

    if (!input.read(bytes.data(), static_cast<std::streamsize>(length)) ||
        input.peek() != std::char_traits<char>::eof()) {
        throw ExchangeError(
            "Cannot read exchange file or its size "
            "changed while reading");
    }

    return decode_exchange(bytes);
}

void write_exchange(const std::filesystem::path& path, const ExchangeDocument& document) {
    const auto encoded = encode_exchange(document);

    std::ofstream output{path, std::ios::binary | std::ios::trunc};

    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));

    output.flush();

    if (!output) {
        throw ExchangeError("Cannot write exchange file");
    }
}

ExchangeDocument export_checkout(BatchRepository& source, const OriginId& origin, const BatchId& id) {
    auto stored = source.load(id);

    if (!stored) {
        throw ExchangeError("Cannot export a batch that does not exist");
    }

    return {origin, stored->revision, std::move(stored->batch)};
}

std::int64_t import_checkout(BatchRepository& receiver, const ExchangeDocument& document) {
    valid_revision(document);
    return receiver.save(document.batch, std::nullopt);
}

ExchangeDocument export_return(BatchRepository& receiver, const ExchangeDocument& original_checkout) {
    valid_revision(original_checkout);

    auto stored = receiver.load(original_checkout.batch.id());

    if (!stored) {
        throw ExchangeError("The checked-out batch does not exist in this app");
    }

    return {original_checkout.origin, original_checkout.base_revision, std::move(stored->batch)};
}

std::int64_t apply_return(BatchRepository& origin, const OriginId& expected_origin, const ExchangeDocument& document) {
    valid_revision(document);

    if (document.origin != expected_origin) {
        throw ExchangeError("Return is addressed to a different origin");
    }

    auto current = origin.load(document.batch.id());

    if (!current || current->revision != document.base_revision) {
        throw ConcurrencyConflict(
            "Origin changed since export; "
            "reload and resolve the differences");
    }

    if (current->batch.state() == BatchState::Sealed) {
        throw DomainError("A stored sealed batch cannot be changed");
    }

    if (current->batch.device() != document.batch.device() || current->batch.frame() != document.batch.frame()) {
        throw ExchangeError(
            "A return cannot change the instrument "
            "or coordinate frame");
    }

    const auto before   = current->batch.observations();
    const auto returned = document.batch.observations();

    if (returned.size() < before.size()) {
        throw ExchangeError("A return cannot remove existing observations");
    }

    for (std::size_t index = 0; index < before.size(); ++index) {
        if (before[index].id() != returned[index].id() || before[index].point() != returned[index].point()) {
            throw ExchangeError(
                "A return cannot change identities, values, "
                "or order of existing observations");
        }
    }

    const auto previous_size = before.size();

    for (const auto& observation : returned.subspan(previous_size)) {
        current->batch.add(observation);
    }

    if (document.batch.state() == BatchState::Sealed) {
        current->batch.seal();
    }

    return origin.save(current->batch, document.base_revision);
}

}  // namespace survey