# Survey Data SDK

A learning project for modelling, storing and exchanging small batches of 3D measurements. It combines a domain model, SQLite persistence, JSON exchange, GoogleTest/GoogleMock tests and a CMake build with GitHub Actions.

I used AI assistance for the initial implementation and subsequent changes. My work has focused on studying the code, challenging its assumptions and driving improvements in testing, tooling and consistency. **The examples below show how that learning led to changes and further design questions.**

## Domain

A measurement instrument collects observations into a **batch**. Each observation has an ID and a finite 3D position in metres. A batch belongs to one instrument and one coordinate frame.

- A batch starts **open** and accepts observations with unique IDs within that batch.
- A non-empty batch can be **sealed**. A stored sealed batch cannot be modified, including by submitting a new open object with the same ID.
- A batch contains at most **8,000 observations**. IDs contain at most **128 UTF-8 bytes**, and an exchange document is limited to **8 MiB**. The observation limit and JSON byte budget are checked together.
- `FrameId` names the agreed coordinate frame. The project does not perform coordinate transformations.
- Batch IDs remain unchanged during exchange. SQLite enforces their uniqueness within each database; the current application does not generate globally unique IDs.

The exchange scenario uses two application instances, A and B, with separate SQLite files. A exports a checkout, B imports it and adds measurements, and B returns an updated snapshot. A accepts it only if its batch has not changed since checkout and the returned changes satisfy the exchange policy.

Each database has its own batch `revision`. The document's `base_revision` always refers to the origin's version at checkout, even when B's local revision differs.

## How I studied and improved this project

I used the project to deepen my understanding of data modelling, DDD and persistence. Working through the code led me to question design choices, identify inconsistencies and make targeted improvements to the implementation, tests and tooling.

| Topic                           | What I explored and improved                                                                                                                                                                                                                                                                                                  |
| ------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Unit-test isolation and mocking | Identified that exchange use cases were tested through the real SQLite repository, leaving no isolated unit coverage for that application logic. Added repository mocks to test revision handling, rejected returns and conditional saves independently of SQLite, and retained real database tests in the integration suite. |
| Test framework                  | Migrated from Catch2 to GoogleTest/GoogleMock for the unit and integration test suites due to more convenient mock tooling.                                                                                                                                                                                                   |
| Static analysis and formatting  | Added clang-format for consistent formatting and optional clang-tidy integration for project C++ targets, with selected analysis checks treated as errors.                                                                                                                                                                    |
| Consistent limits               | Identified that a domain-valid batch could exceed the exchange limit. Reduced the observation limit, added a conservative encoding budget and tested a maximum-size batch with heavily escaped IDs.                                                                                                                           |
| Text validation                 | Tightened ID validation so accepted IDs contain valid UTF-8 and can be encoded as JSON.                                                                                                                                                                                                                                       |
| Test organization               | Made unit/integration categories explicit in filenames and CTest labels, and kept a separate process-level exchange test.                                                                                                                                                                                                     |

As part of that learning, I also questioned design choices and identified remaining limitations:

- **Local versus origin revisions:** checked which database each revision belongs to and how a save detects a writer that intervenes between loading and saving.
- **Container choice:** examined the vector-plus-hash-index design, its memory cost and pointer invalidation. Considered a sorted index and a single owning unordered map as alternatives. The single-map option was discussed conceptually; it has not been implemented or benchmarked.
- **Ordering:** questioned whether acquisition order is a domain requirement. The current return policy still requires existing observations to remain an unchanged prefix; treating the batch as an unordered collection would require changing that policy.
- **`base_revision` persistence:** identified that B stores its local revision but retains the origin's `base_revision` only in the original checkout JSON. Creating a return therefore depends on keeping and selecting the correct file. Proposed persisting the checkout metadata alongside the imported batch, while keeping the base revision unchanged during local edits.
- **Origin ID lifetime:** identified that the source store's `OriginId` is supplied on the command line rather than persisted as a stable store identity. Proposed assigning the identity when the store is created and retaining it in that database for subsequent exports and return validation.
- **Batch ID uniqueness:** identified that `BatchId` validates a supplied string but does not generate globally unique IDs. SQLite enforces uniqueness within one database, so independently created batches in different stores can still have matching IDs and collide on import. Proposed generating a UUID once for each new batch and preserving it across exchanged copies.

## Executables

| Executable                 | Purpose                                                                                                                                                           |
| -------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `survey_demo`              | Creates and seals a sample batch on its first run, then loads it and prints its X bounds. Running it again against the same file demonstrates persistence.        |
| `survey_exchange`          | Provides `seed`, `export`, `checkout`, `append`, `return`, `apply`, `seal` and `show` commands. The same binary represents A or B depending on the database path. |
| `survey_unit_tests`        | Tests domain rules, JSON validation and exchange use cases with a mocked repository. It does not link SQLite.                                                     |
| `survey_integration_tests` | Tests the real SQLite repository and exchange workflow, including migration, stale writes and rollback after an injected SQL failure.                             |

