// Crypto primitives, the auth service, and the permission check.
//
// The crypto half is tested against the vectors published with each standard.
// That is the entire justification for implementing them here rather than
// linking a library: correctness is decidable against a reference, so an
// implementation that agrees on the published vectors is either right or
// visibly wrong. scripts/verify-crypto.sh goes further and cross-checks
// PBKDF2 against Python's hashlib over randomised inputs.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "bourse/auth/auth_service.hpp"
#include "bourse/auth/crypto.hpp"
#include "bourse/auth/totp.hpp"
#include "bourse/cache/keyspace.hpp"
#include "bourse/core/file.hpp"
#include "bourse/exec/command.hpp"
#include "bourse/net/http_codec.hpp"

namespace bourse::auth {
namespace {

std::string hexOf(std::string_view text) {
  return toHex(Sha256::hash(text));
}

std::string repeated(char byte, std::size_t count) {
  return std::string(count, byte);
}

// ---------------------------------------------------------------------------
// SHA-256 -- FIPS 180-4
// ---------------------------------------------------------------------------

TEST(Sha256Test, MatchesTheFipsVectors) {
  EXPECT_EQ(hexOf(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(hexOf("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256Test, HandlesTheLengthPaddingBoundaries) {
  // A message ending at 55, 56 or 64 bytes exercises each padding branch: room
  // for the length, no room so an extra block is needed, and exactly full.
  // Getting this wrong produces a hash that is right for most inputs, which is
  // the worst possible failure mode.
  for (const std::size_t length : {size_t{54}, size_t{55}, size_t{56}, size_t{63}, size_t{64}, size_t{65},
                                   size_t{119}, size_t{120}, size_t{128}}) {
    const std::string message = repeated('a', length);
    Sha256 streaming;
    streaming.update(message);
    EXPECT_EQ(toHex(streaming.finish()), toHex(Sha256::hash(message))) << "length " << length;
  }
}

TEST(Sha256Test, StreamingInAnyChunkingMatchesOneShot) {
  const std::string message = repeated('x', 1000) + "tail";
  const std::string expected = toHex(Sha256::hash(message));

  for (const std::size_t chunk : {size_t{1}, size_t{7}, size_t{63}, size_t{64}, size_t{65}, size_t{999}}) {
    Sha256 sha;
    for (std::size_t offset = 0; offset < message.size(); offset += chunk) {
      sha.update(message.data() + offset, std::min(chunk, message.size() - offset));
    }
    EXPECT_EQ(toHex(sha.finish()), expected) << "chunk size " << chunk;
  }
}

// ---------------------------------------------------------------------------
// HMAC-SHA256 -- RFC 4231
// ---------------------------------------------------------------------------

TEST(HmacSha256Test, MatchesRfc4231Vectors) {
  EXPECT_EQ(toHex(hmacSha256(repeated('\x0b', 20), "Hi There")),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

  EXPECT_EQ(toHex(hmacSha256("Jefe", "what do ya want for nothing?")),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

  // Test case 6: a key longer than the 64-byte block, which must be hashed
  // down first. Skipping that reduction is a classic HMAC bug.
  EXPECT_EQ(
      toHex(hmacSha256(repeated('\xaa', 131), "Test Using Larger Than Block-Size Key - Hash Key First")),
      "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256 -- RFC 8018
// ---------------------------------------------------------------------------

TEST(Pbkdf2Test, MatchesPublishedVectors) {
  const auto derive = [](std::uint32_t iterations, std::size_t length) {
    const std::vector<std::uint8_t> out = pbkdf2HmacSha256("password", "salt", iterations, length);
    return toHex(out.data(), out.size());
  };
  EXPECT_EQ(derive(1, 32), "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
  EXPECT_EQ(derive(2, 32), "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
  EXPECT_EQ(derive(4096, 32), "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
}

TEST(Pbkdf2Test, DerivesLongerKeysAcrossMultipleBlocks) {
  // 40 bytes needs two SHA-256 blocks, exercising the block-index counter that
  // a single-block implementation never touches.
  const std::vector<std::uint8_t> out =
      pbkdf2HmacSha256("passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 40);
  EXPECT_EQ(out.size(), 40u);
  EXPECT_EQ(toHex(out.data(), out.size()),
            "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9");
}

TEST(Pbkdf2Test, IterationCountChangesTheOutput) {
  const std::vector<std::uint8_t> once = pbkdf2HmacSha256("pw", "salt", 1, 32);
  const std::vector<std::uint8_t> twice = pbkdf2HmacSha256("pw", "salt", 2, 32);
  EXPECT_NE(once, twice);
}

// ---------------------------------------------------------------------------
// Encoding and comparison
// ---------------------------------------------------------------------------

TEST(CryptoUtilTest, ConstantTimeEqualsAgreesWithOrdinaryComparison) {
  EXPECT_TRUE(constantTimeEquals("", ""));
  EXPECT_TRUE(constantTimeEquals("abc", "abc"));
  EXPECT_FALSE(constantTimeEquals("abc", "abd"));
  EXPECT_FALSE(constantTimeEquals("abc", "abcd"));
  // Differing in the first byte and the last byte must both be rejected --
  // an early-exit implementation passes this too, so it is a floor, not proof.
  EXPECT_FALSE(constantTimeEquals("xbc", "abc"));
  EXPECT_FALSE(constantTimeEquals("abx", "abc"));
}

TEST(CryptoUtilTest, HexRoundTrips) {
  const std::string raw("\x00\x01\xfe\xff", 4);
  const std::string hex = toHex(raw.data(), raw.size());
  EXPECT_EQ(hex, "0001feff");
  const Result<std::string> back = fromHex(hex);
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back.value(), raw);
}

TEST(CryptoUtilTest, RejectsMalformedHex) {
  EXPECT_FALSE(fromHex("abc").ok());  // odd length
  EXPECT_FALSE(fromHex("zz").ok());   // not hex
  EXPECT_TRUE(fromHex("").ok());
}

TEST(CryptoUtilTest, RandomBytesAreTheRequestedLengthAndNotRepeating) {
  Result<std::string> first = randomBytes(32);
  Result<std::string> second = randomBytes(32);
  ASSERT_TRUE(first.ok()) << first.status().toString();
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(first.value().size(), 32u);
  // Not a randomness test -- it is a wiring test. A CSPRNG that returns the
  // same buffer twice, or all zeros, is broken in a way this does catch.
  EXPECT_NE(first.value(), second.value());
  EXPECT_NE(first.value(), std::string(32, '\0'));
}

// ---------------------------------------------------------------------------
// Password hashing
// ---------------------------------------------------------------------------

TEST(PasswordHashTest, VerifiesTheCorrectPasswordAndRejectsOthers) {
  const Result<std::string> encoded = hashPassword("correct horse battery", 1000);
  ASSERT_TRUE(encoded.ok());

  const Result<bool> good = verifyPassword("correct horse battery", encoded.value());
  ASSERT_TRUE(good.ok());
  EXPECT_TRUE(good.value());

  const Result<bool> bad = verifyPassword("correct horse batterz", encoded.value());
  ASSERT_TRUE(bad.ok());
  EXPECT_FALSE(bad.value());
}

TEST(PasswordHashTest, SaltsSoIdenticalPasswordsHashDifferently) {
  const Result<std::string> a = hashPassword("same-password", 1000);
  const Result<std::string> b = hashPassword("same-password", 1000);
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  // Without a per-hash salt, one rainbow table breaks every account at once.
  EXPECT_NE(a.value(), b.value());
  EXPECT_TRUE(verifyPassword("same-password", a.value()).value());
  EXPECT_TRUE(verifyPassword("same-password", b.value()).value());
}

TEST(PasswordHashTest, EncodingCarriesItsOwnParameters) {
  const Result<std::string> encoded = hashPassword("pw", 4321);
  ASSERT_TRUE(encoded.ok());
  EXPECT_EQ(encoded.value().rfind("pbkdf2_sha256$4321$", 0), 0u);
  // The cost factor travelling with the hash is what lets it be raised later
  // without invalidating every existing credential.
  EXPECT_TRUE(verifyPassword("pw", encoded.value()).value());
}

TEST(PasswordHashTest, MalformedHashIsAnErrorNotAFalse) {
  // "Wrong password" and "corrupt user store" need different responses; a
  // bare false would bury the second under a stream of failed logins.
  for (const char* broken : {"", "notahash", "pbkdf2_sha256$0$aa$bb", "pbkdf2_sha256$1000$zz$bb",
                             "argon2$1000$aa$bb", "pbkdf2_sha256$1000$aa"}) {
    EXPECT_FALSE(verifyPassword("pw", broken).ok()) << "accepted: " << broken;
  }
}

TEST(PasswordHashTest, NeedsRehashTracksTheCostFactor) {
  const Result<std::string> weak = hashPassword("pw", 1000);
  ASSERT_TRUE(weak.ok());
  EXPECT_TRUE(needsRehash(weak.value(), 210000));
  EXPECT_FALSE(needsRehash(weak.value(), 1000));
  EXPECT_TRUE(needsRehash("garbage", 1000));
}

// ---------------------------------------------------------------------------
// AuthService
// ---------------------------------------------------------------------------

AuthConfig testConfig() {
  AuthConfig config;
  config.enabled = true;
  // 1000 rather than the production 210000: these tests hash dozens of times
  // and the work factor is not what they are checking.
  config.iterations = 1000;
  config.session_ttl_ms = 60 * 1000;
  return config;
}

// ---------------------------------------------------------------------------
// Self-service registration
// ---------------------------------------------------------------------------

TEST(RegistrationTest, TheFirstAccountIsAnAdministratorAndTheRestAreTraders) {
  AuthService auth(testConfig());

  // Somebody has to be able to administer a fresh deployment. Doing it this
  // way is what lets the server ship with no seeded account and no published
  // password -- the state that previously locked everyone out of the demo.
  const Result<Role> first = auth.registerUser("alice", "alice-password-1");
  ASSERT_TRUE(first.ok()) << first.status().toString();
  EXPECT_EQ(first.value(), Role::kAdmin);

  const Result<Role> second = auth.registerUser("bob", "bob-password-12");
  ASSERT_TRUE(second.ok()) << second.status().toString();
  EXPECT_EQ(second.value(), Role::kTrader);

  const Result<Role> third = auth.registerUser("carol", "carol-password-1");
  ASSERT_TRUE(third.ok());
  EXPECT_EQ(third.value(), Role::kTrader);
  EXPECT_EQ(auth.userCount(), 3U);
}

TEST(RegistrationTest, RefusesATakenNameWithoutDisturbingTheExistingAccount) {
  AuthService auth(testConfig());
  ASSERT_TRUE(auth.registerUser("alice", "alice-password-1").ok());

  const Result<Role> again = auth.registerUser("alice", "a-different-password");
  ASSERT_FALSE(again.ok());
  EXPECT_EQ(again.status().code(), ErrorCode::kAlreadyExists);

  // The original password must still work: a failed signup that overwrote the
  // account would be a trivial takeover of any username.
  EXPECT_TRUE(auth.login("alice", "alice-password-1", "test").ok());
  EXPECT_FALSE(auth.login("alice", "a-different-password", "test").ok());
}

TEST(RegistrationTest, AppliesTheSameUsernameAndPasswordRulesAsAddUser) {
  AuthService auth(testConfig());
  EXPECT_FALSE(auth.registerUser("has space", "a-good-password").ok());
  EXPECT_FALSE(auth.registerUser("alice", "short").ok());
  EXPECT_FALSE(auth.registerUser("alice", "password").ok());
  EXPECT_EQ(auth.userCount(), 0U);

  // A rejected signup must not consume the "first account is admin" slot.
  const Result<Role> valid = auth.registerUser("alice", "alice-password-1");
  ASSERT_TRUE(valid.ok());
  EXPECT_EQ(valid.value(), Role::kAdmin);
}

TEST(RegistrationTest, ConcurrentSignupsProduceExactlyOneAdministrator) {
  // The role is chosen under the same lock as the insert. Choosing it from a
  // separate userCount() call would let two signups racing on a cold server
  // both observe an empty store and both come back administrators.
  AuthService auth(testConfig());
  constexpr int kThreads = 8;
  std::vector<std::thread> threads;
  std::atomic<int> admins{0};
  std::atomic<int> traders{0};
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&auth, &admins, &traders, i] {
      const Result<Role> role = auth.registerUser("user" + std::to_string(i), "a-good-password-1");
      if (!role.ok()) {
        return;
      }
      if (role.value() == Role::kAdmin) {
        ++admins;
      } else if (role.value() == Role::kTrader) {
        ++traders;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(admins.load(), 1);
  EXPECT_EQ(traders.load(), kThreads - 1);
}

// ---------------------------------------------------------------------------
// Durable accounts
// ---------------------------------------------------------------------------

/// A private directory per instance, removed on destruction. Accounts are
/// written to a real file here rather than a mock: the failure this guards
/// against is a restart losing every account, and only the round trip through
/// the filesystem actually demonstrates it does not.
class AccountStorePath {
 public:
  AccountStorePath() {
    // ctest runs each test in its own process, so a process-local counter read
    // 0 in every one of them and every test landed on the same relative path.
    // Under `ctest --parallel` two tests then shared a single store: one
    // registered "alice" into the other's file, so the second registration
    // failed on a taken name, and each constructor deleted the store the other
    // was still using.
    //
    // Creating the directory is itself the uniqueness check -- create_directory
    // reports false when the path already exists, so the winner of a race
    // between two processes is unambiguous, and there is no separate
    // "does it exist?" test that could be raced.
    std::random_device entropy;
    for (;;) {
      std::filesystem::path candidate =
          std::filesystem::temp_directory_path() /
          ("bourse-test-accounts-" + std::to_string(entropy()) + "-" + std::to_string(entropy()));
      std::error_code ec;
      if (std::filesystem::create_directory(candidate, ec) && !ec) {
        dir_ = std::move(candidate);
        break;
      }
    }
    path_ = (dir_ / "accounts.users").string();
  }

  ~AccountStorePath() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  AccountStorePath(const AccountStorePath&) = delete;
  AccountStorePath& operator=(const AccountStorePath&) = delete;

  [[nodiscard]] const std::string& get() const { return path_; }

 private:
  std::filesystem::path dir_;
  std::string path_;
};

TEST(AccountStoreTest, AccountsSurviveARestart) {
  const AccountStorePath store;
  {
    AuthService auth(testConfig());
    ASSERT_TRUE(auth.openStore(store.get()).ok());
    ASSERT_TRUE(auth.registerUser("alice", "alice-password-1").ok());
    ASSERT_TRUE(auth.registerUser("bob", "bob-password-12").ok());
  }

  // A brand new service, as after a process restart.
  AuthService reopened(testConfig());
  ASSERT_TRUE(reopened.openStore(store.get()).ok());
  EXPECT_EQ(reopened.userCount(), 2U);

  const Result<LoginResult> alice = reopened.login("alice", "alice-password-1", "test");
  ASSERT_TRUE(alice.ok()) << alice.status().toString();
  EXPECT_EQ(alice.value().role, Role::kAdmin);

  const Result<LoginResult> bob = reopened.login("bob", "bob-password-12", "test");
  ASSERT_TRUE(bob.ok());
  EXPECT_EQ(bob.value().role, Role::kTrader);

  // The wrong password must still be wrong after a reload -- proving the hash
  // round-tripped rather than the file simply being trusted.
  EXPECT_FALSE(reopened.login("alice", "bob-password-12", "test").ok());
}

TEST(AccountStoreTest, SessionsAreDeliberatelyNotPersisted) {
  const AccountStorePath store;
  std::string token;
  {
    AuthService auth(testConfig());
    ASSERT_TRUE(auth.openStore(store.get()).ok());
    ASSERT_TRUE(auth.registerUser("alice", "alice-password-1").ok());
    const Result<LoginResult> login = auth.login("alice", "alice-password-1", "test");
    ASSERT_TRUE(login.ok());
    token = login.value().token;
  }

  AuthService reopened(testConfig());
  ASSERT_TRUE(reopened.openStore(store.get()).ok());
  // Signing everyone out across a restart is the safe direction to fail, and a
  // token surviving in a file would be a credential at rest.
  EXPECT_FALSE(reopened.authenticate(token).ok());
  EXPECT_EQ(reopened.sessionCount(), 0U);
}

TEST(AccountStoreTest, RoleChangesAndDeletionsAreWrittenThrough) {
  const AccountStorePath store;
  {
    AuthService auth(testConfig());
    ASSERT_TRUE(auth.openStore(store.get()).ok());
    ASSERT_TRUE(auth.registerUser("alice", "alice-password-1").ok());
    ASSERT_TRUE(auth.registerUser("bob", "bob-password-12").ok());
    ASSERT_TRUE(auth.setRole("bob", Role::kViewer).ok());
    ASSERT_TRUE(auth.registerUser("carol", "carol-password-1").ok());
    ASSERT_TRUE(auth.removeUser("carol").ok());
  }

  AuthService reopened(testConfig());
  ASSERT_TRUE(reopened.openStore(store.get()).ok());
  EXPECT_EQ(reopened.userCount(), 2U);
  EXPECT_FALSE(reopened.hasUser("carol"));

  const Result<LoginResult> bob = reopened.login("bob", "bob-password-12", "test");
  ASSERT_TRUE(bob.ok());
  EXPECT_EQ(bob.value().role, Role::kViewer);
}

TEST(AccountStoreTest, ARewrittenPasswordIsTheOneThatReloads) {
  const AccountStorePath store;
  {
    AuthService auth(testConfig());
    ASSERT_TRUE(auth.openStore(store.get()).ok());
    ASSERT_TRUE(auth.registerUser("alice", "alice-password-1").ok());
    ASSERT_TRUE(auth.setPassword("alice", "a-brand-new-password").ok());
  }

  AuthService reopened(testConfig());
  ASSERT_TRUE(reopened.openStore(store.get()).ok());
  EXPECT_TRUE(reopened.login("alice", "a-brand-new-password", "test").ok());
  EXPECT_FALSE(reopened.login("alice", "alice-password-1", "test").ok());
}

TEST(AccountStoreTest, RefusesAFileItDoesNotUnderstand) {
  const AccountStorePath store;
  // Silently treating an unreadable store as "no accounts yet" would hand the
  // next person to register an administrator account on a populated server.
  ASSERT_TRUE(File::writeWholeFile(store.get(), "bourse-users 99\nu alice admin 0 0 hash\n").ok());
  AuthService auth(testConfig());
  const Status opened = auth.openStore(store.get());
  EXPECT_FALSE(opened.ok());
  EXPECT_EQ(opened.code(), ErrorCode::kCorruption);
}

TEST(AccountStoreTest, TwoFactorEnrolmentSurvivesARestart) {
  const AccountStorePath store;
  // A fixed clock, so the code generated here and the step recorded against it
  // are the same on both sides of the restart.
  static std::int64_t fake_now = 1'700'000'000'000;
  const auto clock = [] { return fake_now; };

  std::string secret;
  {
    AuthService auth(testConfig());
    auth.setClockForTesting(clock);
    ASSERT_TRUE(auth.openStore(store.get()).ok());
    ASSERT_TRUE(auth.registerUser("alice", "alice-password-1").ok());
    const Result<TotpEnrolment> begun = auth.beginTotpEnrolment("alice", "Bourse");
    ASSERT_TRUE(begun.ok()) << begun.status().toString();
    secret = begun.value().base32_secret;

    const Result<std::string> raw = base32Decode(secret);
    ASSERT_TRUE(raw.ok());
    const Result<std::string> code = totp(raw.value(), fake_now / 1000);
    ASSERT_TRUE(code.ok());
    ASSERT_TRUE(auth.confirmTotpEnrolment("alice", code.value()).ok());
  }

  AuthService reopened(testConfig());
  reopened.setClockForTesting(clock);
  ASSERT_TRUE(reopened.openStore(store.get()).ok());
  EXPECT_TRUE(reopened.totpEnabled("alice"));

  const Result<std::string> raw = base32Decode(secret);
  ASSERT_TRUE(raw.ok());

  // The password alone is not enough: 2FA is still enforced after the reload.
  EXPECT_FALSE(reopened.login("alice", "alice-password-1", "test").ok());

  // The code that confirmed enrolment is refused, because the step it was
  // accepted for was persisted alongside the secret. Without that, a restart
  // would reopen the replay window on any code an attacker had just observed.
  const Result<std::string> used = totp(raw.value(), fake_now / 1000);
  ASSERT_TRUE(used.ok());
  EXPECT_FALSE(reopened.login("alice", "alice-password-1", "test", used.value()).ok());

  // The next step's code works, which is only possible if the secret itself
  // round-tripped through the file rather than being regenerated.
  fake_now += kTotpPeriodSeconds * 1000;
  const Result<std::string> fresh = totp(raw.value(), fake_now / 1000);
  ASSERT_TRUE(fresh.ok());
  const Result<LoginResult> with_code = reopened.login("alice", "alice-password-1", "test", fresh.value());
  EXPECT_TRUE(with_code.ok()) << with_code.status().toString();
}

TEST(AuthServiceTest, LoginIssuesAUsableToken) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());

  Result<LoginResult> login = service.login("alice", "hunter2-long-enough", "10.0.0.1");
  ASSERT_TRUE(login.ok()) << login.status().toString();
  EXPECT_EQ(login.value().username, "alice");
  EXPECT_EQ(login.value().role, Role::kTrader);
  EXPECT_FALSE(login.value().token.empty());

  Result<Principal> principal = service.authenticate(login.value().token);
  ASSERT_TRUE(principal.ok());
  EXPECT_EQ(principal.value().username, "alice");
  EXPECT_EQ(principal.value().role, Role::kTrader);
}

TEST(AuthServiceTest, RejectsTheWrongPasswordAndUnknownUsersIdentically) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kViewer).ok());

  const Result<LoginResult> wrong_password = service.login("alice", "wrong-password", "a");
  const Result<LoginResult> no_such_user = service.login("mallory", "wrong-password", "b");
  ASSERT_FALSE(wrong_password.ok());
  ASSERT_FALSE(no_such_user.ok());
  // Identical wording on purpose: a distinguishable message enumerates the
  // user list for anyone who cares to ask.
  EXPECT_EQ(wrong_password.status().message(), no_such_user.status().message());
}

TEST(AuthServiceTest, TokensAreNotStoredInRecoverableForm) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kViewer).ok());
  Result<LoginResult> login = service.login("alice", "hunter2-long-enough", "a");
  ASSERT_TRUE(login.ok());

  // A truncated or extended token must not authenticate -- the lookup is by
  // hash, so a prefix cannot match.
  EXPECT_FALSE(service.authenticate(login.value().token.substr(0, 32)).ok());
  EXPECT_FALSE(service.authenticate(login.value().token + "0").ok());
  EXPECT_FALSE(service.authenticate("").ok());
}

TEST(AuthServiceTest, SessionsExpire) {
  static std::int64_t fake_now = 1'000'000;
  AuthConfig config = testConfig();
  config.session_ttl_ms = 5000;
  AuthService service(config);
  service.setClockForTesting([] { return fake_now; });

  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kViewer).ok());
  Result<LoginResult> login = service.login("alice", "hunter2-long-enough", "a");
  ASSERT_TRUE(login.ok());
  EXPECT_TRUE(service.authenticate(login.value().token).ok());

  fake_now += 4999;
  EXPECT_TRUE(service.authenticate(login.value().token).ok());

  fake_now += 2;
  EXPECT_FALSE(service.authenticate(login.value().token).ok());
  // Lazy expiry should have dropped it during the failed lookup.
  EXPECT_EQ(service.sessionCount(), 0u);
}

TEST(AuthServiceTest, SweepRemovesSessionsNobodyPresentsAgain) {
  static std::int64_t fake_now = 5'000'000;
  AuthConfig config = testConfig();
  config.session_ttl_ms = 1000;
  AuthService service(config);
  service.setClockForTesting([] { return fake_now; });

  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kViewer).ok());
  ASSERT_TRUE(service.login("alice", "hunter2-long-enough", "a").ok());
  ASSERT_TRUE(service.login("alice", "hunter2-long-enough", "a").ok());
  EXPECT_EQ(service.sessionCount(), 2u);

  fake_now += 1001;
  EXPECT_EQ(service.sweepExpired(fake_now), 2u);
  EXPECT_EQ(service.sessionCount(), 0u);
}

TEST(AuthServiceTest, LocksAnAccountOutAfterRepeatedFailures) {
  static std::int64_t fake_now = 9'000'000;
  AuthConfig config = testConfig();
  config.max_failures = 3;
  config.lockout_ms = 30'000;
  AuthService service(config);
  service.setClockForTesting([] { return fake_now; });
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kViewer).ok());

