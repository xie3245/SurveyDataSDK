#include "survey/sqlite_repository.hpp"

#include <sqlite3.h>

#include <limits>
#include <string>
#include <string_view>

namespace survey {
namespace {

struct DbCloser {
    void operator()(sqlite3* db) const noexcept {
        if (db != nullptr) {
            (void)sqlite3_close(db);
        }
    }
};

struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const noexcept { (void)sqlite3_finalize(statement); }
};

using Database = std::unique_ptr<sqlite3, DbCloser>;

[[noreturn]] void fail(sqlite3* db, std::string_view operation) {
    throw DatabaseError(std::string{operation} + ": " + sqlite3_errmsg(db));
}

void exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        fail(db, "SQL execution");
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        sqlite3_stmt* raw = nullptr;
        const int code    = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
        statement_.reset(raw);
        if (code != SQLITE_OK) {
            fail(db_, "Preparing statement");
        }
    }
    void text(int position, std::string_view value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw DatabaseError("Text parameter is too large");
        }
        if (sqlite3_bind_text(statement_.get(), position, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            fail(db_, "Binding text");
        }
    }
    void integer(int position, std::int64_t value) {
        if (sqlite3_bind_int64(statement_.get(), position, value) != SQLITE_OK) {
            fail(db_, "Binding integer");
        }
    }
    void real(int position, double value) {
        if (sqlite3_bind_double(statement_.get(), position, value) != SQLITE_OK) {
            fail(db_, "Binding real");
        }
    }
    [[nodiscard]] bool row() {
        const int result = sqlite3_step(statement_.get());
        if (result == SQLITE_ROW) {
            return true;
        }
        if (result == SQLITE_DONE) {
            return false;
        }
        fail(db_, "Executing statement");
    }
    void done() {
        if (row()) {
            throw DatabaseError("Unexpected row in write statement");
        }
    }
    void reset() {
        if (sqlite3_reset(statement_.get()) != SQLITE_OK || sqlite3_clear_bindings(statement_.get()) != SQLITE_OK) {
            fail(db_, "Resetting statement");
        }
    }
    [[nodiscard]] std::int64_t integer(int column) const { return sqlite3_column_int64(statement_.get(), column); }
    [[nodiscard]] double real(int column) const { return sqlite3_column_double(statement_.get(), column); }
    [[nodiscard]] std::string text(int column) const {
        const auto* value = sqlite3_column_text(statement_.get(), column);
        if (value == nullptr) {
            throw DatabaseError("Unexpected NULL text value");
        }
        return {reinterpret_cast<const char*>(value),
                static_cast<std::size_t>(sqlite3_column_bytes(statement_.get(), column))};
    }

private:
    sqlite3* db_;  // borrowed; the database outlives the statement
    std::unique_ptr<sqlite3_stmt, StatementCloser> statement_;
};

