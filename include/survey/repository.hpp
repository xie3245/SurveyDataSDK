#pragma once

#include "survey/domain.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>

namespace survey {

class DatabaseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ConcurrencyConflict : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct StoredBatch {
    MeasurementBatch batch;
    std::int64_t revision;
};

// An application-facing port expressed in domain types.
class BatchRepository {
public:
    virtual ~BatchRepository()                                               = default;
    [[nodiscard]] virtual std::optional<StoredBatch> load(const BatchId& id) = 0;
    // nullopt = create only. Otherwise update only the specified revision.
    [[nodiscard]] virtual std::int64_t save(const MeasurementBatch& batch,
                                            std::optional<std::int64_t> expected_revision) = 0;
};

}  // namespace survey