  for (int attempt = 0; attempt < 3; ++attempt) {
    EXPECT_FALSE(service.login("alice", "nope-nope-nope", "10.0.0.9").ok());
  }

  // The correct password must now also be refused, otherwise the lockout is
  // decorative: an attacker who guesses right on attempt four still wins.
  const Result<LoginResult> during = service.login("alice", "hunter2-long-enough", "10.0.0.9");
  ASSERT_FALSE(during.ok());
  EXPECT_NE(during.status().message().find("too many failed attempts"), std::string::npos);

  fake_now += 30'001;
  EXPECT_TRUE(service.login("alice", "hunter2-long-enough", "10.0.0.9").ok());
}

TEST(AuthServiceTest, ASuccessfulLoginClearsTheFailureCount) {
  AuthConfig config = testConfig();
  config.max_failures = 3;
  AuthService service(config);
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kViewer).ok());

  EXPECT_FALSE(service.login("alice", "wrong-wrong-wrong", "1.1.1.1").ok());
  EXPECT_FALSE(service.login("alice", "wrong-wrong-wrong", "1.1.1.1").ok());
  ASSERT_TRUE(service.login("alice", "hunter2-long-enough", "1.1.1.1").ok());
  // Two more failures must not trip the lockout: the counter reset.
  EXPECT_FALSE(service.login("alice", "wrong-wrong-wrong", "1.1.1.1").ok());
  EXPECT_FALSE(service.login("alice", "wrong-wrong-wrong", "1.1.1.1").ok());
  EXPECT_TRUE(service.login("alice", "hunter2-long-enough", "1.1.1.1").ok());
}

