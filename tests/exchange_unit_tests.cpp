#include "survey/exchange.hpp"

#include <gmock/gmock.h>

#include <cstdint>
#include <optional>
#include <utility>

using namespace survey;
using ::testing::_;
using ::testing::Return;
using ::testing::StrictMock;
using ::testing::Throw;
using ::testing::Truly;

namespace {

// StrictMock derives from this class, so it must not be final.
class RepositoryMock : public BatchRepository {
public:
    MOCK_METHOD(std::optional<StoredBatch>, load, (const BatchId&), (override));

    MOCK_METHOD(std::int64_t, save, (const MeasurementBatch&, std::optional<std::int64_t>), (override));
};

MeasurementBatch make_batch() {
    MeasurementBatch result{BatchId{"batch-1"}, DeviceId{"device-1"}, FrameId{"LOCAL_METRES"}};

    result.add(Observation{ObservationId{"original"}, Point3D{1.0, 2.0, 3.0}});

    return result;
}

ExchangeDocument make_return() {
    auto batch = make_batch();

    batch.add(Observation{ObservationId{"from-b"}, Point3D{10.0, 20.0, 30.0}});

    return {OriginId{"app-a"}, 7, std::move(batch)};
}

}  // namespace

TEST(ExchangeUnit, ReturnPreservesOriginRevisionRatherThanReceiverRevision) {
    StrictMock<RepositoryMock> receiver;

    const ExchangeDocument checkout{OriginId{"app-a"}, 7, make_batch()};

    const auto edited = make_return();
    const std::optional<StoredBatch> local{StoredBatch{edited.batch, 42}};

    EXPECT_CALL(receiver, load(checkout.batch.id())).WillOnce(Return(local));

    EXPECT_CALL(receiver, save(_, _)).Times(0);

    const auto returned = export_return(receiver, checkout);

    EXPECT_EQ(returned.origin, checkout.origin);
    EXPECT_EQ(returned.base_revision, 7);
    EXPECT_NE(returned.batch.find(ObservationId{"from-b"}), nullptr);
}

TEST(ExchangeUnit, StaleReturnsAreRejectedWithoutSaving) {
    StrictMock<RepositoryMock> origin;
    const auto returned = make_return();

    const std::optional<StoredBatch> current{StoredBatch{make_batch(), 8}};

    EXPECT_CALL(origin, load(returned.batch.id())).WillOnce(Return(current));

    EXPECT_CALL(origin, save(_, _)).Times(0);

    EXPECT_THROW((void)apply_return(origin, OriginId{"app-a"}, returned), ConcurrencyConflict);
}

TEST(ExchangeUnit, AcceptedReturnsUseAConditionalSave) {
    StrictMock<RepositoryMock> origin;
    const auto returned = make_return();

    const std::optional<StoredBatch> current{StoredBatch{make_batch(), 7}};

    EXPECT_CALL(origin, load(returned.batch.id())).WillOnce(Return(current));

    EXPECT_CALL(origin, save(Truly([](const MeasurementBatch& batch) {
                                 return batch.id() == BatchId{"batch-1"} && batch.observations().size() == 2 &&
                                        batch.find(ObservationId{"original"}) != nullptr &&
                                        batch.find(ObservationId{"from-b"}) != nullptr;
                             }),
                             std::optional<std::int64_t>{7}))
        .WillOnce(Return(8));

    EXPECT_EQ(apply_return(origin, OriginId{"app-a"}, returned), 8);
}

TEST(ExchangeUnit, WrongOriginReturnsDoNotAccessStorage) {
    StrictMock<RepositoryMock> origin;
    const auto returned = make_return();

    EXPECT_CALL(origin, load(_)).Times(0);
    EXPECT_CALL(origin, save(_, _)).Times(0);

    EXPECT_THROW((void)apply_return(origin, OriginId{"another-app"}, returned), ExchangeError);
}

TEST(ExchangeUnit, AConflictDuringTheFinalSaveReachesTheCaller) {
    StrictMock<RepositoryMock> origin;
    const auto returned = make_return();

    const std::optional<StoredBatch> current{StoredBatch{make_batch(), 7}};

    EXPECT_CALL(origin, load(returned.batch.id())).WillOnce(Return(current));

    EXPECT_CALL(origin, save(_, std::optional<std::int64_t>{7}))
        .WillOnce(Throw(ConcurrencyConflict{"Changed after load"}));

    EXPECT_THROW((void)apply_return(origin, OriginId{"app-a"}, returned), ConcurrencyConflict);
}

TEST(ExchangeUnit, ImportRequestsCreationWithoutUsingTheOriginRevision) {
    StrictMock<RepositoryMock> receiver;
    const auto checkout = make_return();

    EXPECT_CALL(receiver, load(_)).Times(0);

    EXPECT_CALL(receiver,
                save(Truly([&checkout](const MeasurementBatch& batch) { return batch.id() == checkout.batch.id(); }),
                     std::optional<std::int64_t>{}))
        .WillOnce(Return(1));

    EXPECT_EQ(import_checkout(receiver, checkout), 1);
}