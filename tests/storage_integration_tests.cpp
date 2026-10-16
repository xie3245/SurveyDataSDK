#include "survey/sqlite_repository.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <utility>

using namespace survey;

namespace {

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        std::random_device random;

        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto suffix = std::to_string(random()) + "-" + std::to_string(random());

            directory_ = std::filesystem::temp_directory_path() / ("survey-sdk-" + suffix);

            if (std::filesystem::create_directory(directory_)) {
                return;
            }
        }

        throw std::runtime_error("Cannot create a unique temporary directory");
    }

    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    [[nodiscard]] std::filesystem::path path() const { return directory_ / "test.db"; }

private:
    std::filesystem::path directory_;
};

void raw_sql(const std::filesystem::path& path, const char* sql) {
    sqlite3* raw = nullptr;

    const auto encoded = path.u8string();
    const std::string utf8{encoded.begin(), encoded.end()};
    const int result = sqlite3_open(utf8.c_str(), &raw);

    const auto closer = [](sqlite3* db) {
        if (db) {
            (void)sqlite3_close(db);
        }
    };

    const std::unique_ptr<sqlite3, decltype(closer)> connection{raw, closer};

    if (result != SQLITE_OK || sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(raw ? sqlite3_errmsg(raw) : "SQLite fixture error");
    }
}

MeasurementBatch batch(std::string id = "batch-1") {
    return {BatchId{std::move(id)}, DeviceId{"device-1"}, FrameId{"LOCAL_METRES"}};
}

Observation observation(std::string id) { return {ObservationId{std::move(id)}, Point3D{1.25, -2.5, 3.75}}; }

}  // namespace

TEST(Persistence, SealedBatchSurvivesReopen) {
    TemporaryDatabase temp;

    {
        SqliteBatchRepository repository{temp.path()};
        auto b = batch();

        b.add(observation("b"));
        b.add(observation("a"));
        b.seal();

        ASSERT_EQ(repository.save(b, std::nullopt), 1);
    }

    SqliteBatchRepository reopened{temp.path()};
    const auto stored = reopened.load(BatchId{"batch-1"});

    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->revision, 1);
    ASSERT_EQ(stored->batch.state(), BatchState::Sealed);
    ASSERT_EQ(stored->batch.device(), DeviceId{"device-1"});
    ASSERT_EQ(stored->batch.frame(), FrameId{"LOCAL_METRES"});
    ASSERT_EQ(stored->batch.observations().size(), 2);
    ASSERT_EQ(stored->batch.observations()[0].id(), ObservationId{"b"});
    ASSERT_EQ(stored->batch.observations()[1].id(), ObservationId{"a"});

    ASSERT_EQ(stored->batch.observations()[1].point(), (Point3D{1.25, -2.5, 3.75}));

    ASSERT_FALSE(reopened.load(BatchId{"missing"}).has_value());
}

TEST(Persistence, BoundParametersPreserveQuotedIds) {
    TemporaryDatabase temp;
    SqliteBatchRepository repository{temp.path()};

    MeasurementBatch b{BatchId{"batch'quoted"}, DeviceId{"device'; DROP TABLE batches; --"}, FrameId{"LOCAL_METRES"}};

    b.add(observation("point'quoted"));

    ASSERT_EQ(repository.save(b, std::nullopt), 1);

    const auto stored = repository.load(b.id());
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->batch.device(), b.device());
    ASSERT_EQ(stored->batch.observations().size(), 1);
}

TEST(Persistence, ObservationIdsAreLocalToBatch) {
    TemporaryDatabase temp;
    SqliteBatchRepository repository{temp.path()};

    auto first  = batch("first");
    auto second = batch("second");

    first.add(observation("same-local-id"));
    second.add(observation("same-local-id"));

    ASSERT_EQ(repository.save(first, std::nullopt), 1);
    ASSERT_EQ(repository.save(second, std::nullopt), 1);

    const auto stored_first  = repository.load(first.id());
    const auto stored_second = repository.load(second.id());

    ASSERT_TRUE(stored_first.has_value());
    ASSERT_TRUE(stored_second.has_value());

    EXPECT_EQ(stored_first->batch.observations().size(), 1U);
    EXPECT_EQ(stored_second->batch.observations().size(), 1U);
}

TEST(Concurrency, DuplicateCreateFails) {
    TemporaryDatabase temp;
    SqliteBatchRepository repository{temp.path()};
    auto b = batch();

    ASSERT_EQ(repository.save(b, std::nullopt), 1);

    ASSERT_THROW((void)repository.save(b, std::nullopt), ConcurrencyConflict);
}

TEST(Concurrency, StaleWriterCannotOverwriteChanges) {
    TemporaryDatabase temp;
    SqliteBatchRepository first{temp.path()};
    SqliteBatchRepository second{temp.path()};

    auto initial = batch();
    ASSERT_EQ(first.save(initial, std::nullopt), 1);

    auto alice = first.load(initial.id());
    auto bob   = second.load(initial.id());

    ASSERT_TRUE(alice.has_value());
    ASSERT_TRUE(bob.has_value());

    alice->batch.add(observation("alice-point"));
    bob->batch.add(observation("bob-point"));

    ASSERT_EQ(first.save(alice->batch, alice->revision), 2);

    ASSERT_THROW((void)second.save(bob->batch, bob->revision), ConcurrencyConflict);

    const auto actual = second.load(initial.id());
    ASSERT_TRUE(actual.has_value());
    ASSERT_EQ(actual->revision, 2);

    ASSERT_NE(actual->batch.find(ObservationId{"alice-point"}), nullptr);

    ASSERT_EQ(actual->batch.find(ObservationId{"bob-point"}), nullptr);
}