TEST(AuthServiceTest, SweepDropsStaleFailureRecords) {
  static std::int64_t fake_now = 3'000'000;
  AuthConfig config = testConfig();
  config.max_failures = 5;
  config.lockout_ms = 1000;
  AuthService service(config);
  service.setClockForTesting([] { return fake_now; });

  // One failed attempt against each of many usernames: below the lockout
  // threshold, so nothing here ever gets a lockout to expire. An attacker
  // picks how many of these exist, so they have to age out on their own.
  for (int i = 0; i < 50; ++i) {
    EXPECT_FALSE(service.login("ghost" + std::to_string(i), "whatever-password", "1.2.3.4").ok());
  }

  // Well inside the retention window: still held, so a slow attacker cannot
  // reset their own counter by pausing.
  fake_now += 1000;
  EXPECT_EQ(service.sweepExpired(fake_now), 0u);

  // Past it: reclaimed.
  fake_now += 10 * 60 * 1000;
  service.sweepExpired(fake_now);

  // Nothing observable exposes the table size, so this asserts the behaviour
  // that matters instead: a fresh attempt is not treated as a continuation.
  for (int attempt = 0; attempt < 4; ++attempt) {
    EXPECT_FALSE(service.login("ghost0", "whatever-password", "5.6.7.8").ok());
  }
  ASSERT_TRUE(service.addUser("ghost0", "a-real-password", Role::kViewer).ok());
  EXPECT_TRUE(service.login("ghost0", "a-real-password", "5.6.7.8").ok());
}

