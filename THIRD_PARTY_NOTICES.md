# Third-party notices

This project is distributed under the MIT License. It builds against and links
the following third-party components. Their licenses remain in effect; this
file is informational and does not replace the license text shipped by each
component.

| Component | Used for | License |
| --- | --- | --- |
| Boost (URL, Asio, Beast) | HTTP server, URL parsing | Boost Software License 1.0 |
| OpenSSL | Cryptographic primitives required by Boost | Apache License 2.0 |
| libpq / PostgreSQL | Database client library | PostgreSQL License |
| redis-plus-plus | Redis client | Apache License 2.0 |
| hiredis | Redis protocol client used by redis-plus-plus | BSD 3-Clause |
| nlohmann/json | JSON serialization | MIT |
| Catch2 3 | Test framework | Boost Software License 1.0 |

Development and runtime services:

| Component | Used for | License |
| --- | --- | --- |
| CMake | Build system | BSD 3-Clause |
| PostgreSQL server | Runtime database | PostgreSQL License |
| Redis server | Runtime cache and rate limiter | Version-dependent; check the license of the server version you deploy |
| oha | Development load generator, not linked or shipped | MIT |

The Docker Compose file pins service images for local development. Review the
license of each pinned image tag before redistributing a bundled deployment.