class Transaction {
public:
    explicit Transaction(sqlite3* db, bool write) : db_(db) { exec(db_, write ? "BEGIN IMMEDIATE" : "BEGIN"); }
    ~Transaction() {
        if (active_) {
            (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    Transaction(const Transaction&)            = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() {
        exec(db_, "COMMIT");
        active_ = false;
    }

private:
    sqlite3* db_;
    bool active_{true};
};

std::int64_t schema_version(sqlite3* db) {
    Statement version{db, "PRAGMA user_version"};
    if (!version.row()) {
        throw DatabaseError("Missing schema version");
    }
    return version.integer(0);
}

void migrate(sqlite3* db) {
    Transaction transaction{db, true};
    const auto version = schema_version(db);
    if (version == 0) {
        exec(db, R"sql(
            CREATE TABLE batches (
                id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                state INTEGER NOT NULL CHECK(state IN (0,1)),
                revision INTEGER NOT NULL CHECK(revision >= 1),
                frame TEXT NOT NULL
            ) STRICT;
            CREATE TABLE observations (
                batch_id TEXT NOT NULL REFERENCES batches(id) ON DELETE CASCADE,
                id TEXT NOT NULL,
                ordinal INTEGER NOT NULL CHECK(ordinal >= 0),
                x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL,
                PRIMARY KEY(batch_id,id), UNIQUE(batch_id,ordinal)
            ) STRICT;
            PRAGMA user_version=2;
        )sql");
    } else if (version == 1) {
        // The v1 format explicitly used local coordinates in metres.
        exec(db,
             "ALTER TABLE batches ADD COLUMN frame TEXT NOT NULL DEFAULT 'LOCAL_METRES';"
             "PRAGMA user_version=2;");
    } else if (version != 2) {
        throw DatabaseError("Unsupported schema version: " + std::to_string(version));
    }
    transaction.commit();
}

Database open(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    const std::string utf8{encoded.begin(), encoded.end()};
    sqlite3* raw   = nullptr;
    const int code = sqlite3_open_v2(utf8.c_str(), &raw,
                                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    Database db{raw};  // ownership immediately, including the error path
    if (code != SQLITE_OK) {
        throw DatabaseError(raw == nullptr ? "Opening database failed" : sqlite3_errmsg(raw));
    }
    if (sqlite3_busy_timeout(db.get(), 1000) != SQLITE_OK) {
        fail(db.get(), "Setting busy timeout");
    }
    exec(db.get(), "PRAGMA foreign_keys=ON");
    exec(db.get(), "PRAGMA synchronous=FULL");
    migrate(db.get());
    return db;
}

}  // namespace

struct SqliteBatchRepository::Impl {
    explicit Impl(const std::filesystem::path& path) : db(open(path)) {}
    Database db;
};

SqliteBatchRepository::SqliteBatchRepository(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path)) {}
SqliteBatchRepository::~SqliteBatchRepository() = default;

std::optional<StoredBatch> SqliteBatchRepository::load(const BatchId& id) {
    auto* db = impl_->db.get();
    Transaction transaction{db, false};  // one snapshot for header AND observations
    try {
        Statement header{db, "SELECT device_id,state,revision,frame FROM batches WHERE id=?"};
        header.text(1, id.value());
        if (!header.row()) {
            transaction.commit();
            return std::nullopt;
        }
        const auto state    = header.integer(1);
        const auto revision = header.integer(2);
        if ((state != 0 && state != 1) || revision < 1) {
            throw DatabaseError("Invalid stored state or revision");
        }
        MeasurementBatch batch{id, DeviceId{header.text(0)}, FrameId{header.text(3)}};
        Statement observations{db, "SELECT id,x,y,z FROM observations WHERE batch_id=? ORDER BY ordinal"};
        observations.text(1, id.value());
        while (observations.row()) {
            batch.add(Observation{ObservationId{observations.text(0)},
                                  Point3D{observations.real(1), observations.real(2), observations.real(3)}});
        }
        if (state == 1) {
            batch.seal();
        }
        transaction.commit();
        return StoredBatch{std::move(batch), revision};
    } catch (const DomainError& error) {
        throw DatabaseError(std::string{"Stored data violate domain rules: "} + error.what());
    }
}

std::int64_t SqliteBatchRepository::save(const MeasurementBatch& batch, std::optional<std::int64_t> expected_revision) {
    auto* db = impl_->db.get();
    if (expected_revision &&
        (*expected_revision < 1 || *expected_revision == std::numeric_limits<std::int64_t>::max())) {
        throw DomainError("Expected revision is out of range");
    }
    Transaction transaction{db, true};
    const std::int64_t next_revision = expected_revision ? *expected_revision + 1 : 1;
    if (expected_revision) {
        Statement current{db, "SELECT revision,state FROM batches WHERE id=?"};
        current.text(1, batch.id().value());
        if (!current.row() || current.integer(0) != *expected_revision) {
            throw ConcurrencyConflict("Batch was changed or removed; reload before resolving the conflict");
        }
        if (current.integer(1) == 1) {
            throw DomainError("Stored sealed batches cannot be changed");
        }
        Statement update{db,
                         "UPDATE batches SET device_id=?,state=?,revision=revision+1,frame=? "
                         "WHERE id=? AND revision=?"};
        update.text(1, batch.device().value());
        update.integer(2, batch.state() == BatchState::Sealed ? 1 : 0);
        update.text(3, batch.frame().value());
        update.text(4, batch.id().value());
        update.integer(5, *expected_revision);
        update.done();
        if (sqlite3_changes(db) != 1) {
            throw ConcurrencyConflict("Conditional update did not match");
        }
        Statement remove{db, "DELETE FROM observations WHERE batch_id=?"};
        remove.text(1, batch.id().value());
        remove.done();
    } else {
        Statement insert{db,
                         "INSERT INTO batches(id,device_id,state,revision,frame) VALUES(?,?,?,1,?) "
                         "ON CONFLICT(id) DO NOTHING"};
        insert.text(1, batch.id().value());
        insert.text(2, batch.device().value());
        insert.integer(3, batch.state() == BatchState::Sealed ? 1 : 0);
        insert.text(4, batch.frame().value());
        insert.done();
        if (sqlite3_changes(db) != 1) {
            throw ConcurrencyConflict("Batch already exists; create was not applied");
        }
    }
    Statement insert_observation{db, "INSERT INTO observations(batch_id,id,ordinal,x,y,z) VALUES(?,?,?,?,?,?)"};
    std::int64_t ordinal = 0;
    for (const auto& observation : batch.observations()) {
        insert_observation.text(1, batch.id().value());
        insert_observation.text(2, observation.id().value());
        insert_observation.integer(3, ordinal++);
        insert_observation.real(4, observation.point().x());
        insert_observation.real(5, observation.point().y());
        insert_observation.real(6, observation.point().z());
        insert_observation.done();
        insert_observation.reset();
    }
    transaction.commit();
    return next_revision;
}

}  // namespace survey