TEST(AuthServiceTest, ChangingAPasswordRevokesLiveSessions) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kViewer).ok());
  Result<LoginResult> login = service.login("alice", "hunter2-long-enough", "a");
  ASSERT_TRUE(login.ok());
  ASSERT_TRUE(service.authenticate(login.value().token).ok());

  ASSERT_TRUE(service.setPassword("alice", "a-brand-new-password").ok());
  // The usual reason to change a password is that someone else has the old
  // one. Leaving their session alive defeats the point.
  EXPECT_FALSE(service.authenticate(login.value().token).ok());
}

TEST(AuthServiceTest, DeletingAUserRevokesLiveSessions) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kViewer).ok());
  Result<LoginResult> login = service.login("alice", "hunter2-long-enough", "a");
  ASSERT_TRUE(login.ok());

  ASSERT_TRUE(service.removeUser("alice").ok());
  EXPECT_FALSE(service.authenticate(login.value().token).ok());
  EXPECT_FALSE(service.hasUser("alice"));
}

TEST(AuthServiceTest, DemotionTakesEffectOnLiveSessions) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kAdmin).ok());
  Result<LoginResult> login = service.login("alice", "hunter2-long-enough", "a");
  ASSERT_TRUE(login.ok());
  EXPECT_EQ(service.authenticate(login.value().token).value().role, Role::kAdmin);

  ASSERT_TRUE(service.setRole("alice", Role::kViewer).ok());
  // A session carries a copy of the role, so a demotion that only took effect
  // at next login would leave an ex-admin with admin rights for 12 hours.
  EXPECT_EQ(service.authenticate(login.value().token).value().role, Role::kViewer);
}

