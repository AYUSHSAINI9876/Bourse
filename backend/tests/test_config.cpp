// Command-line and environment parsing for the server.
//
// The environment half exists so one container image can be deployed to any
// platform that assigns its port at run time. That makes it the kind of code
// whose failure mode is a server that starts, logs "listening", and is never
// reached -- worth pinning down here rather than discovering in a deploy log.

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "bourse/server/bourse_server.hpp"

namespace bourse::server {
namespace {

/// fromArgs takes `char**`, so the literals have to be copied somewhere
/// mutable. The vector owns the strings; the pointer array borrows from it.
Result<Config> parse(std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  return Config::fromArgs(static_cast<int>(args.size()), argv.data());
}

class ConfigEnvironmentTest : public ::testing::Test {
 protected:
  // Every test starts from a clean environment and leaves one behind. Without
  // this a leaked PORT would silently reconfigure every later test in the
  // binary, and the failure would land somewhere unrelated.
  void SetUp() override { clear(); }

  void TearDown() override { clear(); }

  static void clear() {
    ::unsetenv("PORT");
    ::unsetenv("BOURSE_HOST");
    ::unsetenv("BOURSE_AUTH");
    ::unsetenv("BOURSE_ADMIN_USER");
    ::unsetenv("BOURSE_ADMIN_PASSWORD");
  }

  static void set(const char* name, const char* value) { ASSERT_EQ(::setenv(name, value, 1), 0); }
};

TEST_F(ConfigEnvironmentTest, DefaultsApplyWhenTheEnvironmentIsEmpty) {
  const Result<Config> config = parse({"bourse-server"});
  ASSERT_TRUE(config.ok()) << config.status().toString();
  EXPECT_EQ(config.value().http_port, 8080);
  EXPECT_EQ(config.value().resp_port, 6380);
  EXPECT_EQ(config.value().host, "0.0.0.0");
}

TEST_F(ConfigEnvironmentTest, PortEnvironmentVariableSetsTheHttpPort) {
  set("PORT", "10000");
  const Result<Config> config = parse({"bourse-server"});
  ASSERT_TRUE(config.ok()) << config.status().toString();
  EXPECT_EQ(config.value().http_port, 10000);
  // $PORT is the platform's public HTTP port and must not disturb RESP.
  EXPECT_EQ(config.value().resp_port, 6380);
}

TEST_F(ConfigEnvironmentTest, ExplicitFlagOverridesTheEnvironment) {
  set("PORT", "10000");
  const Result<Config> config = parse({"bourse-server", "--http-port", "9999"});
  ASSERT_TRUE(config.ok()) << config.status().toString();
  EXPECT_EQ(config.value().http_port, 9999);
}

TEST_F(ConfigEnvironmentTest, BourseHostEnvironmentVariableSetsTheBindAddress) {
  set("BOURSE_HOST", "127.0.0.1");
  const Result<Config> config = parse({"bourse-server"});
  ASSERT_TRUE(config.ok()) << config.status().toString();
  EXPECT_EQ(config.value().host, "127.0.0.1");
}

TEST_F(ConfigEnvironmentTest, EmptyEnvironmentVariablesAreIgnored) {
  // Deployment dashboards happily store a variable with a blank value; that
  // has to mean "unset" rather than "bind to nothing".
  set("PORT", "");
  set("BOURSE_HOST", "");
  const Result<Config> config = parse({"bourse-server"});
  ASSERT_TRUE(config.ok()) << config.status().toString();
  EXPECT_EQ(config.value().http_port, 8080);
  EXPECT_EQ(config.value().host, "0.0.0.0");
}

TEST_F(ConfigEnvironmentTest, MalformedPortIsAnErrorRatherThanASilentFallback) {
  // Falling back to 8080 here would start a server the platform never routes
  // to: healthy-looking logs, every request timing out.
  for (const char* bad : {"abc", "0", "65536", "80x", "-1"}) {
    set("PORT", bad);
    const Result<Config> config = parse({"bourse-server"});
    EXPECT_FALSE(config.ok()) << "accepted PORT=" << bad;
    EXPECT_NE(config.status().message().find("PORT"), std::string::npos);
  }
}

TEST_F(ConfigEnvironmentTest, AuthCredentialsComeFromTheEnvironment) {
  // argv is visible to every user on the box via `ps`, so credentials are read
  // from the environment in preference to flags.
  set("BOURSE_ADMIN_USER", "root");
  set("BOURSE_ADMIN_PASSWORD", "a-strong-password");
  const Result<Config> config = parse({"bourse-server"});
  ASSERT_TRUE(config.ok()) << config.status().toString();
  EXPECT_EQ(config.value().admin_user, "root");
  EXPECT_EQ(config.value().admin_password, "a-strong-password");
  // Supplying a password without asking for auth is unambiguous intent; a
  // server that silently ignored it would be enforcing nothing.
  EXPECT_TRUE(config.value().auth_enabled);
}

TEST_F(ConfigEnvironmentTest, NoAdministratorIsSeededByDefault) {
  // The deployment ships with no accounts at all: whoever registers first
  // becomes the administrator. A default admin_user here would mean a server
  // that always creates a named account, which is the shape that forced a
  // password to be published in the first place.
  const Result<Config> config = parse({"bourse-server"});
  ASSERT_TRUE(config.ok()) << config.status().toString();
  EXPECT_TRUE(config.value().admin_user.empty());
  EXPECT_TRUE(config.value().admin_password.empty());
  EXPECT_FALSE(config.value().auth_enabled);
}

TEST_F(ConfigEnvironmentTest, AdminFlagsOverrideTheEnvironment) {
  set("BOURSE_ADMIN_USER", "from-env");
  const Result<Config> config = parse({"bourse-server", "--admin-user", "from-argv"});
  ASSERT_TRUE(config.ok()) << config.status().toString();
  EXPECT_EQ(config.value().admin_user, "from-argv");
}

TEST(ConfigUsageTest, UsageDocumentsTheEnvironmentVariables) {
  // The usage text is the only place a deployer looks before reading source.
  const std::string usage = Config::usage();
  EXPECT_NE(usage.find("PORT"), std::string::npos);
  EXPECT_NE(usage.find("BOURSE_HOST"), std::string::npos);
  EXPECT_NE(usage.find("BOURSE_ADMIN_PASSWORD"), std::string::npos);
}

}  // namespace
}  // namespace bourse::server
