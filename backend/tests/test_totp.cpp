// SHA-1, Base32, HOTP and TOTP, checked against the vectors published with
// each standard.
//
// This is the whole justification for implementing them rather than linking
// something: a one-time-password implementation is either bit-for-bit
// compatible with what a phone generates or it is useless, and the RFCs print
// exactly the table needed to prove which.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bourse/auth/crypto.hpp"
#include "bourse/auth/totp.hpp"

namespace bourse::auth {
namespace {

/// RFC 4226 Appendix D and RFC 6238 Appendix B both use this seed, given as
/// the ASCII string "12345678901234567890".
const std::string kRfcSecret = "12345678901234567890";

// ---------------------------------------------------------------------------
// SHA-1 -- FIPS 180-4
// ---------------------------------------------------------------------------

TEST(Sha1Test, MatchesTheFipsVectors) {
  EXPECT_EQ(toHex(Sha1::hash("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(toHex(Sha1::hash("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(toHex(Sha1::hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(Sha1Test, HandlesTheLengthPaddingBoundaries) {
  // 55, 56 and 64 bytes exercise each padding branch: room for the length, no
  // room so an extra block is needed, and exactly full. Getting this wrong
  // yields a hash that is correct for most inputs, which is the failure mode
  // that survives longest.
  for (const std::size_t length : {size_t{54}, size_t{55}, size_t{56}, size_t{63}, size_t{64}, size_t{65},
                                   size_t{119}, size_t{120}, size_t{128}}) {
    const std::string message(length, 'a');
    Sha1 streaming;
    streaming.update(message);
    EXPECT_EQ(toHex(streaming.finish()), toHex(Sha1::hash(message))) << "length " << length;
  }
}

TEST(Sha1Test, StreamingInAnyChunkingMatchesOneShot) {
  const std::string message = std::string(1000, 'x') + "tail";
  const std::string expected = toHex(Sha1::hash(message));
  for (const std::size_t chunk : {size_t{1}, size_t{7}, size_t{63}, size_t{64}, size_t{65}}) {
    Sha1 sha;
    for (std::size_t offset = 0; offset < message.size(); offset += chunk) {
      sha.update(message.data() + offset, std::min(chunk, message.size() - offset));
    }
    EXPECT_EQ(toHex(sha.finish()), expected) << "chunk " << chunk;
  }
}

TEST(HmacSha1Test, MatchesRfc2202Vectors) {
  EXPECT_EQ(toHex(hmacSha1(std::string(20, '\x0b'), "Hi There")), "b617318655057264e28bc0b6fb378c8ef146be00");
  EXPECT_EQ(toHex(hmacSha1("Jefe", "what do ya want for nothing?")),
            "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
  // A key longer than the 64-byte block must be hashed down first; skipping
  // that reduction is the classic HMAC bug.
  EXPECT_EQ(
      toHex(hmacSha1(std::string(80, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")),
      "aa4ae5e15272d00e95705637ce8a3b55ed402112");
}

// ---------------------------------------------------------------------------
// Base32 -- RFC 4648
// ---------------------------------------------------------------------------

TEST(Base32Test, MatchesRfc4648Vectors) {
  EXPECT_EQ(base32Encode(""), "");
  EXPECT_EQ(base32Encode("f"), "MY======");
  EXPECT_EQ(base32Encode("fo"), "MZXQ====");
  EXPECT_EQ(base32Encode("foo"), "MZXW6===");
  EXPECT_EQ(base32Encode("foob"), "MZXW6YQ=");
  EXPECT_EQ(base32Encode("fooba"), "MZXW6YTB");
  EXPECT_EQ(base32Encode("foobar"), "MZXW6YTBOI======");
}

TEST(Base32Test, RoundTrips) {
  for (const char* text : {"", "f", "fo", "foo", "foob", "fooba", "foobar", "12345678901234567890"}) {
    const Result<std::string> back = base32Decode(base32Encode(text));
    ASSERT_TRUE(back.ok()) << text;
    EXPECT_EQ(back.value(), std::string(text));
  }
}

TEST(Base32Test, ToleratesWhatPeopleActuallyType) {
  // Users retype these off a screen. Case and the separators a UI inserts for
  // legibility carry no information, so rejecting them would only produce
  // support requests.
  const std::string canonical = base32Encode("foobar");
  for (const char* variant : {"mzxw6ytboi======", "MZXW6YTBOI", "MZXW 6YTB OI", "MZXW-6YTB-OI"}) {
    const Result<std::string> decoded = base32Decode(variant);
    ASSERT_TRUE(decoded.ok()) << variant;
    EXPECT_EQ(decoded.value(), "foobar") << variant;
  }
  EXPECT_EQ(canonical, "MZXW6YTBOI======");
}

TEST(Base32Test, RejectsCharactersOutsideTheAlphabet) {
  // Silently skipping a typo yields a secret that is wrong in a way nobody can
  // debug: the codes simply never match and there is nothing to look at.
  for (const char* bad : {"MZXW6YTB01", "MZXW6YTB!I", "MZXW6YTB8I"}) {
    EXPECT_FALSE(base32Decode(bad).ok()) << bad;
  }
}

// ---------------------------------------------------------------------------
// HOTP -- RFC 4226 Appendix D
// ---------------------------------------------------------------------------

TEST(HotpTest, MatchesRfc4226AppendixD) {
  const std::vector<std::string> expected = {"755224", "287082", "359152", "969429", "338314",
                                             "254676", "287922", "162583", "399871", "520489"};
  for (std::uint64_t counter = 0; counter < expected.size(); ++counter) {
    const Result<std::string> code = hotp(kRfcSecret, counter);
    ASSERT_TRUE(code.ok()) << counter;
    EXPECT_EQ(code.value(), expected[counter]) << "counter " << counter;
  }
}

TEST(HotpTest, PadsShortCodesRatherThanTrimmingThem) {
  // A code whose numeric value has fewer digits than requested must keep its
  // leading zeros; trimming produces something no authenticator will match.
  for (std::uint64_t counter = 0; counter < 500; ++counter) {
    const Result<std::string> code = hotp(kRfcSecret, counter);
    ASSERT_TRUE(code.ok());
    EXPECT_EQ(code.value().size(), 6u) << "counter " << counter << " gave " << code.value();
  }
}

TEST(HotpTest, RejectsUnsupportedDigitCounts) {
  EXPECT_FALSE(hotp(kRfcSecret, 0, 5).ok());
  EXPECT_FALSE(hotp(kRfcSecret, 0, 9).ok());
  EXPECT_FALSE(hotp("", 0).ok());
}

// ---------------------------------------------------------------------------
// TOTP -- RFC 6238 Appendix B
// ---------------------------------------------------------------------------

TEST(TotpTest, MatchesRfc6238AppendixB) {
  // The RFC's SHA-1 column, at eight digits.
  const std::vector<std::pair<std::int64_t, std::string>> vectors = {
      {59, "94287082"},         {1111111109, "07081804"}, {1111111111, "14050471"},
      {1234567890, "89005924"}, {2000000000, "69279037"}, {20000000000, "65353130"},
  };
  for (const auto& [time, expected] : vectors) {
    const Result<std::string> code = totp(kRfcSecret, time, 8);
    ASSERT_TRUE(code.ok()) << time;
    EXPECT_EQ(code.value(), expected) << "T=" << time;
  }
}

TEST(TotpTest, StepAdvancesEveryThirtySeconds) {
  EXPECT_EQ(totpStep(0), 0);
  EXPECT_EQ(totpStep(29), 0);
  EXPECT_EQ(totpStep(30), 1);
  EXPECT_EQ(totpStep(59), 1);
  EXPECT_EQ(totpStep(60), 2);
  // Floors rather than truncating toward zero, so the counter never runs
  // backwards as it crosses the epoch.
  EXPECT_EQ(totpStep(-1), -1);
  EXPECT_EQ(totpStep(-30), -1);
  EXPECT_EQ(totpStep(-31), -2);
}

TEST(TotpTest, AcceptsTheCurrentCode) {
  const std::int64_t now = 1'700'000'000;
  const Result<std::string> code = totp(kRfcSecret, now);
  ASSERT_TRUE(code.ok());
  const TotpVerification result = verifyTotp(kRfcSecret, code.value(), now);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.step, totpStep(now));
}

TEST(TotpTest, ToleratesOneStepOfClockDrift) {
  const std::int64_t now = 1'700'000'000;
  // A phone thirty seconds behind or ahead still works; that is ordinary drift
  // between two clocks, not an attack.
  for (const std::int64_t offset : {-30, 0, 30}) {
    const Result<std::string> code = totp(kRfcSecret, now + offset);
    ASSERT_TRUE(code.ok());
    EXPECT_TRUE(verifyTotp(kRfcSecret, code.value(), now).accepted) << "offset " << offset;
  }
  // Two steps away is outside the window.
  const Result<std::string> stale = totp(kRfcSecret, now - 90);
  ASSERT_TRUE(stale.ok());
  EXPECT_FALSE(verifyTotp(kRfcSecret, stale.value(), now).accepted);
}

TEST(TotpTest, RefusesAReplayedCode) {
  // The property that makes this a second factor rather than a second password:
  // observing a code must not let it be used again inside its window.
  const std::int64_t now = 1'700'000'000;
  const Result<std::string> code = totp(kRfcSecret, now);
  ASSERT_TRUE(code.ok());

  const TotpVerification first = verifyTotp(kRfcSecret, code.value(), now);
  ASSERT_TRUE(first.accepted);

  const TotpVerification replay = verifyTotp(kRfcSecret, code.value(), now, first.step);
  EXPECT_FALSE(replay.accepted) << "a code accepted once was accepted again";
}

TEST(TotpTest, ReplayGuardDoesNotBlockTheNextCode) {
  const std::int64_t now = 1'700'000'000;
  const TotpVerification first = verifyTotp(kRfcSecret, totp(kRfcSecret, now).value(), now);
  ASSERT_TRUE(first.accepted);

  // Thirty seconds later a genuinely new code must still be accepted -- a
  // replay guard that also locks out the legitimate next code is worse than
  // none, because it locks the user out of their own account.
  const std::int64_t later = now + 30;
  const Result<std::string> next = totp(kRfcSecret, later);
  ASSERT_TRUE(next.ok());
  EXPECT_TRUE(verifyTotp(kRfcSecret, next.value(), later, first.step).accepted);
}

TEST(TotpTest, RejectsWrongAndMalformedCodes) {
  const std::int64_t now = 1'700'000'000;
  EXPECT_FALSE(verifyTotp(kRfcSecret, "000000", now).accepted || totp(kRfcSecret, now).value() == "000000");
  EXPECT_FALSE(verifyTotp(kRfcSecret, "", now).accepted);
  EXPECT_FALSE(verifyTotp(kRfcSecret, "12345", now).accepted);
  EXPECT_FALSE(verifyTotp(kRfcSecret, "abcdef", now).accepted);
}

// ---------------------------------------------------------------------------
// Enrolment
// ---------------------------------------------------------------------------

TEST(TotpEnrolmentTest, GeneratesADistinctDecodableSecret) {
  const Result<std::string> a = generateTotpSecret();
  const Result<std::string> b = generateTotpSecret();
  ASSERT_TRUE(a.ok()) << a.status().toString();
  ASSERT_TRUE(b.ok());
  EXPECT_NE(a.value(), b.value());

  const Result<std::string> raw = base32Decode(a.value());
  ASSERT_TRUE(raw.ok());
  EXPECT_EQ(raw.value().size(), 20u) << "RFC 4226 R6 asks for a 160-bit secret";
}

TEST(TotpEnrolmentTest, ProvisioningUriIsShapedTheWayAppsExpect) {
  const std::string uri = totpProvisioningUri("Bourse", "alice@example.com", "MZXW6YTBOI======");

  EXPECT_EQ(uri.rfind("otpauth://totp/", 0), 0u);
  // The '@' must be percent-encoded or the label terminates early.
  EXPECT_NE(uri.find("alice%40example.com"), std::string::npos) << uri;
  // Padding stripped: '=' is legal base32 but several apps refuse it in a URI.
  EXPECT_EQ(uri.find("secret=MZXW6YTBOI&"), uri.find("secret=")) << uri;
  EXPECT_EQ(uri.find('='), uri.find("secret=") + 6) << uri;
  EXPECT_NE(uri.find("issuer=Bourse"), std::string::npos);
  EXPECT_NE(uri.find("algorithm=SHA1"), std::string::npos);
  EXPECT_NE(uri.find("period=30"), std::string::npos);
}

TEST(TotpEnrolmentTest, AGeneratedSecretProducesVerifiableCodes) {
  // End to end: enrol, decode as an app would, generate, verify.
  const Result<std::string> base32 = generateTotpSecret();
  ASSERT_TRUE(base32.ok());
  const Result<std::string> secret = base32Decode(base32.value());
  ASSERT_TRUE(secret.ok());

  const std::int64_t now = 1'700'000'123;
  const Result<std::string> code = totp(secret.value(), now);
  ASSERT_TRUE(code.ok());
  EXPECT_EQ(code.value().size(), 6u);
  EXPECT_TRUE(verifyTotp(secret.value(), code.value(), now).accepted);
}

}  // namespace
}  // namespace bourse::auth