TEST(AuthServiceTest, LogoutInvalidatesOnlyThatSession) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kViewer).ok());
  Result<LoginResult> laptop = service.login("alice", "hunter2-long-enough", "a");
  Result<LoginResult> phone = service.login("alice", "hunter2-long-enough", "b");
  ASSERT_TRUE(laptop.ok());
  ASSERT_TRUE(phone.ok());

  EXPECT_TRUE(service.logout(laptop.value().token));
  EXPECT_FALSE(service.authenticate(laptop.value().token).ok());
  EXPECT_TRUE(service.authenticate(phone.value().token).ok());
}

TEST(AuthServiceTest, RejectsWeakAndMalformedCredentials) {
  AuthService service(testConfig());
  EXPECT_FALSE(service.addUser("alice", "short", Role::kViewer).ok());
  EXPECT_FALSE(service.addUser("alice", "password", Role::kViewer).ok());
  EXPECT_FALSE(service.addUser("", "hunter2-long-enough", Role::kViewer).ok());
  EXPECT_FALSE(service.addUser("has space", "hunter2-long-enough", Role::kViewer).ok());
  EXPECT_FALSE(service.addUser("alice", "hunter2-long-enough", Role::kAnonymous).ok());
  EXPECT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kViewer).ok());
  EXPECT_FALSE(service.addUser("alice", "another-good-password", Role::kViewer).ok());
}

TEST(AuthServiceTest, ListUsersNeverExposesPasswordHashes) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kAdmin).ok());
  const std::vector<UserRecord> users = service.listUsers();
  ASSERT_EQ(users.size(), 1u);
  EXPECT_EQ(users[0].username, "alice");
  // Stripped at the source rather than trusted not to be printed downstream.
  EXPECT_TRUE(users[0].password_hash.empty());
}

