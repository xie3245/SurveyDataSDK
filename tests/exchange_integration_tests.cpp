#include "survey/exchange.hpp"
#include "survey/sqlite_repository.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>

using namespace survey;

namespace {

MeasurementBatch batch() {
    MeasurementBatch result{BatchId{"batch-1"}, DeviceId{"device-1"}, FrameId{"LOCAL_METRES"}};

    result.add(Observation{ObservationId{"original"}, Point3D{1.2345678901234567, -2.5, 3.75}});

    return result;
}

Observation extra(std::string id) { return {ObservationId{std::move(id)}, Point3D{10.0, 20.0, 30.0}}; }

ExchangeDocument document() { return {OriginId{"app-a"}, 1, batch()}; }

}  // namespace

TEST(Exchange, RoundTripKeepsOriginAndReceiverRevisionsSeparate) {
    SqliteBatchRepository app_a{":memory:"};
    SqliteBatchRepository app_b{":memory:"};

    auto source = batch();
    ASSERT_EQ(app_a.save(source, std::nullopt), 1);

    source.add(extra("a-second"));
    ASSERT_EQ(app_a.save(source, 1), 2);

    source.add(extra("a-third"));
    ASSERT_EQ(app_a.save(source, 2), 3);

    const auto checkout = decode_exchange(encode_exchange(export_checkout(app_a, OriginId{"app-a"}, source.id())));

    ASSERT_EQ(checkout.base_revision, 3);
    ASSERT_EQ(import_checkout(app_b, checkout), 1);

    auto local = app_b.load(source.id());
    ASSERT_TRUE(local.has_value());

    local->batch.add(extra("from-b"));
    local->batch.seal();

    ASSERT_EQ(app_b.save(local->batch, local->revision), 2);

    const auto returned = decode_exchange(encode_exchange(export_return(app_b, checkout)));

    ASSERT_EQ(returned.base_revision, 3);
    ASSERT_EQ(apply_return(app_a, OriginId{"app-a"}, returned), 4);

    const auto actual = app_a.load(source.id());
    ASSERT_TRUE(actual.has_value());
    ASSERT_EQ(actual->revision, 4);
    ASSERT_EQ(actual->batch.state(), BatchState::Sealed);
    ASSERT_EQ(actual->batch.observations().size(), 4);

    ASSERT_EQ(actual->batch.observations()[0].id(), ObservationId{"original"});

    ASSERT_EQ(actual->batch.observations()[0].point(), source.observations()[0].point());

    ASSERT_NE(actual->batch.find(ObservationId{"from-b"}), nullptr);
}

TEST(Exchange, StaleReturnPreservesNewerOriginData) {
    SqliteBatchRepository app_a{":memory:"};
    auto source = batch();

    ASSERT_EQ(app_a.save(source, std::nullopt), 1);

    auto returned = export_checkout(app_a, OriginId{"app-a"}, source.id());

    returned.batch.add(extra("from-b"));
    source.add(extra("newer-in-a"));

    ASSERT_EQ(app_a.save(source, 1), 2);

    ASSERT_THROW((void)apply_return(app_a, OriginId{"app-a"}, returned), ConcurrencyConflict);

    const auto actual = app_a.load(source.id());
    ASSERT_TRUE(actual.has_value());
    ASSERT_EQ(actual->revision, 2);

    ASSERT_NE(actual->batch.find(ObservationId{"newer-in-a"}), nullptr);

    ASSERT_EQ(actual->batch.find(ObservationId{"from-b"}), nullptr);
}

TEST(Exchange, ReturnCannotBeAppliedTwice) {
    SqliteBatchRepository app_a{":memory:"};
    auto source = batch();

    ASSERT_EQ(app_a.save(source, std::nullopt), 1);

    auto returned = export_checkout(app_a, OriginId{"app-a"}, source.id());

    returned.batch.add(extra("from-b"));

    ASSERT_EQ(apply_return(app_a, OriginId{"app-a"}, returned), 2);

    ASSERT_THROW((void)apply_return(app_a, OriginId{"app-a"}, returned), ConcurrencyConflict);

    const auto actual = app_a.load(source.id());
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(actual->batch.observations().size(), 2U);
}

TEST(Exchange, WrongOriginDoesNotChangeData) {
    SqliteBatchRepository app_a{":memory:"};
    auto source = batch();

    ASSERT_EQ(app_a.save(source, std::nullopt), 1);

    auto returned = export_checkout(app_a, OriginId{"app-a"}, source.id());

    returned.origin = OriginId{"different-app"};

    ASSERT_THROW((void)apply_return(app_a, OriginId{"app-a"}, returned), ExchangeError);

    const auto actual = app_a.load(source.id());
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(actual->revision, 1);
}

