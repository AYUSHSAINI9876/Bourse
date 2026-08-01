// A thin CLI over the crypto primitives, so they can be compared against an
// independent implementation.
//
// The unit tests pin the published standard vectors, which proves the common
// cases. This exists for the other half of the argument: scripts/verify-crypto.sh
// drives it with thousands of randomised inputs -- odd lengths, empty keys,
// keys longer than the block size, multi-block derived keys -- and diffs the
// output against Python's hashlib. Agreement on random inputs is what turns
// "matches the vectors" into "matches the reference".
//
// Reads one request per line and writes one hex digest per line:
//
//   sha256 <hex-message>
//   hmac   <hex-key> <hex-message>
//   pbkdf2 <hex-password> <hex-salt> <iterations> <length>
//
// Hex in, hex out, so a NUL byte or invalid UTF-8 in a password is exercised
// rather than mangled by the shell.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "bourse/auth/crypto.hpp"

namespace {

using bourse::Result;
using bourse::auth::fromHex;
using bourse::auth::hmacSha256;
using bourse::auth::pbkdf2HmacSha256;
using bourse::auth::Sha256;
using bourse::auth::toHex;

/// Decodes a hex argument, reporting the line rather than dying silently: a
/// verifier that skips malformed input while reporting success is worse than
/// no verifier.
///
/// `-` means the empty string. An empty field cannot be written literally in a
/// whitespace-separated line -- it vanishes, and the *next* field silently
/// slides into its place. For `hmac <key> <message>` that would compare
/// HMAC(message, "") against HMAC("", message) and call it a pass.
bool decode(const std::string& hex, std::string& out, const std::string& line) {
  if (hex == "-") {
    out.clear();
    return true;
  }
  Result<std::string> decoded = fromHex(hex);
  if (!decoded.ok()) {
    std::fprintf(stderr, "crypto-check: bad hex in: %s\n", line.c_str());
    return false;
  }
  out = std::move(decoded).value();
  return true;
}

}  // namespace

int main() {
  std::ios::sync_with_stdio(false);
  std::string line;

  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream fields(line);
    std::string operation;
    fields >> operation;

    if (operation == "sha256") {
      std::string message_hex;
      fields >> message_hex;
      std::string message;
      if (!decode(message_hex, message, line)) {
        return 1;
      }
      std::cout << toHex(Sha256::hash(message)) << '\n';

    } else if (operation == "hmac") {
      std::string key_hex;
      std::string message_hex;
      fields >> key_hex >> message_hex;
      std::string key;
      std::string message;
      if (!decode(key_hex, key, line) || !decode(message_hex, message, line)) {
        return 1;
      }
      std::cout << toHex(hmacSha256(key, message)) << '\n';

    } else if (operation == "pbkdf2") {
      std::string password_hex;
      std::string salt_hex;
      unsigned long iterations = 0;
      unsigned long length = 0;
      fields >> password_hex >> salt_hex >> iterations >> length;
      std::string password;
      std::string salt;
      if (!decode(password_hex, password, line) || !decode(salt_hex, salt, line)) {
        return 1;
      }
      const std::vector<std::uint8_t> derived = pbkdf2HmacSha256(
          password, salt, static_cast<std::uint32_t>(iterations), static_cast<std::size_t>(length));
      std::cout << toHex(derived.data(), derived.size()) << '\n';

    } else {
      std::fprintf(stderr, "crypto-check: unknown operation '%s'\n", operation.c_str());
      return 1;
    }
  }
  return 0;
}
