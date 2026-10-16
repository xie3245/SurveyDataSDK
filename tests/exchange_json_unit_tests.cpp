#include "survey/exchange.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <limits>
#include <optional>
#include <string>
#include <utility>

using namespace survey;
using Json = nlohmann::json;

namespace {

MeasurementBatch batch() {
    MeasurementBatch result{BatchId{"batch-1"}, DeviceId{"device-1"}, FrameId{"LOCAL_METRES"}};

    result.add(Observation{ObservationId{"original"}, Point3D{1.2345678901234567, -2.5, 3.75}});

    return result;
}

Observation extra(std::string id) { return {ObservationId{std::move(id)}, Point3D{10.0, 20.0, 30.0}}; }

ExchangeDocument document() { return {OriginId{"app-a"}, 1, batch()}; }

// Produce distinct IDs whose every byte needs a six-character
// JSON escape. Both control characters are valid UTF-8.
std::string maximally_escaped_id(std::size_t value) {
    std::string result(BatchId::max_bytes, '\x01');

    for (auto& byte : result) {
        byte = (value % 2U == 0U) ? '\x01' : '\x02';
        value /= 2U;
    }

    return result;
}

}  // namespace

TEST(Exchange, PreservesBatchThroughJson) {
    auto original = document();
    original.batch.add(extra("quoted-\"observation\""));
    original.batch.seal();

    const auto restored = decode_exchange(encode_exchange(original));

    ASSERT_EQ(restored.origin, original.origin);
    ASSERT_EQ(restored.base_revision, original.base_revision);
    ASSERT_EQ(restored.batch.id(), original.batch.id());
    ASSERT_EQ(restored.batch.device(), original.batch.device());
    ASSERT_EQ(restored.batch.frame(), original.batch.frame());
    ASSERT_EQ(restored.batch.state(), BatchState::Sealed);

    ASSERT_EQ(restored.batch.observations().size(), original.batch.observations().size());

    for (std::size_t i = 0; i < original.batch.observations().size(); ++i) {
        ASSERT_EQ(restored.batch.observations()[i].id(), original.batch.observations()[i].id());

        ASSERT_EQ(restored.batch.observations()[i].point(), original.batch.observations()[i].point());
    }
}

TEST(Exchange, PreservesFullInt64Revision) {
    auto original          = document();
    original.base_revision = std::numeric_limits<std::int64_t>::max() - 1;

    const auto encoded = encode_exchange(original);
    const auto json    = Json::parse(encoded);

    ASSERT_TRUE(json.at("base_revision").is_string());

    ASSERT_EQ(decode_exchange(encoded).base_revision, original.base_revision);
}

TEST(Exchange, RejectsFutureFormat) {
    auto json                = Json::parse(encode_exchange(document()));
    json["exchange_version"] = 2;

    ASSERT_THROW((void)decode_exchange(json.dump()), ExchangeError);
}

TEST(Exchange, RejectsMissingField) {
    auto json = Json::parse(encode_exchange(document()));
    json.erase("origin");

    ASSERT_THROW((void)decode_exchange(json.dump()), ExchangeError);
}

TEST(Exchange, RejectsUnknownField) {
    auto json          = Json::parse(encode_exchange(document()));
    json["unexpected"] = true;

    ASSERT_THROW((void)decode_exchange(json.dump()), ExchangeError);
}

TEST(Exchange, RejectsWrongUnits) {
    auto json              = Json::parse(encode_exchange(document()));
    json["batch"]["units"] = "mm";

    ASSERT_THROW((void)decode_exchange(json.dump()), ExchangeError);
}

TEST(Exchange, RejectsNumericRevision) {
    auto json             = Json::parse(encode_exchange(document()));
    json["base_revision"] = 1;

    ASSERT_THROW((void)decode_exchange(json.dump()), ExchangeError);
}

TEST(Exchange, RejectsZeroRevision) {
    auto json             = Json::parse(encode_exchange(document()));
    json["base_revision"] = "0";

    ASSERT_THROW((void)decode_exchange(json.dump()), ExchangeError);
}

TEST(Exchange, RejectsOverflowingRevision) {
    auto json             = Json::parse(encode_exchange(document()));
    json["base_revision"] = "9223372036854775808";

    ASSERT_THROW((void)decode_exchange(json.dump()), ExchangeError);
}

