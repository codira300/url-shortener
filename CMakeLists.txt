cmake_minimum_required(VERSION 3.25)

project(urlshortener VERSION 1.0.0 LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(URLSHORT_BUILD_TESTS "Build unit, integration and concurrency tests" ON)
option(URLSHORT_ENABLE_INTEGRATION_TESTS "Register tests that need PostgreSQL and Redis with CTest" OFF)
option(URLSHORT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(URLSHORT_ENABLE_SANITIZERS "Enable ASan/UBSan (Linux/Clang/GCC only)" OFF)
set(URLSHORT_SANITIZER "" CACHE STRING "Sanitizer to enable: address, thread, undefined, or a comma separated list")

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
  if(URLSHORT_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

if(URLSHORT_WARNINGS_AS_ERRORS AND MSVC)
  add_compile_options(/WX)
endif()

if(URLSHORT_ENABLE_SANITIZERS OR URLSHORT_SANITIZER)
  if(MSVC)
    message(FATAL_ERROR "Sanitizers are not supported by this CMake configuration on MSVC")
  endif()
  set(_san "${URLSHORT_SANITIZER}")
  if(NOT _san)
    set(_san "address,undefined")
  endif()
  add_compile_options(-fno-omit-frame-pointer -fsanitize=${_san})
  add_link_options(-fsanitize=${_san})
  message(STATUS "Sanitizers enabled: ${_san}")
endif()

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-Wa,-mbig-obj)
endif()

find_package(Boost 1.83 REQUIRED COMPONENTS url)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)
pkg_check_modules(REDISPP QUIET IMPORTED_TARGET redis++)

if(REDISPP_FOUND)
  set(URLSHORT_REDIS_TARGET PkgConfig::REDISPP)
else()
  find_package(redis++ CONFIG QUIET)
  if(TARGET redis++::redis++)
    set(URLSHORT_REDIS_TARGET redis++::redis++)
  else()
    find_path(REDISPP_INCLUDE_DIR sw/redis++/redis++.h REQUIRED)
    find_library(REDISPP_LIBRARY NAMES redis++ REQUIRED)
    set(URLSHORT_REDIS_TARGET ${REDISPP_LIBRARY})
    set(URLSHORT_REDIS_INCLUDE_DIR ${REDISPP_INCLUDE_DIR})
  endif()
endif()

find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

add_library(urlshort_core STATIC
  src/config/Config.cpp
  src/util/Time.cpp
  src/util/Logger.cpp
  src/util/ThreadPool.cpp
  src/util/SingleFlight.cpp
  src/domain/Base62.cpp
  src/domain/ShortCode.cpp
  src/domain/ShortCodeGenerator.cpp
  src/domain/UrlValidator.cpp
  src/domain/UrlRecord.cpp
  src/observability/Metrics.cpp
  src/observability/ProcessMetrics.cpp
  src/observability/HealthService.cpp
  src/net/ClientIdentity.cpp
  src/db/PgConnection.cpp
  src/db/PgPool.cpp
  src/db/PostgresUrlRepository.cpp
  src/cache/RedisGateway.cpp
  src/cache/CacheCodec.cpp
  src/cache/RedisUrlCache.cpp
  src/ratelimit/RedisRateLimiter.cpp
  src/app/UrlService.cpp
  src/http/Router.cpp
  src/http/HttpServer.cpp
  src/app/Application.cpp
)

target_include_directories(urlshort_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${URLSHORT_REDIS_INCLUDE_DIR}
)

target_compile_definitions(urlshort_core
  PUBLIC
    $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:BOOST_ASIO_NO_DEPRECATED>
)

target_link_libraries(urlshort_core
  PUBLIC
    Boost::url
    OpenSSL::Crypto
    PkgConfig::LIBPQ
    ${URLSHORT_REDIS_TARGET}
    Threads::Threads
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Windows>:mswsock>
    $<$<PLATFORM_ID:Windows>:psapi>
)

add_executable(urlshortener src/main.cpp)
target_link_libraries(urlshortener PRIVATE urlshort_core)

if(URLSHORT_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)

  add_executable(urlshort_unit_tests
    tests/unit/test_base62.cpp
    tests/unit/test_short_code.cpp
    tests/unit/test_url_validator.cpp
    tests/unit/test_time.cpp
    tests/unit/test_config.cpp
    tests/unit/test_cache_serialization.cpp
    tests/unit/test_metrics.cpp
    tests/unit/test_client_identity.cpp
    tests/unit/test_router.cpp
    tests/unit/test_url_service.cpp
    tests/unit/test_thread_pool.cpp
    tests/unit/test_failure_modes.cpp
  )
  target_link_libraries(urlshort_unit_tests PRIVATE urlshort_core Catch2::Catch2WithMain)
  target_include_directories(urlshort_unit_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})

  add_test(NAME unit COMMAND urlshort_unit_tests)

  if(URLSHORT_ENABLE_INTEGRATION_TESTS)
    add_executable(urlshort_integration_tests
      tests/integration/test_integration.cpp
    )
    target_link_libraries(urlshort_integration_tests PRIVATE urlshort_core Catch2::Catch2WithMain)
    target_include_directories(urlshort_integration_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
    add_test(NAME integration COMMAND urlshort_integration_tests)
    set_tests_properties(integration PROPERTIES SKIP_RETURN_CODE 4)

    add_executable(urlshort_concurrency_tests
      tests/concurrency/test_concurrency.cpp
    )
    target_link_libraries(urlshort_concurrency_tests PRIVATE urlshort_core Catch2::Catch2WithMain)
    target_include_directories(urlshort_concurrency_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
    add_test(NAME concurrency COMMAND urlshort_concurrency_tests)
    set_tests_properties(concurrency PROPERTIES SKIP_RETURN_CODE 4)
  endif()
endif()

install(TARGETS urlshortener RUNTIME DESTINATION bin)