TEST(Persistence, SealingCannotBeBypassed) {
    TemporaryDatabase temp;
    SqliteBatchRepository repository{temp.path()};

    auto original = batch();
    original.add(observation("first"));
    original.seal();

    ASSERT_EQ(repository.save(original, std::nullopt), 1);

    auto replacement = batch();
    replacement.add(observation("replacement"));

    ASSERT_THROW((void)repository.save(replacement, 1), DomainError);

    const auto stored = repository.load(original.id());
    ASSERT_TRUE(stored.has_value());

    EXPECT_NE(stored->batch.find(ObservationId{"first"}), nullptr);
}

TEST(Rollback, FailedInsertRollsBackWholeSave) {
    TemporaryDatabase temp;
    SqliteBatchRepository repository{temp.path()};

    auto b = batch();
    b.add(observation("original"));

    ASSERT_EQ(repository.save(b, std::nullopt), 1);

    raw_sql(temp.path(), R"sql(
        CREATE TRIGGER injected_failure
        BEFORE INSERT ON observations
        WHEN NEW.id='fail-here'
        BEGIN
            SELECT RAISE(ABORT,'injected write failure');
        END;
    )sql");

    b.add(observation("fail-here"));
    b.seal();

    ASSERT_THROW((void)repository.save(b, 1), DatabaseError);

    const auto stored = repository.load(b.id());
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->revision, 1);
    ASSERT_EQ(stored->batch.state(), BatchState::Open);
    ASSERT_EQ(stored->batch.observations().size(), 1);

    ASSERT_NE(stored->batch.find(ObservationId{"original"}), nullptr);
}

TEST(Migration, UpgradesV1WithoutDataLoss) {
    TemporaryDatabase temp;

    raw_sql(temp.path(), R"sql(
        CREATE TABLE batches(
            id TEXT PRIMARY KEY,
            device_id TEXT NOT NULL,
            state INTEGER NOT NULL CHECK(state IN (0,1)),
            revision INTEGER NOT NULL CHECK(revision>=1)
        ) STRICT;

        CREATE TABLE observations(
            batch_id TEXT NOT NULL
                REFERENCES batches(id) ON DELETE CASCADE,
            id TEXT NOT NULL,
            ordinal INTEGER NOT NULL CHECK(ordinal>=0),
            x REAL NOT NULL,
            y REAL NOT NULL,
            z REAL NOT NULL,
            PRIMARY KEY(batch_id,id),
            UNIQUE(batch_id,ordinal)
        ) STRICT;

        INSERT INTO batches VALUES('legacy','device-1',1,7);
        INSERT INTO observations VALUES(
            'legacy','p-1',0,1.25,-2.5,3.75);
        PRAGMA user_version=1;
    )sql");

    {
        SqliteBatchRepository repository{temp.path()};
        const auto stored = repository.load(BatchId{"legacy"});

        ASSERT_TRUE(stored.has_value());
        ASSERT_EQ(stored->revision, 7);
        ASSERT_EQ(stored->batch.frame(), FrameId{"LOCAL_METRES"});
        ASSERT_EQ(stored->batch.state(), BatchState::Sealed);
        ASSERT_EQ(stored->batch.observations().size(), 1U);

        ASSERT_EQ(stored->batch.observations()[0].point(), (Point3D{1.25, -2.5, 3.75}));
    }

    SqliteBatchRepository reopened{temp.path()};
    ASSERT_TRUE(reopened.load(BatchId{"legacy"}).has_value());
}

TEST(Migration, RejectsFutureSchemaWithoutMutation) {
    TemporaryDatabase temp;

    raw_sql(temp.path(), R"sql(
        PRAGMA user_version=99;
        CREATE TABLE sentinel(value INTEGER);
        INSERT INTO sentinel VALUES(42);
    )sql");

    ASSERT_THROW((void)SqliteBatchRepository{temp.path()}, DatabaseError);

    // These checks only succeed if the original data/version survived.
    raw_sql(temp.path(), R"sql(
        CREATE TABLE verify(value INTEGER CHECK(value=42));
        INSERT INTO verify SELECT value FROM sentinel;

        CREATE TABLE verify_version(value INTEGER CHECK(value=99));
        INSERT INTO verify_version
            SELECT user_version FROM pragma_user_version;
    )sql");
}

TEST(Persistence, RejectsInvalidStoredDomainData) {
    TemporaryDatabase temp;
    SqliteBatchRepository repository{temp.path()};

    auto b = batch();
    ASSERT_EQ(repository.save(b, std::nullopt), 1);

    // SQL permits this state, but the domain rejects an empty sealed batch.
    raw_sql(temp.path(), "UPDATE batches SET state=1 WHERE id='batch-1'");

    ASSERT_THROW((void)repository.load(b.id()), DatabaseError);
}