TEST(AuthServiceTest, ConcurrentLoginsAndLookupsAreSafe) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());

  constexpr int kThreads = 8;
  constexpr int kPerThread = 20;
  std::vector<std::thread> workers;
  std::atomic<int> succeeded{0};

  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&service, &succeeded] {
      for (int i = 0; i < kPerThread; ++i) {
        Result<LoginResult> login = service.login("alice", "hunter2-long-enough", "shared");
        if (login.ok() && service.authenticate(login.value().token).ok()) {
          succeeded.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  EXPECT_EQ(succeeded.load(), kThreads * kPerThread);
  EXPECT_EQ(service.sessionCount(), static_cast<std::size_t>(kThreads * kPerThread));
}

// ---------------------------------------------------------------------------
// Role ordering
// ---------------------------------------------------------------------------

TEST(RoleTest, OrdersFromLeastToMostPrivileged) {
  const Principal viewer{"v", Role::kViewer};
  const Principal trader{"t", Role::kTrader};
  const Principal admin{"a", Role::kAdmin};
  const Principal anonymous{};

  EXPECT_FALSE(anonymous.authenticated());
  EXPECT_TRUE(viewer.can(Role::kViewer));
  EXPECT_FALSE(viewer.can(Role::kTrader));
  EXPECT_TRUE(trader.can(Role::kViewer));
  EXPECT_TRUE(trader.can(Role::kTrader));
  EXPECT_FALSE(trader.can(Role::kAdmin));
  EXPECT_TRUE(admin.can(Role::kAdmin));
  EXPECT_FALSE(anonymous.can(Role::kViewer));
}

TEST(RoleTest, ParsesKnownNamesAndRejectsEverythingElse) {
  Role role{};
  EXPECT_TRUE(parseRole("viewer", role));
  EXPECT_EQ(role, Role::kViewer);
  EXPECT_TRUE(parseRole("admin", role));
  EXPECT_EQ(role, Role::kAdmin);
  // A typo in a config file must not silently grant or revoke anything.
  EXPECT_FALSE(parseRole("Admin", role));
  EXPECT_FALSE(parseRole("root", role));
  EXPECT_FALSE(parseRole("", role));
  EXPECT_FALSE(parseRole("anonymous", role));
}

// ---------------------------------------------------------------------------
// Two-factor enrolment lifecycle
// ---------------------------------------------------------------------------

// Drives a real enrolment and returns the base32 secret, so the tests below
// work against genuine codes rather than a stubbed verifier.
std::string enrolAndConfirm(AuthService& service, const std::string& user) {
  Result<TotpEnrolment> begun = service.beginTotpEnrolment(user, "Bourse");
  EXPECT_TRUE(begun.ok()) << begun.status().toString();
  const std::string secret = begun.value().base32_secret;

  const Result<std::string> raw = base32Decode(secret);
  EXPECT_TRUE(raw.ok());
  const Result<std::string> code = totp(raw.value(), std::time(nullptr));
  EXPECT_TRUE(code.ok());
  EXPECT_TRUE(service.confirmTotpEnrolment(user, code.value()).ok());
  return secret;
}

TEST(AuthServiceTotpTest, EnrolmentOnlyEnforcesAfterConfirmation) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());

  ASSERT_TRUE(service.beginTotpEnrolment("alice", "Bourse").ok());
  // Opening the setup screen must not lock the account: the secret is stored
  // but unconfirmed, so a password-only login still succeeds.
  EXPECT_FALSE(service.totpEnabled("alice"));
  EXPECT_TRUE(service.login("alice", "hunter2-long-enough", "10.0.0.1").ok());
}

TEST(AuthServiceTotpTest, RefusesToReEnrolWhileEnabled) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());
  enrolAndConfirm(service, "alice");
  ASSERT_TRUE(service.totpEnabled("alice"));

  // Beginning a fresh enrolment would replace the working secret and, because
  // login enforces on the enabled flag, switch 2FA off -- with no password
  // check. That would let one live session strip the second factor, which is
  // exactly what disableTotp()'s password check exists to prevent.
  const Result<TotpEnrolment> again = service.beginTotpEnrolment("alice", "Bourse");
  EXPECT_FALSE(again.ok());

  // The point of refusing is that the account is left exactly as it was.
  EXPECT_TRUE(service.totpEnabled("alice"));
  const Result<LoginResult> without_code = service.login("alice", "hunter2-long-enough", "10.0.0.1");
  EXPECT_FALSE(without_code.ok());
}

TEST(AuthServiceTotpTest, DisablingNeedsThePasswordAndThenAllowsReEnrolment) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());
  const std::string first = enrolAndConfirm(service, "alice");

  EXPECT_FALSE(service.disableTotp("alice", "not-the-password").ok());
  EXPECT_TRUE(service.totpEnabled("alice"));

  ASSERT_TRUE(service.disableTotp("alice", "hunter2-long-enough").ok());
  EXPECT_FALSE(service.totpEnabled("alice"));
  EXPECT_TRUE(service.login("alice", "hunter2-long-enough", "10.0.0.1").ok());

  // A new device gets a new secret rather than the retired one.
  const std::string second = enrolAndConfirm(service, "alice");
  EXPECT_NE(first, second);
}

TEST(AuthServiceTotpTest, MissingCodeIsDistinctFromAWrongOne) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());
  enrolAndConfirm(service, "alice");

  // The dashboard has to tell "now ask for a code" apart from "those
  // credentials are wrong", or it cannot show the second-factor prompt.
  const Result<LoginResult> missing = service.login("alice", "hunter2-long-enough", "10.0.0.1", "");
  ASSERT_FALSE(missing.ok());
  EXPECT_NE(missing.status().message().find("two-factor"), std::string::npos) << missing.status().toString();

  const Result<LoginResult> wrong = service.login("alice", "hunter2-long-enough", "10.0.0.1", "000000");
  EXPECT_FALSE(wrong.ok());
}

TEST(AuthServiceTotpTest, DoesNotLeakTheSecretThroughUserListings) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());
  const std::string secret = enrolAndConfirm(service, "alice");

  bool seen = false;
  for (const UserRecord& record : service.listUsers()) {
    // listUsers() promises to leave the hash out; the shared secret is kept in
    // a side table for the same reason, so there is no field here to leak it
    // through. What a listing may say is *that* 2FA is on, never the seed.
    EXPECT_TRUE(record.password_hash.empty()) << record.username;
    if (record.username == "alice") {
      EXPECT_TRUE(record.totp_enabled);
      seen = true;
    }
  }
  EXPECT_TRUE(seen);

  // Kept out of the listing but not lost: enrolment is confirmed, so the
  // service is still holding the secret and enforcing on it.
  EXPECT_TRUE(service.totpEnabled("alice"));
  EXPECT_FALSE(secret.empty());
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------

TEST(AuthServiceSessionTest, RevokingOthersKeepsTheCallersSession) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());
  ASSERT_TRUE(service.addUser("bob", "bobs-long-password", Role::kTrader).ok());

  const std::string keep = service.login("alice", "hunter2-long-enough", "10.0.0.1").value().token;
  const std::string drop = service.login("alice", "hunter2-long-enough", "10.0.0.2").value().token;
  const std::string other = service.login("bob", "bobs-long-password", "10.0.0.3").value().token;

  EXPECT_EQ(service.revokeOtherSessions("alice", keep), 1U);
  EXPECT_TRUE(service.authenticate(keep).ok());
  EXPECT_FALSE(service.authenticate(drop).ok());
  // Signing out everywhere means everywhere *you* are, not everyone.
  EXPECT_TRUE(service.authenticate(other).ok());
}

