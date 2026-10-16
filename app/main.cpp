#include "survey/sqlite_repository.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>

int main(int argc, char** argv) {
    try {
        const auto path = argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::path{"survey-demo.db"};
        survey::SqliteBatchRepository repository{path};
        const survey::BatchId id{"demo-batch"};
        if (!repository.load(id)) {
            survey::MeasurementBatch batch{id, survey::DeviceId{"instrument-1"}, survey::FrameId{"LOCAL_METRES"}};
            batch.add(survey::Observation{survey::ObservationId{"p-1"}, survey::Point3D{1.0, 2.0, 3.0}});
            batch.add(survey::Observation{survey::ObservationId{"p-2"}, survey::Point3D{-4.0, 5.0, 6.0}});
            batch.seal();
            const auto revision = repository.save(batch, std::nullopt);
            std::cout << "Created and sealed demo batch; revision=" << revision << '\n';
        }
        const auto stored = repository.load(id);
        if (!stored) {
            throw survey::DatabaseError("Demo batch could not be loaded");
        }
        const auto box = survey::bounds(stored->batch.observations());
        std::cout << "Loaded " << stored->batch.observations().size() << " observations in frame "
                  << stored->batch.frame().value() << "; revision=" << stored->revision << '\n';
        if (box) {
            std::cout << "X bounds: " << box->minimum.x() << " .. " << box->maximum.x() << " metres\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
