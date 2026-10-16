#pragma once

#include "survey/repository.hpp"

#include <filesystem>
#include <memory>

namespace survey {

// One repository instance per thread; separate instances may use the same file.
class SqliteBatchRepository final : public BatchRepository {
public:
    explicit SqliteBatchRepository(const std::filesystem::path& path);
    ~SqliteBatchRepository() override;
    SqliteBatchRepository(const SqliteBatchRepository&)            = delete;
    SqliteBatchRepository& operator=(const SqliteBatchRepository&) = delete;
    [[nodiscard]] std::optional<StoredBatch> load(const BatchId& id) override;
    [[nodiscard]] std::int64_t save(const MeasurementBatch& batch,
                                    std::optional<std::int64_t> expected_revision) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace survey
