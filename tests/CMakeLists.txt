# GoogleTest is resolved from the system first (Ubuntu ships a compiled
# libgtest-dev since 22.04) and only downloaded when it is genuinely missing.
# That keeps an offline build working and keeps CI fast.
include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
  FIND_PACKAGE_ARGS NAMES GTest
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

if(TARGET GTest::gtest_main)
  set(BOURSE_GTEST_TARGET GTest::gtest_main)
else()
  set(BOURSE_GTEST_TARGET gtest_main)
endif()

add_executable(bourse_tests
  test_main.cpp
  test_core.cpp
  test_cache.cpp
  test_resp.cpp
  test_exec.cpp
  test_match.cpp
  test_sql.cpp
  test_storage.cpp
  test_btree.cpp
  test_http.cpp
  test_net.cpp
)

target_link_libraries(bourse_tests PRIVATE bourse_core ${BOURSE_GTEST_TARGET})
bourse_set_warnings(bourse_tests)
bourse_enable_sanitizers(bourse_tests)

include(GoogleTest)
gtest_discover_tests(bourse_tests
  PROPERTIES TIMEOUT 120
  DISCOVERY_TIMEOUT 60
)
