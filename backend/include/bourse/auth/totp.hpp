#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "bourse/core/result.hpp"

/// \file totp.hpp
/// Time-based one-time passwords, RFC 6238 over RFC 4226.
///
/// Implemented here rather than delegated for the same reason the password
/// hashing is: it is a published standard with published test vectors, so
/// correctness is decidable. `test_totp.cpp` checks every function against the
/// vectors in RFC 4226 Appendix D and RFC 6238 Appendix B, which is the
/// difference between "I wrote 2FA" and "I wrote 2FA that a phone agrees with".

namespace bourse::auth {

/// Base32 as RFC 4648 defines it: A-Z2-7, padded to a multiple of 8 with '='.
///
/// This is the alphabet authenticator apps expect in an `otpauth://` secret.
/// Base64 would be a smaller encoding and completely useless here.
[[nodiscard]] std::string base32Encode(std::string_view data);

/// Decodes Base32. Padding is optional and case is ignored, because users
/// retype these by hand from a screen and will not reproduce either faithfully.
/// Characters outside the alphabet are an error rather than skipped: silently
/// ignoring a typo produces a secret that is wrong in a way nobody can debug.
[[nodiscard]] Result<std::string> base32Decode(std::string_view text);

/// RFC 4226 §5.3. `digits` is 6 or 8; anything else is an error.
///
/// The "dynamic truncation" it performs -- taking the low nibble of the last
/// byte as an offset, then reading four bytes from there -- exists so that the
/// same HMAC yields a different slice per counter, rather than always exposing
/// the same bits of the digest.
[[nodiscard]] Result<std::string> hotp(std::string_view secret, std::uint64_t counter, int digits = 6);

/// Seconds per code. 30 is the value every authenticator app assumes, and the
/// only one an `otpauth://` URI can omit.
inline constexpr std::int64_t kTotpPeriodSeconds = 30;

/// RFC 6238: HOTP with the counter derived from the clock.
[[nodiscard]] Result<std::string> totp(std::string_view secret, std::int64_t unix_seconds, int digits = 6,
                                       std::int64_t period = kTotpPeriodSeconds);

/// The time step a moment falls in. Exposed because replay prevention needs to
/// remember which step a code was accepted for.
[[nodiscard]] std::int64_t totpStep(std::int64_t unix_seconds, std::int64_t period = kTotpPeriodSeconds);

struct TotpVerification {
  bool accepted = false;
  /// Which step matched. Store it and refuse anything <= it next time: without
  /// that, a code stays valid for its whole window and an attacker who observes
  /// one has ~30 seconds to replay it.
  std::int64_t step = 0;
};

/// Verifies a presented code, allowing `tolerance` steps either side.
///
/// One step of tolerance (the default) accepts codes up to 30 seconds stale,
/// which covers ordinary clock drift between a phone and a server. More than
/// that widens the replay window for no real gain.
///
/// `last_accepted_step` is the step this account last authenticated with; a
/// code from that step or earlier is refused even if it is otherwise valid.
[[nodiscard]] TotpVerification verifyTotp(std::string_view secret, std::string_view code,
                                          std::int64_t unix_seconds, std::int64_t last_accepted_step = -1,
                                          int digits = 6, int tolerance = 1);

/// Builds the `otpauth://totp/...` URI an authenticator app enrols from.
///
/// `issuer` appears twice by design -- once as a label prefix and once as a
/// parameter -- because older apps read one and newer apps read the other.
[[nodiscard]] std::string totpProvisioningUri(std::string_view issuer, std::string_view account,
                                              std::string_view base32_secret, int digits = 6,
                                              std::int64_t period = kTotpPeriodSeconds);

/// A fresh 160-bit secret, Base32-encoded. 160 bits is what RFC 4226 §4 R6
/// recommends and matches the HMAC-SHA1 block structure.
[[nodiscard]] Result<std::string> generateTotpSecret();

}  // namespace bourse::auth