TEST(AuthServiceSessionTest, ListingsCarryAnIdThatIsNotTheToken) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());
  const std::string token = service.login("alice", "hunter2-long-enough", "10.0.0.1").value().token;

  const std::vector<SessionInfo> sessions = service.listSessions("alice", token);
  ASSERT_EQ(sessions.size(), 1U);
  EXPECT_EQ(sessions.front().username, "alice");
  EXPECT_TRUE(sessions.front().current);
  EXPECT_FALSE(sessions.front().id.empty());
  // A listing that echoed the bearer token would hand every viewer of the
  // security panel a working credential for each of its rows.
  EXPECT_NE(sessions.front().id, token);
  EXPECT_EQ(token.find(sessions.front().id), std::string::npos);
}

TEST(AuthServiceSessionTest, RevokingByIdEndsThatSessionOnly) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());
  const std::string keep = service.login("alice", "hunter2-long-enough", "10.0.0.1").value().token;
  const std::string drop = service.login("alice", "hunter2-long-enough", "10.0.0.2").value().token;

  std::string drop_id;
  for (const SessionInfo& session : service.listSessions("alice", keep)) {
    if (!session.current) {
      drop_id = session.id;
    }
  }
  ASSERT_FALSE(drop_id.empty());

  EXPECT_TRUE(service.revokeSessionById("alice", drop_id));
  EXPECT_FALSE(service.authenticate(drop).ok());
  EXPECT_TRUE(service.authenticate(keep).ok());

  // A second revoke of the same id is a miss, not a silent success.
  EXPECT_FALSE(service.revokeSessionById("alice", drop_id));
}

TEST(AuthServiceSessionTest, OneUserCannotRevokeAnothersSession) {
  AuthService service(testConfig());
  ASSERT_TRUE(service.addUser("alice", "hunter2-long-enough", Role::kTrader).ok());
  ASSERT_TRUE(service.addUser("bob", "bobs-long-password", Role::kTrader).ok());

  const std::string alice = service.login("alice", "hunter2-long-enough", "10.0.0.1").value().token;
  const std::string bob = service.login("bob", "bobs-long-password", "10.0.0.2").value().token;

  std::string bob_id;
  for (const SessionInfo& session : service.listSessions("bob", bob)) {
    bob_id = session.id;
  }
  ASSERT_FALSE(bob_id.empty());

  // Session ids travel in a URL, so scoping the revoke to the caller is the
  // difference between a sign-out button and a way to log anyone out.
  EXPECT_FALSE(service.revokeSessionById("alice", bob_id));
  EXPECT_TRUE(service.authenticate(bob).ok());
  EXPECT_TRUE(service.authenticate(alice).ok());
}

}  // namespace
}  // namespace bourse::auth

// ---------------------------------------------------------------------------
// Enforcement in the command registry
// ---------------------------------------------------------------------------

namespace bourse::exec {
namespace {

class AuthEnforcementTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auth::AuthConfig config;
    config.enabled = true;
    config.iterations = 1000;
    service_ = std::make_unique<auth::AuthService>(config);
    ASSERT_TRUE(service_->addUser("viewer", "viewer-password", auth::Role::kViewer).ok());
    ASSERT_TRUE(service_->addUser("trader", "trader-password", auth::Role::kTrader).ok());
    ASSERT_TRUE(service_->addUser("admin", "admin-password1", auth::Role::kAdmin).ok());

    registry_ = CommandRegistry::createDefault();
    context_.keyspace = &keyspace_;
    context_.auth = service_.get();
  }

  Reply run(const std::vector<std::string>& argv, auth::Role role) {
    CommandContext command_context{context_, nullptr, auth::Principal{"someone", role}, "test"};
    return registry_->dispatch(command_context, argv);
  }

  Reply runAnonymous(const std::vector<std::string>& argv) {
    CommandContext command_context{context_, nullptr, auth::Principal{}, "test"};
    return registry_->dispatch(command_context, argv);
  }

  cache::Keyspace keyspace_;
  std::unique_ptr<auth::AuthService> service_;
  std::unique_ptr<CommandRegistry> registry_;
  ServerContext context_;
};

TEST_F(AuthEnforcementTest, AnonymousCallersReachOnlyThePreAuthVerbs) {
  EXPECT_FALSE(runAnonymous({"PING"}).isError());
  EXPECT_FALSE(runAnonymous({"QUIT"}).isError());

  const Reply denied = runAnonymous({"GET", "key"});
  ASSERT_TRUE(denied.isError());
  // NOAUTH tells a client to authenticate and retry; NOPERM tells it not to
  // bother. Collapsing them sends clients into login loops.
  EXPECT_EQ(denied.text().rfind("NOAUTH", 0), 0u);
}

TEST_F(AuthEnforcementTest, ViewersReadButDoNotWrite) {
  EXPECT_FALSE(run({"GET", "key"}, auth::Role::kViewer).isError());
  EXPECT_FALSE(run({"DBSIZE"}, auth::Role::kViewer).isError());

  const Reply denied = run({"SET", "key", "value"}, auth::Role::kViewer);
  ASSERT_TRUE(denied.isError());
  EXPECT_EQ(denied.text().rfind("NOPERM", 0), 0u);
}

TEST_F(AuthEnforcementTest, TradersWriteButDoNotAdminister) {
  EXPECT_FALSE(run({"SET", "key", "value"}, auth::Role::kTrader).isError());
  EXPECT_FALSE(run({"GET", "key"}, auth::Role::kTrader).isError());

  for (const std::vector<std::string>& forbidden :
       {std::vector<std::string>{"FLUSHALL"}, std::vector<std::string>{"USER", "LIST"}}) {
    const Reply denied = run(forbidden, auth::Role::kTrader);
    ASSERT_TRUE(denied.isError()) << forbidden[0];
    EXPECT_EQ(denied.text().rfind("NOPERM", 0), 0u) << forbidden[0];
  }
}

TEST_F(AuthEnforcementTest, AdminsReachEverything) {
  EXPECT_FALSE(run({"SET", "key", "value"}, auth::Role::kAdmin).isError());
  EXPECT_FALSE(run({"FLUSHALL"}, auth::Role::kAdmin).isError());
  EXPECT_FALSE(run({"USER", "LIST"}, auth::Role::kAdmin).isError());
}