TEST(Exchange, ReturnCannotRemoveObservations) {
    SqliteBatchRepository app_a{":memory:"};
    auto source = batch();

    ASSERT_EQ(app_a.save(source, std::nullopt), 1);

    auto returned = export_checkout(app_a, OriginId{"app-a"}, source.id());

    returned.batch = MeasurementBatch{source.id(), source.device(), source.frame()};

    ASSERT_THROW((void)apply_return(app_a, OriginId{"app-a"}, returned), ExchangeError);

    const auto actual = app_a.load(source.id());
    ASSERT_TRUE(actual.has_value());

    ASSERT_EQ(actual->batch.observations().size(), source.observations().size());

    ASSERT_EQ(actual->revision, 1);

    ASSERT_EQ(actual->batch.observations()[0].point(), source.observations()[0].point());
}

TEST(Exchange, ReturnCannotChangeExistingValues) {
    SqliteBatchRepository app_a{":memory:"};
    auto source = batch();

    ASSERT_EQ(app_a.save(source, std::nullopt), 1);

    auto returned = export_checkout(app_a, OriginId{"app-a"}, source.id());

    returned.batch = MeasurementBatch{source.id(), source.device(), source.frame()};

    returned.batch.add(Observation{ObservationId{"original"}, Point3D{99.0, 0.0, 0.0}});

    ASSERT_THROW((void)apply_return(app_a, OriginId{"app-a"}, returned), ExchangeError);

    const auto actual = app_a.load(source.id());
    ASSERT_TRUE(actual.has_value());

    ASSERT_EQ(actual->batch.observations().size(), source.observations().size());

    ASSERT_EQ(actual->revision, 1);

    ASSERT_EQ(actual->batch.observations()[0].point(), source.observations()[0].point());
}

TEST(Exchange, ReturnCannotChangeFrame) {
    SqliteBatchRepository app_a{":memory:"};
    auto source = batch();

    ASSERT_EQ(app_a.save(source, std::nullopt), 1);

    auto returned = export_checkout(app_a, OriginId{"app-a"}, source.id());

    returned.batch = MeasurementBatch{source.id(), source.device(), FrameId{"OTHER_FRAME"}};

    returned.batch.add(source.observations()[0]);

    ASSERT_THROW((void)apply_return(app_a, OriginId{"app-a"}, returned), ExchangeError);

    const auto actual = app_a.load(source.id());
    ASSERT_TRUE(actual.has_value());

    ASSERT_EQ(actual->batch.observations().size(), source.observations().size());

    ASSERT_EQ(actual->revision, 1);

    ASSERT_EQ(actual->batch.observations()[0].point(), source.observations()[0].point());
}

TEST(Exchange, ReturnCannotChangeDevice) {
    SqliteBatchRepository app_a{":memory:"};
    auto source = batch();

    ASSERT_EQ(app_a.save(source, std::nullopt), 1);

    auto returned = export_checkout(app_a, OriginId{"app-a"}, source.id());

    returned.batch = MeasurementBatch{source.id(), DeviceId{"OTHER_DEVICE"}, source.frame()};

    returned.batch.add(source.observations()[0]);

    ASSERT_THROW((void)apply_return(app_a, OriginId{"app-a"}, returned), ExchangeError);

    const auto actual = app_a.load(source.id());
    ASSERT_TRUE(actual.has_value());

    ASSERT_EQ(actual->batch.observations().size(), source.observations().size());

    ASSERT_EQ(actual->revision, 1);

    ASSERT_EQ(actual->batch.observations()[0].point(), source.observations()[0].point());
}

TEST(Exchange, CheckoutCannotReplaceLocalWork) {
    SqliteBatchRepository app_b{":memory:"};
    const auto checkout = document();

    ASSERT_EQ(import_checkout(app_b, checkout), 1);

    auto local = app_b.load(checkout.batch.id());
    ASSERT_TRUE(local.has_value());

    local->batch.add(extra("local-work"));

    ASSERT_EQ(app_b.save(local->batch, local->revision), 2);

    ASSERT_THROW((void)import_checkout(app_b, checkout), ConcurrencyConflict);

    const auto actual = app_b.load(checkout.batch.id());
    ASSERT_TRUE(actual.has_value());

    EXPECT_NE(actual->batch.find(ObservationId{"local-work"}), nullptr);
}

TEST(Exchange, ReturnCannotReopenSealedOrigin) {
    SqliteBatchRepository app_a{":memory:"};
    auto source = batch();
    source.seal();

    ASSERT_EQ(app_a.save(source, std::nullopt), 1);

    auto returned = export_checkout(app_a, OriginId{"app-a"}, source.id());

    returned.batch = batch();

    ASSERT_THROW((void)apply_return(app_a, OriginId{"app-a"}, returned), DomainError);

    const auto actual = app_a.load(source.id());
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(actual->batch.state(), BatchState::Sealed);
}