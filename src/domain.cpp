#include "survey/domain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace survey {

bool detail::valid_utf8(std::string_view value) noexcept {
    std::size_t position = 0;

    while (position < value.size()) {
        const auto first = static_cast<unsigned char>(value[position++]);

        if (first <= 0x7FU) {
            continue;
        }

        std::uint32_t codepoint = 0;
        std::uint32_t minimum   = 0;
        std::size_t trailing    = 0;

        if (first >= 0xC2U && first <= 0xDFU) {
            codepoint = first & 0x1FU;
            minimum   = 0x80U;
            trailing  = 1;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            codepoint = first & 0x0FU;
            minimum   = 0x800U;
            trailing  = 2;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            codepoint = first & 0x07U;
            minimum   = 0x10000U;
            trailing  = 3;
        } else {
            return false;
        }

        if (value.size() - position < trailing) {
            return false;
        }

        for (std::size_t i = 0; i < trailing; ++i) {
            const auto next = static_cast<unsigned char>(value[position++]);

            if ((next & 0xC0U) != 0x80U) {
                return false;
            }

            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }

        if (codepoint < minimum || codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
    }

    return true;
}

Point3D::Point3D(double x_metres, double y_metres, double z_metres) : x_(x_metres), y_(y_metres), z_(z_metres) {
    if (!std::ranges::all_of(std::array{x_, y_, z_}, [](double value) { return std::isfinite(value); })) {
        throw DomainError("Coordinates must be finite");
    }
}

MeasurementBatch::MeasurementBatch(BatchId id, DeviceId device, FrameId frame)
    : id_(std::move(id))
    , device_(std::move(device))
    , frame_(std::move(frame)) {}

void MeasurementBatch::add(Observation observation) {
    if (state_ != BatchState::Open) {
        throw DomainError("A sealed batch cannot accept observations");
    }

    if (observations_.size() >= max_observations) {
        throw DomainError("Batch capacity exceeded");
    }

    const auto [position, inserted] = by_id_.emplace(observation.id(), observations_.size());

    if (!inserted) {
        throw DomainError("Observation IDs must be unique within a batch");
    }

    try {
        observations_.push_back(std::move(observation));
    } catch (...) {
        // Restore consistency if growing the vector fails.
        by_id_.erase(position);
        throw;
    }
}

void MeasurementBatch::seal() {
    if (state_ != BatchState::Open || observations_.empty()) {
        throw DomainError("Only a non-empty open batch can be sealed");
    }

    state_ = BatchState::Sealed;
}

const Observation* MeasurementBatch::find(const ObservationId& id) const& noexcept {
    const auto found = by_id_.find(id);

    return found == by_id_.end() ? nullptr : &observations_[found->second];
}

std::optional<Bounds3D> bounds(std::span<const Observation> observations) {
    if (observations.empty()) {
        return std::nullopt;
    }

    auto lo = observations.front().point();
    auto hi = lo;

    for (const auto& observation : observations.subspan(1)) {
        const auto& point = observation.point();

        lo = Point3D{std::min(lo.x(), point.x()), std::min(lo.y(), point.y()), std::min(lo.z(), point.z())};

        hi = Point3D{std::max(hi.x(), point.x()), std::max(hi.y(), point.y()), std::max(hi.z(), point.z())};
    }

    return Bounds3D{lo, hi};
}

}  // namespace survey