TEST_F(AuthEnforcementTest, AuthPromotesTheContextPrincipal) {
  CommandContext command_context{context_, nullptr, auth::Principal{}, "test"};
  const Reply reply = registry_->dispatch(command_context, {"AUTH", "trader", "trader-password"});
  ASSERT_FALSE(reply.isError()) << reply.text();
  EXPECT_EQ(command_context.principal.username, "trader");
  EXPECT_EQ(command_context.principal.role, auth::Role::kTrader);
}

TEST_F(AuthEnforcementTest, AuthWithBadCredentialsLeavesThePrincipalUntouched) {
  CommandContext command_context{context_, nullptr, auth::Principal{}, "test"};
  const Reply reply = registry_->dispatch(command_context, {"AUTH", "trader", "wrong-password"});
  ASSERT_TRUE(reply.isError());
  EXPECT_EQ(reply.text().rfind("WRONGPASS", 0), 0u);
  EXPECT_FALSE(command_context.principal.authenticated());
}

TEST_F(AuthEnforcementTest, NothingIsEnforcedWhenAuthIsDisabled) {
  // The default deployment has no credentials, and every existing smoke test
  // depends on that path staying wide open.
  ServerContext open;
  open.keyspace = &keyspace_;
  open.auth = nullptr;
  CommandContext command_context{open, nullptr, auth::Principal{}, "test"};
  EXPECT_FALSE(registry_->dispatch(command_context, {"SET", "k", "v"}).isError());
  EXPECT_FALSE(registry_->dispatch(command_context, {"FLUSHALL"}).isError());
}

TEST_F(AuthEnforcementTest, EveryRegisteredCommandHasADefensibleRequiredRole) {
  // A verb added tomorrow inherits viewer, or trader if it writes. This asserts
  // nothing accidentally landed on anonymous, which is the only setting that
  // silently disables the check.
  const std::vector<std::string> expected_public = {"PING", "QUIT", "AUTH", "WHOAMI"};
  for (const std::string& name : registry_->commandNames()) {
    const Command* command = registry_->find(name);
    ASSERT_NE(command, nullptr) << name;
    if (command->requiredRole() == auth::Role::kAnonymous) {
      EXPECT_NE(std::find(expected_public.begin(), expected_public.end(), name), expected_public.end())
          << name << " is reachable without credentials; add it to the list above "
          << "deliberately or remove isNoAuth()";
    }
  }
}

}  // namespace
}  // namespace bourse::exec

// ---------------------------------------------------------------------------
// JSON field extraction -- it reads credentials, so it gets its own tests
// ---------------------------------------------------------------------------

namespace bourse::net {
namespace {

TEST(JsonFieldTest, ReadsFlatStringFields) {
  const std::string body = R"({"username":"alice","password":"hunter2"})";
  EXPECT_EQ(jsonFieldOf(body, "username"), "alice");
  EXPECT_EQ(jsonFieldOf(body, "password"), "hunter2");
  EXPECT_EQ(jsonFieldOf(body, "missing"), "");
}

TEST(JsonFieldTest, OnlyMatchesTopLevelKeys) {
  // A nested object must not shadow the real field. Getting this wrong on a
  // credential parser lets a caller smuggle a value past validation.
  const std::string body = R"({"profile":{"password":"nested"},"password":"real"})";
  EXPECT_EQ(jsonFieldOf(body, "password"), "real");
}

TEST(JsonFieldTest, DecodesEscapeSequences) {
  const std::string body = R"({"password":"a\"b\\c\ndAé"})";
  EXPECT_EQ(jsonFieldOf(body, "password"), "a\"b\\c\ndA\xc3\xa9");
}

TEST(JsonFieldTest, DecodesSurrogatePairs) {
  const std::string body = R"({"text":"😀"})";
  EXPECT_EQ(jsonFieldOf(body, "text"), "\xf0\x9f\x98\x80");
}

TEST(JsonFieldTest, TreatsNonStringValuesAsAbsent) {
  // Coercing here is how a caller ends up authenticating as the literal
  // string "null" or "123".
  bool found = true;
  EXPECT_EQ(jsonFieldOf(R"({"password":123})", "password", &found), "");
  EXPECT_FALSE(found);
  EXPECT_EQ(jsonFieldOf(R"({"password":null})", "password"), "");
  EXPECT_EQ(jsonFieldOf(R"({"password":{"a":"b"}})", "password"), "");
  EXPECT_EQ(jsonFieldOf(R"({"password":["a"]})", "password"), "");
}

TEST(JsonFieldTest, DistinguishesAbsentFromEmpty) {
  bool found = false;
  EXPECT_EQ(jsonFieldOf(R"({"password":""})", "password", &found), "");
  EXPECT_TRUE(found);

  found = true;
  EXPECT_EQ(jsonFieldOf(R"({"other":"x"})", "password", &found), "");
  EXPECT_FALSE(found);
}

TEST(JsonFieldTest, SkipsPrecedingFieldsOfEveryType) {
  const std::string body =
      R"({"n":1,"f":1.5e-3,"t":true,"f2":false,"z":null,"arr":[1,{"k":"v"}],"obj":{"a":[2]},"password":"end"})";
  EXPECT_EQ(jsonFieldOf(body, "password"), "end");
}

TEST(JsonFieldTest, ReturnsEmptyForMalformedInput) {
  for (const char* broken : {"", "not json", "{", R"({"a")", R"({"a":})", R"({"a":"unterminated)",
                             R"({"a":"bad\escape"})", R"(["array"])", R"({"a":"\ud83d"})"}) {
    EXPECT_EQ(jsonFieldOf(broken, "a"), "") << broken;
  }
}

TEST(JsonFieldTest, RefusesDeeplyNestedInput) {
  // Bounded rather than recursive, so a hostile body cannot exhaust the stack.
  std::string body = R"({"deep":)";
  body.append(64, '[');
  body.append(64, ']');
  body += R"(,"password":"x"})";
  EXPECT_EQ(jsonFieldOf(body, "password"), "");
}

TEST(JsonFieldTest, ToleratesWhitespace) {
  const std::string body = "{\n  \"username\" : \"alice\" ,\n  \"password\" : \"pw\"\n}";
  EXPECT_EQ(jsonFieldOf(body, "username"), "alice");
  EXPECT_EQ(jsonFieldOf(body, "password"), "pw");
}

}  // namespace
}  // namespace bourse::net
