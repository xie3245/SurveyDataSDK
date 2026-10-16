#pragma once

#include "survey/repository.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace survey {

struct OriginTag {};
using OriginId = Id<OriginTag>;

class ExchangeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// base_revision always belongs to origin, never to the receiving app's database.
struct ExchangeDocument {
    OriginId origin;
    std::int64_t base_revision;
    MeasurementBatch batch;
};

inline constexpr std::size_t max_exchange_bytes = std::size_t{8} * 1024 * 1024;

[[nodiscard]] std::string encode_exchange(const ExchangeDocument& document);
[[nodiscard]] ExchangeDocument decode_exchange(std::string_view text);
[[nodiscard]] ExchangeDocument read_exchange(const std::filesystem::path& path);
void write_exchange(const std::filesystem::path& path, const ExchangeDocument& document);

[[nodiscard]] ExchangeDocument export_checkout(BatchRepository& source, const OriginId& origin, const BatchId& id);
[[nodiscard]] std::int64_t import_checkout(BatchRepository& receiver, const ExchangeDocument& document);
[[nodiscard]] ExchangeDocument export_return(BatchRepository& receiver, const ExchangeDocument& original_checkout);
[[nodiscard]] std::int64_t apply_return(BatchRepository& origin,
                                        const OriginId& expected_origin,
                                        const ExchangeDocument& document);

}  // namespace survey