CTest also runs a script that launches separate CLI processes to exercise a successful exchange and rejection of a stale return.

## Build and run

Requirements: CMake 3.20 or newer, a C compiler and a C++20-capable compiler. Dependencies are vendored; CMake does not download them. The included SQLite source archive is unpacked automatically during configuration.

### GCC or Clang

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/survey_demo example.db
./build/survey_demo example.db
```

### Windows / Visual Studio 2022

Run from PowerShell with the Desktop development with C++ workload installed:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
.\build\Release\survey_demo.exe example.db
```

Use a separate build directory when switching compilers. See [Windows build notes](docs/WINDOWS_BUILD.md) for MSYS2 GCC and Clang instructions.

## Try an exchange

The following commands use the GCC/Clang build above and fresh database filenames:

```sh
./build/survey_exchange seed app-a.db
./build/survey_exchange export app-a.db store-A demo-batch checkout.json
./build/survey_exchange checkout app-b.db checkout.json
./build/survey_exchange append app-b.db demo-batch from-b 10 20 30
./build/survey_exchange return app-b.db checkout.json returned.json
./build/survey_exchange apply app-a.db store-A returned.json
./build/survey_exchange show app-a.db demo-batch
```

A now contains both observations at local revision 2. Applying the same return again produces a conflict, because its base revision is still 1.

On a Visual Studio build, use `.\build\Release\survey_exchange.exe` instead. The [exchange walkthrough](docs/ROUNDTRIP.md) includes a longer PowerShell example and terminology explanations.

## Tests, analysis and CI

Tests cover domain rules, malformed JSON, exact size limits, independent revision histories, persisted sealed-state protection, schema upgrades and transaction rollback.

Run one category or inspect the process-level test:

```sh
ctest --test-dir build -L unit --output-on-failure
ctest --test-dir build -L integration --output-on-failure
ctest --test-dir build -R survey.roundtrip.processes -V
```

For Visual Studio builds, add `-C Release` to these commands.

To enable clang-tidy, install clang-tidy and Ninja, then use a separate build:

```sh
cmake -S . -B build-tidy -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSURVEY_ENABLE_CLANG_TIDY=ON
cmake --build build-tidy --parallel 2
ctest --test-dir build-tidy --output-on-failure
```

The [GitHub Actions workflow](.github/workflows/ci.yml) defines Linux Release, Linux Debug with AddressSanitizer/UndefinedBehaviorSanitizer, and Windows Release jobs. Packaging depends on the entire build/test matrix and produces a Linux delivery archive. The workflow has not yet been run on GitHub.

Clang-format provides consistent formatting; clang-tidy performs static analysis and can be enabled locally using the commands above.

## What can be learned from the code

| Topic                         | Questions explored                                                                                                                             | Start here                                                                                         |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------- |
| Data modelling and DDD        | Which objects have identity? Which rules belong to the batch aggregate? Which responsibilities belong to application use cases or persistence? | [Domain model](include/survey/domain.hpp), [design notes](docs/DESIGN.md)                          |
| Data structures and ownership | Why keep ordered storage and an ID index? What survives vector growth? What is borrowed versus owned?                                          | [Domain implementation](src/domain.cpp)                                                            |
| Persistence and concurrency   | What must be saved together? How do transactions, revision checks and schema migrations solve different problems?                              | [Repository interface](include/survey/repository.hpp), [SQLite adapter](src/sqlite_repository.cpp) |
| Data exchange                 | How are domain objects mapped to JSON? How are identity, compatibility, byte limits and stale returns handled?                                 | [Exchange implementation](src/exchange.cpp)                                                        |
| Modern C++ and testing        | How do strong ID types, RAII, value semantics, `std::span`, `std::optional` and a mockable interface support the design?                       | [Public headers](include/survey), [tests](tests)                                                   |

## Scope and next steps

This is a deliberately small study project. It stores a bounded batch as one unit and replaces its child rows during a save. It does not implement large point-cloud ingestion, networking or automatic conflict merging.

The current exchange policy permits appending observations and sealing a batch. It rejects changes to existing observations, their order, the device or the coordinate frame. IDs and origin labels are supplied by callers; the original checkout file must remain available when creating a return.

Next steps include persisting checkout metadata and a stable store identity, generating UUID-based batch IDs, and implementing an in-memory repository with shared behavioral contract tests.

Dependency versions, sources and licenses are recorded in [third_party](third_party/README.md).
