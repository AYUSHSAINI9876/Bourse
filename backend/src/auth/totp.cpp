#include "bourse/auth/totp.hpp"

#include <array>
#include <cctype>

#include "bourse/auth/crypto.hpp"

namespace bourse::auth {
namespace {

constexpr std::string_view kBase32Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

/// Powers of ten up to 10^8, so HOTP's modulus is a lookup rather than a call
/// into <cmath> that returns a double and has to be rounded back.
constexpr std::uint32_t kPowersOfTen[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

/// Zero-pads to `digits`. A code whose numeric value is 41 must be shown as
/// "000041"; trimming the leading zeros produces something no app will match.
std::string padCode(std::uint32_t value, int digits) {
  std::string out = std::to_string(value);
  if (out.size() < static_cast<std::size_t>(digits)) {
    out.insert(0, static_cast<std::size_t>(digits) - out.size(), '0');
  }
  return out;
}

}  // namespace

std::string base32Encode(std::string_view data) {
  std::string out;
  out.reserve(((data.size() + 4) / 5) * 8);

  std::size_t i = 0;
  while (i < data.size()) {
    // Take up to five bytes (40 bits) and emit eight 5-bit groups.
    std::uint64_t buffer = 0;
    std::size_t bytes = 0;
    for (; bytes < 5 && i + bytes < data.size(); ++bytes) {
      buffer = (buffer << 8) | static_cast<std::uint8_t>(data[i + bytes]);
    }
    // Left-align a partial group so the first output character carries the most
    // significant bits, as the encoding requires.
    buffer <<= (5 - bytes) * 8;

    const std::size_t groups = (bytes * 8 + 4) / 5;
    for (std::size_t g = 0; g < 8; ++g) {
      if (g < groups) {
        const auto index = static_cast<std::size_t>((buffer >> (35 - 5 * g)) & 0x1f);
        out.push_back(kBase32Alphabet[index]);
      } else {
        out.push_back('=');
      }
    }
    i += bytes;
  }
  return out;
}

Result<std::string> base32Decode(std::string_view text) {
  std::string out;
  std::uint64_t buffer = 0;
  int bits = 0;

  for (const char raw : text) {
    if (raw == '=' || raw == ' ' || raw == '-') {
      // Padding, plus the spaces and dashes people insert when copying a secret
      // off a screen. None of them carries information.
      continue;
    }
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
    const std::size_t index = kBase32Alphabet.find(c);
    if (index == std::string_view::npos) {
      return Status::invalidArgument(std::string("not a base32 character: '") + raw + "'");
    }
    buffer = (buffer << 5) | index;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xff));
    }
  }
  return out;
}

Result<std::string> hotp(std::string_view secret, std::uint64_t counter, int digits) {
  if (digits < 6 || digits > 8) {
    return Status::invalidArgument("HOTP digits must be 6, 7 or 8");
  }
  if (secret.empty()) {
    return Status::invalidArgument("HOTP secret must not be empty");
  }

  // RFC 4226 section 5.2: the counter is eight bytes, big-endian.
  std::array<char, 8> message{};
  for (int i = 7; i >= 0; --i) {
    message[static_cast<std::size_t>(i)] = static_cast<char>(counter & 0xff);
    counter >>= 8;
  }

  const Sha1::Digest mac = hmacSha1(secret, std::string_view(message.data(), message.size()));

  // Section 5.3, dynamic truncation: the low nibble of the last byte selects
  // where to read, so different counters expose different parts of the digest
  // rather than always the same bits.
  const std::size_t offset = mac[mac.size() - 1] & 0x0f;
  const std::uint32_t binary = (static_cast<std::uint32_t>(mac[offset] & 0x7f) << 24) |
                               (static_cast<std::uint32_t>(mac[offset + 1]) << 16) |
                               (static_cast<std::uint32_t>(mac[offset + 2]) << 8) |
                               static_cast<std::uint32_t>(mac[offset + 3]);

  return padCode(binary % kPowersOfTen[digits], digits);
}

std::int64_t totpStep(std::int64_t unix_seconds, std::int64_t period) {
  if (period <= 0) {
    return 0;
  }
  // Floor division, which differs from C++'s truncation toward zero for
  // negative values. Pre-1970 timestamps are nonsense here, but a step counter
  // that jumps backwards as it crosses zero is a baffling bug to chase.
  const std::int64_t quotient = unix_seconds / period;
  return (unix_seconds < 0 && unix_seconds % period != 0) ? quotient - 1 : quotient;
}

Result<std::string> totp(std::string_view secret, std::int64_t unix_seconds, int digits,
                         std::int64_t period) {
  if (period <= 0) {
    return Status::invalidArgument("TOTP period must be positive");
  }
  return hotp(secret, static_cast<std::uint64_t>(totpStep(unix_seconds, period)), digits);
}

TotpVerification verifyTotp(std::string_view secret, std::string_view code, std::int64_t unix_seconds,
                            std::int64_t last_accepted_step, int digits, int tolerance) {
  TotpVerification result;
  if (code.empty() || tolerance < 0) {
    return result;
  }

  const std::int64_t current = totpStep(unix_seconds, kTotpPeriodSeconds);
  for (int delta = -tolerance; delta <= tolerance; ++delta) {
    const std::int64_t step = current + delta;

    // Refuse a code that has already been used. Without this a code stays valid
    // for its whole window, so anyone who observes one has the rest of that
    // window to replay it -- precisely what a second factor exists to prevent.
    if (step <= last_accepted_step) {
      continue;
    }

    const Result<std::string> expected = hotp(secret, static_cast<std::uint64_t>(step), digits);
    if (!expected.ok()) {
      return result;
    }
    // Constant-time: within its window a code is a shared secret, and an early
    // return would leak how many leading digits were correct.
    if (constantTimeEquals(expected.value(), code)) {
      result.accepted = true;
      result.step = step;
      return result;
    }
  }
  return result;
}

std::string totpProvisioningUri(std::string_view issuer, std::string_view account,
                                std::string_view base32_secret, int digits, std::int64_t period) {
  // Percent-encoding restricted to what actually appears in an issuer or an
  // account name. A general URI encoder would be more code and no more correct
  // for this input.
  const auto encode = [](std::string_view text) {
    std::string out;
    for (const char c : text) {
      const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '.' || c == '_' || c == '~';
      if (safe) {
        out.push_back(c);
      } else {
        static constexpr char kHex[] = "0123456789ABCDEF";
        out.push_back('%');
        out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0x0f]);
        out.push_back(kHex[static_cast<unsigned char>(c) & 0x0f]);
      }
    }
    return out;
  };

  // Padding is stripped: '=' is legal Base32, but several apps reject it inside
  // a URI rather than percent-decoding it first.
  std::string secret(base32_secret);
  while (!secret.empty() && secret.back() == '=') {
    secret.pop_back();
  }

  std::string uri = "otpauth://totp/";
  uri += encode(issuer);
  uri += "%3A";  // a literal ':' separating issuer from account, per the spec
  uri += encode(account);
  uri += "?secret=" + secret;
  uri += "&issuer=" + encode(issuer);
  uri += "&algorithm=SHA1";
  uri += "&digits=" + std::to_string(digits);
  uri += "&period=" + std::to_string(period);
  return uri;
}

Result<std::string> generateTotpSecret() {
  Result<std::string> bytes = randomBytes(20);  // 160 bits, per RFC 4226 section 4 R6
  if (!bytes.ok()) {
    return bytes.status();
  }
  return base32Encode(bytes.value());
}

}  // namespace bourse::auth
