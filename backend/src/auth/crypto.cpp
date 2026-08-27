#include "bourse/auth/crypto.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "bourse/auth/principal.hpp"

namespace bourse::auth {
namespace {

// FIPS 180-4 §4.2.2: the first 32 bits of the fractional parts of the cube
// roots of the first 64 primes.
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// FIPS 180-4 §5.3.3: fractional parts of the square roots of the first 8 primes.
constexpr std::uint32_t kInitialState[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept {
  return (x >> n) | (x << (32U - n));
}

constexpr std::uint32_t bigSigma0(std::uint32_t x) noexcept {
  return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

constexpr std::uint32_t bigSigma1(std::uint32_t x) noexcept {
  return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

constexpr std::uint32_t smallSigma0(std::uint32_t x) noexcept {
  return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

constexpr std::uint32_t smallSigma1(std::uint32_t x) noexcept {
  return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

constexpr std::uint32_t choose(std::uint32_t e, std::uint32_t f, std::uint32_t g) noexcept {
  return (e & f) ^ (~e & g);
}

constexpr std::uint32_t majority(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept {
  return (a & b) ^ (a & c) ^ (b & c);
}

/// Big-endian load/store. SHA-256 is defined over big-endian words and this
/// code runs on little-endian hardware, so the conversion is explicit rather
/// than a reinterpret_cast that would silently produce a different hash.
constexpr std::uint32_t loadBe32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

constexpr void storeBe32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}

constexpr void storeBe64(std::uint8_t* p, std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
  }
}

/// Overwrites a buffer that held key material.
///
/// A plain `memset` here is removable by the optimiser under the as-if rule --
/// the compiler can see nothing reads the buffer afterwards -- which is how
/// secrets survive in freed stack frames. The volatile pointer makes each
/// store observable, so it cannot be elided.
void secureZero(void* data, std::size_t size) noexcept {
  auto* p = static_cast<volatile std::uint8_t*>(data);
  while (size-- > 0) {
    *p++ = 0;
  }
}

}  // namespace

bool parseRole(std::string_view text, Role& out) noexcept {
  if (text == "viewer") {
    out = Role::kViewer;
    return true;
  }
  if (text == "trader") {
    out = Role::kTrader;
    return true;
  }
  if (text == "admin") {
    out = Role::kAdmin;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

void Sha256::reset() noexcept {
  std::copy(std::begin(kInitialState), std::end(kInitialState), state_.begin());
  buffered_ = 0;
  total_bits_ = 0;
}

void Sha256::compress(const std::uint8_t block[kBlockSize]) noexcept {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = loadBe32(block + 4 * i);
  }
  for (int i = 16; i < 64; ++i) {
    w[i] = smallSigma1(w[i - 2]) + w[i - 7] + smallSigma0(w[i - 15]) + w[i - 16];
  }

  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t t1 = h + bigSigma1(e) + choose(e, f, g) + kRoundConstants[i] + w[i];
    const std::uint32_t t2 = bigSigma0(a) + majority(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;

  secureZero(w, sizeof(w));
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  const auto* input = static_cast<const std::uint8_t*>(data);
  total_bits_ += static_cast<std::uint64_t>(size) * 8;

  // Top up a partial block first, then run whole blocks straight from the
  // caller's buffer without copying.
  if (buffered_ > 0) {
    const std::size_t take = std::min(kBlockSize - buffered_, size);
    std::memcpy(buffer_.data() + buffered_, input, take);
    buffered_ += take;
    input += take;
    size -= take;
    if (buffered_ == kBlockSize) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }

  while (size >= kBlockSize) {
    compress(input);
    input += kBlockSize;
    size -= kBlockSize;
  }

  if (size > 0) {
    std::memcpy(buffer_.data(), input, size);
    buffered_ = size;
  }
}

Sha256::Digest Sha256::finish() noexcept {
  // FIPS 180-4 §5.1.1: append 0x80, then zeros, then the 64-bit big-endian
  // length, so the message ends on a block boundary.
  const std::uint64_t bits = total_bits_;
  std::uint8_t padding[kBlockSize * 2] = {0x80};
  const std::size_t remainder = static_cast<std::size_t>((bits / 8) % kBlockSize);
  const std::size_t pad_len = (remainder < 56) ? (56 - remainder) : (120 - remainder);
  storeBe64(padding + pad_len, bits);
  update(padding, pad_len + 8);

  Digest digest{};
  for (int i = 0; i < 8; ++i) {
    storeBe32(digest.data() + 4 * i, state_[i]);
  }
  secureZero(buffer_.data(), buffer_.size());
  return digest;
}

Sha256::Digest Sha256::hash(const void* data, std::size_t size) noexcept {
  Sha256 sha;
  sha.update(data, size);
  return sha.finish();
}

Sha256::Digest Sha256::hash(std::string_view text) noexcept {
  return hash(text.data(), text.size());
}

// ---------------------------------------------------------------------------
// HMAC-SHA256 (RFC 2104)
// ---------------------------------------------------------------------------

Sha256::Digest hmacSha256(std::string_view key, std::string_view message) noexcept {
  std::uint8_t block_key[Sha256::kBlockSize] = {};

  // A key longer than the block size is hashed down first; anything shorter is
  // zero-padded. Both are RFC 2104 §2.
  if (key.size() > Sha256::kBlockSize) {
    const Sha256::Digest reduced = Sha256::hash(key);
    std::memcpy(block_key, reduced.data(), reduced.size());
  } else if (!key.empty()) {
    std::memcpy(block_key, key.data(), key.size());
  }

  std::uint8_t inner_pad[Sha256::kBlockSize];
  std::uint8_t outer_pad[Sha256::kBlockSize];
  for (std::size_t i = 0; i < Sha256::kBlockSize; ++i) {
    inner_pad[i] = static_cast<std::uint8_t>(block_key[i] ^ 0x36);
    outer_pad[i] = static_cast<std::uint8_t>(block_key[i] ^ 0x5c);
  }

  Sha256 inner;
  inner.update(inner_pad, sizeof(inner_pad));
  inner.update(message.data(), message.size());
  const Sha256::Digest inner_digest = inner.finish();

  Sha256 outer;
  outer.update(outer_pad, sizeof(outer_pad));
  outer.update(inner_digest.data(), inner_digest.size());
  const Sha256::Digest result = outer.finish();

  secureZero(block_key, sizeof(block_key));
  secureZero(inner_pad, sizeof(inner_pad));
  secureZero(outer_pad, sizeof(outer_pad));
  return result;
}

// ---------------------------------------------------------------------------
// SHA-1 (FIPS 180-4) -- for TOTP only; see the note in crypto.hpp
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kSha1InitialState[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

constexpr std::uint32_t rotl(std::uint32_t x, unsigned n) noexcept {
  return (x << n) | (x >> (32U - n));
}

}  // namespace

void Sha1::reset() noexcept {
  std::copy(std::begin(kSha1InitialState), std::end(kSha1InitialState), state_.begin());
  buffered_ = 0;
  total_bits_ = 0;
}

void Sha1::compress(const std::uint8_t block[kBlockSize]) noexcept {
  std::uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = loadBe32(block + 4 * i);
  }
  for (int i = 16; i < 80; ++i) {
    w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];

  for (int i = 0; i < 80; ++i) {
    std::uint32_t f = 0;
    std::uint32_t k = 0;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = temp;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;

  secureZero(w, sizeof(w));
}

void Sha1::update(const void* data, std::size_t size) noexcept {
  const auto* input = static_cast<const std::uint8_t*>(data);
  total_bits_ += static_cast<std::uint64_t>(size) * 8;

  if (buffered_ > 0) {
    const std::size_t take = std::min(kBlockSize - buffered_, size);
    std::memcpy(buffer_.data() + buffered_, input, take);
    buffered_ += take;
    input += take;
    size -= take;
    if (buffered_ == kBlockSize) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }

  while (size >= kBlockSize) {
    compress(input);
    input += kBlockSize;
    size -= kBlockSize;
  }

  if (size > 0) {
    std::memcpy(buffer_.data(), input, size);
    buffered_ = size;
  }
}

Sha1::Digest Sha1::finish() noexcept {
  // Identical padding rule to SHA-256: 0x80, zeros, then the length as a
  // 64-bit big-endian count of bits.
  const std::uint64_t bits = total_bits_;
  std::uint8_t padding[kBlockSize * 2] = {0x80};
  const std::size_t remainder = static_cast<std::size_t>((bits / 8) % kBlockSize);
  const std::size_t pad_len = (remainder < 56) ? (56 - remainder) : (120 - remainder);
  storeBe64(padding + pad_len, bits);
  update(padding, pad_len + 8);

  Digest digest{};
  for (int i = 0; i < 5; ++i) {
    storeBe32(digest.data() + 4 * i, state_[i]);
  }
  secureZero(buffer_.data(), buffer_.size());
  return digest;
}

Sha1::Digest Sha1::hash(const void* data, std::size_t size) noexcept {
  Sha1 sha;
  sha.update(data, size);
  return sha.finish();
}

Sha1::Digest Sha1::hash(std::string_view text) noexcept {
  return hash(text.data(), text.size());
}

Sha1::Digest hmacSha1(std::string_view key, std::string_view message) noexcept {
  std::uint8_t block_key[Sha1::kBlockSize] = {};
  if (key.size() > Sha1::kBlockSize) {
    const Sha1::Digest reduced = Sha1::hash(key);
    std::memcpy(block_key, reduced.data(), reduced.size());
  } else if (!key.empty()) {
    std::memcpy(block_key, key.data(), key.size());
  }

  std::uint8_t inner_pad[Sha1::kBlockSize];
  std::uint8_t outer_pad[Sha1::kBlockSize];
  for (std::size_t i = 0; i < Sha1::kBlockSize; ++i) {
    inner_pad[i] = static_cast<std::uint8_t>(block_key[i] ^ 0x36);
    outer_pad[i] = static_cast<std::uint8_t>(block_key[i] ^ 0x5c);
  }

  Sha1 inner;
  inner.update(inner_pad, sizeof(inner_pad));
  inner.update(message.data(), message.size());
  const Sha1::Digest inner_digest = inner.finish();

  Sha1 outer;
  outer.update(outer_pad, sizeof(outer_pad));
  outer.update(inner_digest.data(), inner_digest.size());
  const Sha1::Digest result = outer.finish();

  secureZero(block_key, sizeof(block_key));
  secureZero(inner_pad, sizeof(inner_pad));
  secureZero(outer_pad, sizeof(outer_pad));
  return result;
}

std::string toHex(const Sha1::Digest& digest) {
  return toHex(digest.data(), digest.size());
}

// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256 (RFC 8018 §5.2)
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> pbkdf2HmacSha256(std::string_view password, std::string_view salt,
                                           std::uint32_t iterations, std::size_t length) {
  std::vector<std::uint8_t> derived;
  derived.reserve(length);
  if (iterations == 0 || length == 0) {
    return derived;
  }

  std::uint32_t block_index = 1;
  while (derived.size() < length) {
    // U_1 = PRF(password, salt || INT_32_BE(i))
    std::string seed;
    seed.reserve(salt.size() + 4);
    seed.append(salt);
    seed.push_back(static_cast<char>((block_index >> 24) & 0xff));
    seed.push_back(static_cast<char>((block_index >> 16) & 0xff));
    seed.push_back(static_cast<char>((block_index >> 8) & 0xff));
    seed.push_back(static_cast<char>(block_index & 0xff));

    Sha256::Digest u = hmacSha256(password, seed);
    Sha256::Digest accumulator = u;

    // T_i = U_1 xor U_2 xor ... xor U_c. The chain is inherently serial --
    // that serialisation is the cost function, so there is nothing to
    // parallelise away here.
    for (std::uint32_t iteration = 1; iteration < iterations; ++iteration) {
      u = hmacSha256(password, std::string_view(reinterpret_cast<const char*>(u.data()), u.size()));
      for (std::size_t i = 0; i < accumulator.size(); ++i) {
        accumulator[i] ^= u[i];
      }
    }

    const std::size_t take = std::min(accumulator.size(), length - derived.size());
    derived.insert(derived.end(), accumulator.begin(), accumulator.begin() + static_cast<long>(take));
    ++block_index;
  }

  return derived;
}

// ---------------------------------------------------------------------------
// Encoding and comparison
// ---------------------------------------------------------------------------

bool constantTimeEquals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  // Volatile so the accumulation cannot be optimised into an early exit.
  volatile unsigned char difference = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    difference |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return difference == 0;
}

std::string toHex(const void* data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::string out;
  out.resize(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 0x0f];
  }
  return out;
}

std::string toHex(const Sha256::Digest& digest) {
  return toHex(digest.data(), digest.size());
}

Result<std::string> fromHex(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return Status::invalidArgument("hex string has an odd length");
  }
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return Status::invalidArgument("hex string contains a non-hex character");
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

Result<std::string> randomBytes(std::size_t count) {
  if (count == 0) {
    return std::string{};
  }
  std::ifstream urandom("/dev/urandom", std::ios::binary);
  if (!urandom) {
    return Status::ioError("cannot open /dev/urandom; refusing to generate weak secrets");
  }
  std::string out(count, '\0');
  urandom.read(out.data(), static_cast<std::streamsize>(count));
  if (static_cast<std::size_t>(urandom.gcount()) != count) {
    return Status::ioError("short read from /dev/urandom; refusing to generate weak secrets");
  }
  return out;
}

Result<std::string> randomToken(std::size_t entropy_bytes) {
  Result<std::string> bytes = randomBytes(entropy_bytes);
  if (!bytes.ok()) {
    return bytes.status();
  }
  return toHex(bytes.value().data(), bytes.value().size());
}

// ---------------------------------------------------------------------------
// Password hashing
// ---------------------------------------------------------------------------

namespace {

constexpr std::string_view kAlgorithm = "pbkdf2_sha256";
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kDerivedBytes = 32;

/// Splits `algorithm$iterations$salt$hash`. Returns false on any deviation --
/// a partially-parsed credential must never be treated as usable.
bool splitEncoded(std::string_view encoded, std::string_view& algorithm, std::uint32_t& iterations,
                  std::string_view& salt_hex, std::string_view& hash_hex) noexcept {
  const std::size_t a = encoded.find('$');
  if (a == std::string_view::npos)
    return false;
  const std::size_t b = encoded.find('$', a + 1);
  if (b == std::string_view::npos)
    return false;
  const std::size_t c = encoded.find('$', b + 1);
  if (c == std::string_view::npos)
    return false;
  if (encoded.find('$', c + 1) != std::string_view::npos)
    return false;

  algorithm = encoded.substr(0, a);
  const std::string_view iterations_text = encoded.substr(a + 1, b - a - 1);
  salt_hex = encoded.substr(b + 1, c - b - 1);
  hash_hex = encoded.substr(c + 1);

  if (iterations_text.empty() || iterations_text.size() > 9)
    return false;
  std::uint32_t parsed = 0;
  for (const char ch : iterations_text) {
    if (ch < '0' || ch > '9')
      return false;
    parsed = parsed * 10 + static_cast<std::uint32_t>(ch - '0');
  }
  if (parsed == 0)
    return false;
  iterations = parsed;
  return true;
}

}  // namespace

Result<std::string> hashPassword(std::string_view password, std::uint32_t iterations) {
  if (iterations == 0) {
    return Status::invalidArgument("iteration count must be positive");
  }
  Result<std::string> salt = randomBytes(kSaltBytes);
  if (!salt.ok()) {
    return salt.status();
  }
  const std::vector<std::uint8_t> derived =
      pbkdf2HmacSha256(password, salt.value(), iterations, kDerivedBytes);

  std::string encoded;
  encoded.append(kAlgorithm);
  encoded.push_back('$');
  encoded.append(std::to_string(iterations));
  encoded.push_back('$');
  encoded.append(toHex(salt.value().data(), salt.value().size()));
  encoded.push_back('$');
  encoded.append(toHex(derived.data(), derived.size()));
  return encoded;
}

Result<bool> verifyPassword(std::string_view password, std::string_view encoded) {
  std::string_view algorithm;
  std::string_view salt_hex;
  std::string_view hash_hex;
  std::uint32_t iterations = 0;
  if (!splitEncoded(encoded, algorithm, iterations, salt_hex, hash_hex)) {
    return Status::invalidArgument("malformed password hash");
  }
  if (algorithm != kAlgorithm) {
    return Status::unsupported("unsupported password hash algorithm: " + std::string(algorithm));
  }

  Result<std::string> salt = fromHex(salt_hex);
  if (!salt.ok()) {
    return salt.status();
  }
  Result<std::string> expected = fromHex(hash_hex);
  if (!expected.ok()) {
    return expected.status();
  }
  if (expected.value().empty()) {
    return Status::invalidArgument("password hash has an empty digest");
  }

  const std::vector<std::uint8_t> actual =
      pbkdf2HmacSha256(password, salt.value(), iterations, expected.value().size());
  return constantTimeEquals(expected.value(),
                            std::string_view(reinterpret_cast<const char*>(actual.data()), actual.size()));
}

bool needsRehash(std::string_view encoded, std::uint32_t desired) noexcept {
  std::string_view algorithm;
  std::string_view salt_hex;
  std::string_view hash_hex;
  std::uint32_t iterations = 0;
  if (!splitEncoded(encoded, algorithm, iterations, salt_hex, hash_hex)) {
    // Unparseable, so it cannot be verified either; re-hashing on next login
    // is the only path back to a usable credential.
    return true;
  }
  return algorithm != kAlgorithm || iterations < desired;
}

}  // namespace bourse::auth
