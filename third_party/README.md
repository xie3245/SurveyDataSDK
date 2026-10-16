# Upstream dependency sources

## Catch2

Version 3.8.1, official amalgamated C++ source/header, distributed under the Boost Software License 1.0. See `catch2/LICENSE.txt`. Upstream: https://github.com/catchorg/Catch2

## SQLite

Version and original download URL are recorded in `sources.json`. The amalgamation archive was downloaded from the official SQLite site and checked against its published SHA3-256 checksum. `sqlite3.c` and `sqlite3.h` contain their upstream notices. SQLite's core source is in the public domain: https://www.sqlite.org/copyright.html

The compact project ZIP bundles those two unchanged source files in `sqlite3/sqlite-source.tar.xz`. CMake verifies the local archive's SHA-256 and unpacks it into the build directory without network access. The upstream download hashes in `sources.json` describe the original upstream ZIP, not this locally repacked archive.

## JSON for Modern C++

nlohmann/json 3.12.0, official single header, distributed under the MIT License. See `json/LICENSE.MIT`; upstream: https://github.com/nlohmann/json

These files are included to make the example build without installing a SQLite development package or fetching libraries during CMake configuration. They are not authored as part of the example's domain implementation.
