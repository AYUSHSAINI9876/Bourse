#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bourse/core/result.hpp"

/// \file crypto.hpp
/// SHA-256, HMAC-SHA256 and PBKDF2 -- implemented here rather than linked in.
///
/// The usual and correct advice is "do not write your own crypto". What that
/// warns against is *inventing* a scheme. These are published standards
/// (FIPS 180-4, RFC 2104, RFC 8018) whose correctness is decidable: the test
/// suite checks every primitive against the vectors in those documents and
/// then cross-checks PBKDF2 against Python's hashlib over randomised inputs.
/// An implementation that agrees with a reference on the published vectors is
/// either right or wrong in a way that is visible, which is not true of a
/// scheme someone made up.
///
/// The trade-off that remains is side channels: this is a straightforward
/// implementation with no cache-timing hardening. Password comparison is
/// constant-time (see `constantTimeEquals`), and PBKDF2's cost is dominated by
/// its iteration count rather than by data-dependent branching, so the
/// realistic exposure is low. A deployment holding real user accounts should
/// still link libsodium and use Argon2id; that is a one-file change behind
/// `hashPassword`/`verifyPassword` and is noted in docs/security.md.

namespace bourse::auth {

/// Streaming SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  static constexpr std::size_t kDigestSize = 32;
  static constexpr std::size_t kBlockSize = 64;

  using Digest = std::array<std::uint8_t, kDigestSize>;

  Sha256() noexcept { reset(); }

  void reset() noexcept;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }

  /// Finalises and returns the digest. The object is spent afterwards; call
  /// `reset()` to reuse it.
  [[nodiscard]] Digest finish() noexcept;

  [[nodiscard]] static Digest hash(std::string_view text) noexcept;
  [[nodiscard]] static Digest hash(const void* data, std::size_t size) noexcept;

 private:
  void compress(const std::uint8_t block[kBlockSize]) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, kBlockSize> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bits_ = 0;
};

/// HMAC-SHA256 (RFC 2104).
[[nodiscard]] Sha256::Digest hmacSha256(std::string_view key, std::string_view message) noexcept;

/// PBKDF2-HMAC-SHA256 (RFC 8018 §5.2).
///
/// `iterations` is the work factor; `length` is the derived-key length in
/// bytes. Cost is linear in iterations by design -- that is the entire point,
/// and it is why `login` is rate-limited rather than left open.
[[nodiscard]] std::vector<std::uint8_t> pbkdf2HmacSha256(std::string_view password, std::string_view salt,
                                                        std::uint32_t iterations, std::size_t length);

/// Compares without an early return.
///
/// `a == b` on std::string stops at the first differing byte, so the time it
/// takes reveals how many leading bytes an attacker guessed correctly. Over
/// enough attempts that recovers a token one byte at a time. This reads both
/// buffers to the end regardless. Length is compared up front and is not
/// secret -- token and digest lengths are fixed and public.
[[nodiscard]] bool constantTimeEquals(std::string_view a, std::string_view b) noexcept;

[[nodiscard]] std::string toHex(const void* data, std::size_t size);
[[nodiscard]] std::string toHex(const Sha256::Digest& digest);
[[nodiscard]] Result<std::string> fromHex(std::string_view hex);

/// Cryptographically secure random bytes, read from the operating system.
///
/// Not `std::mt19937` seeded from the clock, and not `std::random_device`,
/// whose quality is implementation-defined and which has historically been a
/// constant on some toolchains. A short read is a hard error rather than a
/// silent fallback: silently degrading a CSPRNG produces guessable session
/// tokens and nothing else looks wrong.
[[nodiscard]] Result<std::string> randomBytes(std::size_t count);

/// A URL-safe random token, hex-encoded. 32 bytes = 256 bits of entropy.
[[nodiscard]] Result<std::string> randomToken(std::size_t entropy_bytes = 32);

/// Cost factor for new password hashes.
///
/// OWASP's 2023 guidance for PBKDF2-HMAC-SHA256 is 600,000. This is lower
/// because every login pays it on a single shared core of a free-tier
/// container, where 600k adds roughly a second of CPU per attempt and turns
/// the login endpoint into its own denial-of-service. 210,000 with a
/// per-account and per-address rate limit in front of it is the trade this
/// deployment makes; `--auth-iterations` raises it where the CPU exists.
inline constexpr std::uint32_t kDefaultPbkdf2Iterations = 210000;

/// Hashes a password for storage.
///
/// Returns `pbkdf2_sha256$<iterations>$<salt_hex>$<derived_hex>` -- the same
/// shape Django and passlib use. Self-describing on purpose: the iteration
/// count travels with the hash, so raising the cost factor later does not
/// invalidate existing credentials, and `verifyPassword` can report that a
/// stored hash needs upgrading.
[[nodiscard]] Result<std::string> hashPassword(std::string_view password,
                                               std::uint32_t iterations = kDefaultPbkdf2Iterations);

/// Verifies a password against an encoded hash.
///
/// A malformed `encoded` is an error, not a `false`: the two mean different
/// things (corrupt user store vs. wrong password) and conflating them hides
/// the first behind a stream of failed logins.
[[nodiscard]] Result<bool> verifyPassword(std::string_view password, std::string_view encoded);

/// True when `encoded` was produced with fewer iterations than `desired`, so
/// the caller can transparently re-hash on the next successful login.
[[nodiscard]] bool needsRehash(std::string_view encoded, std::uint32_t desired) noexcept;

}  // namespace bourse::auth
