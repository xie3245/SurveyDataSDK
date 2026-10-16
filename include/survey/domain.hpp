#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace survey {

class DomainError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {
[[nodiscard]] bool valid_utf8(std::string_view value) noexcept;
}  // namespace detail

// Tags prevent passing one kind of ID where another is required.
template <class Tag>
class Id {
public:
    static constexpr std::size_t max_bytes = 128;

    explicit Id(std::string value) : value_(std::move(value)) {
        if (value_.empty() || value_.size() > max_bytes || value_.find('\0') != std::string::npos ||
            !detail::valid_utf8(value_)) {
            throw DomainError("IDs must contain 1..128 UTF-8 bytes and no embedded NUL");
        }
    }

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    bool operator==(const Id&) const = default;

private:
    std::string value_;
};

struct BatchTag {};
struct DeviceTag {};
struct ObservationTag {};
struct FrameTag {};

using BatchId       = Id<BatchTag>;
using DeviceId      = Id<DeviceTag>;
using ObservationId = Id<ObservationTag>;
using FrameId       = Id<FrameTag>;

struct IdHash {
    template <class Tag>
    std::size_t operator()(const Id<Tag>& id) const noexcept {
        return std::hash<std::string>{}(id.value());
    }
};

// Finite coordinates in metres. The batch identifies their frame.
class Point3D {
public:
    Point3D(double x_metres, double y_metres, double z_metres);

    [[nodiscard]] double x() const noexcept { return x_; }
    [[nodiscard]] double y() const noexcept { return y_; }
    [[nodiscard]] double z() const noexcept { return z_; }

    bool operator==(const Point3D&) const = default;

private:
    double x_;
    double y_;
    double z_;
};

class Observation {
public:
    Observation(ObservationId id, Point3D point) : id_(std::move(id)), point_(point) {}

    [[nodiscard]] const ObservationId& id() const noexcept { return id_; }

    [[nodiscard]] const Point3D& point() const noexcept { return point_; }

private:
    ObservationId id_;
    Point3D point_;
};

enum class BatchState { Open, Sealed };

// Aggregate root: controls the batch and its observation rules.
class MeasurementBatch {
public:
    // Chosen together with the JSON size budget in exchange.cpp.
    static constexpr std::size_t max_observations = 8'000;

    MeasurementBatch(BatchId id, DeviceId device, FrameId frame);

    void add(Observation observation);
    void seal();

    [[nodiscard]] const BatchId& id() const noexcept { return id_; }

    [[nodiscard]] const DeviceId& device() const noexcept { return device_; }

    [[nodiscard]] const FrameId& frame() const noexcept { return frame_; }

    [[nodiscard]] BatchState state() const noexcept { return state_; }

    [[nodiscard]] std::span<const Observation> observations() const& noexcept { return observations_; }

    std::span<const Observation> observations() const&& = delete;

    [[nodiscard]] const Observation* find(const ObservationId& id) const& noexcept;

    const Observation* find(const ObservationId& id) const&& = delete;

private:
    BatchId id_;
    DeviceId device_;
    FrameId frame_;
    BatchState state_{BatchState::Open};
    std::vector<Observation> observations_;
    std::unordered_map<ObservationId, std::size_t, IdHash> by_id_;
};

struct Bounds3D {
    Point3D minimum;
    Point3D maximum;
};

[[nodiscard]] std::optional<Bounds3D> bounds(std::span<const Observation> observations);

}  // namespace survey