TEST(Exchange, RejectsStringCoordinate) {
    auto json                             = Json::parse(encode_exchange(document()));
    json["batch"]["observations"][0]["x"] = "1.25";

    ASSERT_THROW((void)decode_exchange(json.dump()), ExchangeError);
}

TEST(Exchange, RejectsDuplicateObservationIds) {
    auto json       = Json::parse(encode_exchange(document()));
    const auto copy = json["batch"]["observations"][0];
    json["batch"]["observations"].push_back(copy);

    ASSERT_THROW((void)decode_exchange(json.dump()), ExchangeError);
}

TEST(Exchange, RejectsEmptySealedBatch) {
    auto json                     = Json::parse(encode_exchange(document()));
    json["batch"]["state"]        = "sealed";
    json["batch"]["observations"] = Json::array();

    ASSERT_THROW((void)decode_exchange(json.dump()), ExchangeError);
}

TEST(Exchange, RejectsMalformedJsonAndDuplicateKeys) {
    ASSERT_THROW((void)decode_exchange("{broken"), ExchangeError);

    auto duplicate = encode_exchange(document());
    duplicate.insert(1, "\"origin\":\"other-origin\",");

    ASSERT_THROW((void)decode_exchange(duplicate), ExchangeError);
}

TEST(Exchange, EnforcesTheExactByteLimit) {
    auto encoded = encode_exchange(document());

    encoded.resize(max_exchange_bytes, ' ');
    EXPECT_NO_THROW((void)decode_exchange(encoded));

    encoded.push_back(' ');
    EXPECT_THROW((void)decode_exchange(encoded), ExchangeError);
}

TEST(Exchange, EnforcesObservationCountBelowTheByteLimit) {
    auto json          = Json::parse(encode_exchange(document()));
    auto& observations = json["batch"]["observations"];

    observations = Json::array();

    for (std::size_t i = 0; i < MeasurementBatch::max_observations; ++i) {
        observations.push_back({{"id", std::to_string(i)}, {"x", 1.0}, {"y", 2.0}, {"z", 3.0}});
    }

    const auto at_limit = json.dump();
    ASSERT_LT(at_limit.size(), max_exchange_bytes);

    const auto decoded = decode_exchange(at_limit);

    EXPECT_EQ(decoded.batch.observations().size(), MeasurementBatch::max_observations);

    observations.push_back({{"id", "one-too-many"}, {"x", 1.0}, {"y", 2.0}, {"z", 3.0}});

    const auto over_limit = json.dump();
    ASSERT_LT(over_limit.size(), max_exchange_bytes);

    EXPECT_THROW((void)decode_exchange(over_limit), ExchangeError);
}

TEST(Exchange, MaximumPermittedBatchFitsAndRoundTrips) {
    ExchangeDocument large{OriginId{maximally_escaped_id(0)}, std::numeric_limits<std::int64_t>::max(),
                           MeasurementBatch{BatchId{maximally_escaped_id(1)}, DeviceId{maximally_escaped_id(2)},
                                            FrameId{maximally_escaped_id(3)}}};

    const Point3D point{std::numeric_limits<double>::lowest(), -std::numeric_limits<double>::min(),
                        -std::numeric_limits<double>::denorm_min()};

    for (std::size_t i = 0; i < MeasurementBatch::max_observations; ++i) {
        large.batch.add(Observation{ObservationId{maximally_escaped_id(i + 4U)}, point});
    }

    large.batch.seal();

    const auto encoded = encode_exchange(large);
    ASSERT_LE(encoded.size(), max_exchange_bytes);

    RecordProperty("encoded_bytes", std::to_string(encoded.size()));

    const auto restored = decode_exchange(encoded);

    EXPECT_EQ(restored.origin, large.origin);
    EXPECT_EQ(restored.base_revision, large.base_revision);
    EXPECT_EQ(restored.batch.id(), large.batch.id());
    EXPECT_EQ(restored.batch.device(), large.batch.device());
    EXPECT_EQ(restored.batch.frame(), large.batch.frame());
    EXPECT_EQ(restored.batch.state(), BatchState::Sealed);

    ASSERT_EQ(restored.batch.observations().size(), MeasurementBatch::max_observations);

    for (std::size_t i = 0; i < restored.batch.observations().size(); ++i) {
        EXPECT_EQ(restored.batch.observations()[i].id(), large.batch.observations()[i].id());

        EXPECT_EQ(restored.batch.observations()[i].point(), point);
    }
}