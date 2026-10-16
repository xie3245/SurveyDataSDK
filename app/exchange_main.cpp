#include "survey/exchange.hpp"
#include "survey/sqlite_repository.hpp"

#include <charconv>
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
int usage() {
    std::cerr << "Usage:\n"
                 "  survey_exchange seed DB\n"
                 "  survey_exchange export DB ORIGIN BATCH_ID OUTPUT.json\n"
                 "  survey_exchange checkout DB INPUT.json\n"
                 "  survey_exchange append DB BATCH_ID OBSERVATION_ID X Y Z\n"
                 "  survey_exchange return DB ORIGINAL_CHECKOUT.json OUTPUT.json\n"
                 "  survey_exchange apply DB EXPECTED_ORIGIN RETURNED.json\n"
                 "  survey_exchange seal DB BATCH_ID\n"
                 "  survey_exchange show DB BATCH_ID\n";
    return 64;
}

int expected_arguments(std::string_view command) {
    if (command == "seed") {
        return 3;
    }
    if (command == "export") {
        return 6;
    }
    if (command == "checkout") {
        return 4;
    }
    if (command == "append") {
        return 8;
    }
    if (command == "return" || command == "apply") {
        return 5;
    }
    if (command == "seal" || command == "show") {
        return 4;
    }
    return 0;
}

double number(std::string_view input) {
    double result     = 0.0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size()) {
        throw survey::DomainError("Coordinates must be valid numbers");
    }
    return result;
}

survey::StoredBatch load_required(survey::BatchRepository& repository, const survey::BatchId& id) {
    auto stored = repository.load(id);
    if (!stored) {
        throw survey::DomainError("Batch does not exist");
    }
    return std::move(*stored);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    const std::string_view command{argv[1]};
    if (argc != expected_arguments(command)) {
        return usage();
    }
    try {
        survey::SqliteBatchRepository repository{argv[2]};
        if (command == "seed") {
            survey::MeasurementBatch batch{survey::BatchId{"demo-batch"}, survey::DeviceId{"instrument-1"},
                                           survey::FrameId{"LOCAL_METRES"}};
            batch.add(survey::Observation{survey::ObservationId{"original"}, survey::Point3D{1.25, 2.5, 3.75}});
            const auto revision = repository.save(batch, std::nullopt);
            std::cout << "Created open demo-batch, local revision=" << revision << '\n';
        } else if (command == "export") {
            const auto document =
                survey::export_checkout(repository, survey::OriginId{argv[3]}, survey::BatchId{argv[4]});
            survey::write_exchange(argv[5], document);
            std::cout << "Exported checkout, origin=" << document.origin.value()
                      << " base_revision=" << document.base_revision << '\n';
        } else if (command == "checkout") {
            const auto document = survey::read_exchange(argv[3]);
            const auto local    = survey::import_checkout(repository, document);
            std::cout << "Imported checkout, local revision=" << local
                      << " origin base_revision=" << document.base_revision << '\n';
        } else if (command == "append") {
            auto stored = load_required(repository, survey::BatchId{argv[3]});
            stored.batch.add(survey::Observation{survey::ObservationId{argv[4]},
                                                 survey::Point3D{number(argv[5]), number(argv[6]), number(argv[7])}});
            const auto next = repository.save(stored.batch, stored.revision);
            std::cout << "Appended observation, local revision=" << next << '\n';
        } else if (command == "return") {
            const auto checkout = survey::read_exchange(argv[3]);
            const auto document = survey::export_return(repository, checkout);
            survey::write_exchange(argv[4], document);
            std::cout << "Exported return; retained origin base_revision=" << document.base_revision << '\n';
        } else if (command == "apply") {
            const auto document = survey::read_exchange(argv[4]);
            const auto next     = survey::apply_return(repository, survey::OriginId{argv[3]}, document);
            std::cout << "Applied return, origin revision=" << next << '\n';
        } else if (command == "seal") {
            auto stored = load_required(repository, survey::BatchId{argv[3]});
            stored.batch.seal();
            const auto next = repository.save(stored.batch, stored.revision);
            std::cout << "Sealed batch, local revision=" << next << '\n';
        } else {
            const auto stored = load_required(repository, survey::BatchId{argv[3]});
            std::cout << "batch=" << stored.batch.id().value() << " revision=" << stored.revision
                      << " observations=" << stored.batch.observations().size()
                      << " state=" << (stored.batch.state() == survey::BatchState::Open ? "open" : "sealed") << '\n';
            for (const auto& observation : stored.batch.observations()) {
                std::cout << "  id=" << observation.id().value() << " x=" << observation.point().x()
                          << " y=" << observation.point().y() << " z=" << observation.point().z() << '\n';
            }
        }
        return 0;
    } catch (const survey::ConcurrencyConflict& error) {
        std::cerr << "CONFLICT: " << error.what() << '\n';
        return 2;
    } catch (const survey::ExchangeError& error) {
        std::cerr << "EXCHANGE ERROR: " << error.what() << '\n';
        return 3;
    } catch (const survey::DomainError& error) {
        std::cerr << "DOMAIN ERROR: " << error.what() << '\n';
        return 4;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
