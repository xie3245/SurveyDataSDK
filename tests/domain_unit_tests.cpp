#include "survey/domain.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

using namespace survey;

namespace {

MeasurementBatch batch() { return {BatchId{"batch-1"}, DeviceId{"device-1"}, FrameId{"LOCAL_METRES"}}; }

Observation observation(std::string id, double x = 1.0) { return {ObservationId{std::move(id)}, Point3D{x, 2.0, 3.0}}; }

static_assert(!std::is_convertible_v<DeviceId, BatchId>);
static_assert(!std::is_convertible_v<std::string, BatchId>);

}  // namespace

TEST(Domain, RejectsInvalidIds) {
    ASSERT_THROW((void)BatchId{std::string{}}, DomainError);
    ASSERT_THROW((void)BatchId{std::string(129, 'x')}, DomainError);
    ASSERT_THROW((void)(BatchId{std::string{"a\0b", 3}}), DomainError);
}

TEST(Domain, AcceptsAnIdAtTheLengthLimit) {
    const BatchId id{std::string(128, 'x')};
    EXPECT_EQ(id.value().size(), 128U);
}

TEST(Domain, AcceptsUtf8Ids) {
    const std::string value = "id-\xC2\xA3-\xE4\xB8\xAD-\xF0\x9F\x98\x80";

    EXPECT_EQ(BatchId{value}.value(), value);
}

TEST(Domain, RejectsMalformedUtf8Ids) {
    const std::array invalid{std::string{"\x80"},
                             std::string{"\xC0\xAF"},
                             std::string{"\xC2"},
                             std::string{"\xC2\x41"},
                             std::string{"\xED\xA0\x80"},
                             std::string{"\xF4\x90\x80\x80"},
                             std::string{"\xF5\x80\x80\x80"}};

    for (const auto& value : invalid) {
        SCOPED_TRACE(::testing::PrintToString(value));
        EXPECT_THROW((void)BatchId{value}, DomainError);
    }
}

TEST(Domain, RejectsNonFiniteCoordinates) {
    const Point3D a{1.0, 2.0, 3.0};
    const Point3D b{1.0, 2.0, 3.0};

    ASSERT_EQ(a, b);

    ASSERT_THROW((void)(Point3D{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}), DomainError);

    ASSERT_THROW((void)(Point3D{0.0, std::numeric_limits<double>::infinity(), 0.0}), DomainError);
}

TEST(Domain, RejectsDuplicateObservationIds) {
    auto b = batch();
    b.add(observation("p-1"));

    ASSERT_THROW((void)b.add(observation("p-1", 99.0)), DomainError);

    ASSERT_EQ(b.observations().size(), 1);

    const auto* found = b.find(ObservationId{"p-1"});
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->point().x(), 1.0);

    ASSERT_EQ(b.find(ObservationId{"missing"}), nullptr);
}

TEST(Domain, OnlyNonEmptyOpenBatchesCanBeSealed) {
    auto b = batch();

    ASSERT_THROW((void)b.seal(), DomainError);

    b.add(observation("p-1"));
    b.seal();

    ASSERT_EQ(b.state(), BatchState::Sealed);

    ASSERT_THROW((void)b.add(observation("p-2")), DomainError);

    ASSERT_THROW((void)b.seal(), DomainError);
}

TEST(Domain, IndexSurvivesVectorGrowth) {
    auto b = batch();

    for (int i = 0; i < 1000; ++i) {
        b.add(observation(std::to_string(i), static_cast<double>(i)));
    }

    for (int i = 0; i < 1000; ++i) {
        const auto* found = b.find(ObservationId{std::to_string(i)});

        ASSERT_NE(found, nullptr);
        ASSERT_EQ(found->point().x(), static_cast<double>(i));
    }
}

TEST(Domain, EnforcesObservationLimit) {
    auto b = batch();

    for (std::size_t i = 0; i < MeasurementBatch::max_observations; ++i) {
        b.add(observation(std::to_string(i)));
    }

    ASSERT_THROW((void)b.add(observation("too-many")), DomainError);

    ASSERT_EQ(b.observations().size(), MeasurementBatch::max_observations);
}

TEST(Domain, ComputesBounds) {
    auto b = batch();

    ASSERT_FALSE(bounds(b.observations()).has_value());

    b.add(observation("a", -4.0));
    b.add(observation("b", 8.0));

    const auto box = bounds(b.observations());
    ASSERT_TRUE(box.has_value());

    ASSERT_EQ(box->minimum.x(), -4.0);
    ASSERT_EQ(box->maximum.x(), 8.0);
}