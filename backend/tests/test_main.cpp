#include <gtest/gtest.h>

#include "bourse/core/logger.hpp"

namespace {

/// Silences the server's own logging for the duration of the suite.
///
/// Several tests start real servers and event loops, each of which logs at
/// INFO. Left on, that buries the actual assertion output. Registered as a
/// gtest global environment so it applies to every test without each fixture
/// having to remember.
class QuietLoggingEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    if (std::getenv("BOURSE_TEST_LOGS") == nullptr) {
      bourse::Logger::instance().setLevel(bourse::LogLevel::kOff);
    }
  }
  void TearDown() override { bourse::Logger::instance().flush(); }
};

// Registered during static initialisation, which gtest explicitly supports and
// which runs before gtest_main's RUN_ALL_TESTS.
const ::testing::Environment* const kQuietLogging =
    ::testing::AddGlobalTestEnvironment(new QuietLoggingEnvironment);

}  // namespace
