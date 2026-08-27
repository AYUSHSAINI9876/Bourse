#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bourse/auth/principal.hpp"
#include "bourse/core/result.hpp"

/// \file auth_service.hpp
/// Users, passwords, sessions and roles.
///
/// Sessions are held here rather than in the keyspace, even though the
/// keyspace is right there and has TTL support. Under `--maxmemory` with an
/// `allkeys-*` policy the keyspace evicts whatever it likes, so storing
/// sessions there would log users out at random under load -- and the harder
/// the demo is hit, the more often it would happen. Session storage needs a
/// different eviction rule from cache storage, so it gets its own store.

namespace bourse::auth {

struct AuthConfig {
  /// When false the service still works -- users can be added, logins issue
  /// tokens -- but nothing is *enforced*. This is what keeps a local run and
  /// the whole existing smoke suite working with no credentials.
  bool enabled = false;

  std::int64_t session_ttl_ms = 12 * 60 * 60 * 1000;  ///< 12 hours
  std::uint32_t iterations = 210000;

  /// After this many consecutive failures an account is locked for
  /// `lockout_ms`. PBKDF2 is deliberately expensive, so an unthrottled login
  /// endpoint is both a password oracle and a CPU exhaustion vector.
  int max_failures = 5;
  std::int64_t lockout_ms = 60 * 1000;

  /// Hard ceiling on live sessions, so a flood of logins cannot grow the
  /// process without bound.
  std::size_t max_sessions = 10000;
};

struct UserRecord {
  std::string username;
  Role role = Role::kViewer;
  std::int64_t created_at_ms = 0;
  /// Never populated by `listUsers`. Present so the store has one shape.
  std::string password_hash;

  /// True once enrolment has been confirmed with a working code.
  ///
  /// Separate from "a secret exists" on purpose: a secret is created the moment
  /// enrolment begins, and enabling on that alone would lock the account the
  /// instant someone opened the enrolment screen and closed it again.
  bool totp_enabled = false;
};

struct LoginResult {
  /// The only time the raw token exists outside the client. Not stored: the
  /// service keeps SHA-256(token), so a heap dump or a logged struct yields
  /// nothing a caller can replay.
  std::string token;
  std::string username;
  Role role = Role::kAnonymous;
  std::int64_t expires_at_ms = 0;
};

struct SessionInfo {
  /// Stable public handle for this session.
  ///
  /// Random and unrelated to the token, so listing sessions to their owner
  /// reveals nothing that could be replayed -- the token itself never leaves
  /// the client after login.
  std::string id;
  std::string username;
  Role role = Role::kAnonymous;
  std::int64_t created_at_ms = 0;
  std::int64_t expires_at_ms = 0;
  /// Whatever identified the caller at login: an address, or "unknown".
  std::string client_id;
  /// True for the session making the request, so a UI can label it "this
  /// device" and think twice before revoking it.
  bool current = false;
};

/// What `beginTotpEnrolment` hands back. The secret is shown exactly once.
struct TotpEnrolment {
  std::string base32_secret;
  std::string provisioning_uri;
};

/// Thread-safe. Every public method takes the lock; the server touches this
/// from the HTTP thread, every RESP I/O thread, and the background sweeper.
class AuthService {
 public:
  explicit AuthService(AuthConfig config = {});

  [[nodiscard]] bool enabled() const noexcept { return config_.enabled; }

  [[nodiscard]] const AuthConfig& config() const noexcept { return config_; }

  // -- users -------------------------------------------------------------

  /// Fails if the user exists. Username rules are deliberately strict
  /// (alphanumeric, `_`, `-`, `.`, 1..64) so a name can never collide with
  /// protocol syntax or turn up in a log line as something else.
  Status addUser(std::string_view username, std::string_view password, Role role);
  Status setPassword(std::string_view username, std::string_view password);
  Status setRole(std::string_view username, Role role);

  /// Removing a user also revokes their live sessions. Leaving them valid is
  /// the classic mistake: the account is gone from the list and still works.
  Status removeUser(std::string_view username);

  [[nodiscard]] std::vector<UserRecord> listUsers() const;
  [[nodiscard]] bool hasUser(std::string_view username) const;
  [[nodiscard]] std::size_t userCount() const;

  // -- authentication ----------------------------------------------------

  /// Verifies credentials and issues a session token.
  ///
  /// `client_id` is whatever identifies the caller for rate limiting -- a peer
  /// address for RESP, `X-Forwarded-For` or the socket address for HTTP. It is
  /// only used for throttling.
  /// `totp_code` may be empty. When the account has two-factor enabled and no
  /// code is supplied this fails with a distinct message -- "two-factor code
  /// required" -- rather than looking like a wrong password, so the UI can ask
  /// for the code instead of telling the user their password is wrong.
  Result<LoginResult> login(std::string_view username, std::string_view password, std::string_view client_id,
                            std::string_view totp_code = {});

  /// Credential check with no token issued, for RESP `AUTH`, where the
  /// connection itself carries the authenticated state.
  Result<Principal> verifyCredentials(std::string_view username, std::string_view password,
                                      std::string_view client_id);

  /// Resolves a bearer token. Expired sessions are removed as they are found,
  /// which is the same lazy-expiry approach the keyspace uses for TTLs.
  Result<Principal> authenticate(std::string_view token);

  [[nodiscard]] Result<SessionInfo> describeSession(std::string_view token);

  /// Every live session for one account, newest first.
  ///
  /// `current_token` marks the caller's own session rather than filtering it
  /// out: "sign out everywhere except here" is the operation people actually
  /// want, and it needs the current one identified, not hidden.
  [[nodiscard]] std::vector<SessionInfo> listSessions(std::string_view username,
                                                      std::string_view current_token = {});

  /// Revokes one session by its public id. Returns false if it is not this
  /// user's -- checked rather than assumed, or any signed-in user could revoke
  /// anyone else's sessions by guessing ids.
  bool revokeSessionById(std::string_view username, std::string_view session_id);

  /// Revokes every session for the account except the one presenting
  /// `keep_token`. Pass an empty token to revoke all of them.
  std::size_t revokeOtherSessions(std::string_view username, std::string_view keep_token);

  // -- two-factor --------------------------------------------------------

  /// Starts enrolment: generates a secret and returns it with the
  /// `otpauth://` URI. Nothing is enforced until `confirmTotpEnrolment`.
  Result<TotpEnrolment> beginTotpEnrolment(std::string_view username, std::string_view issuer);

  /// Finishes enrolment once the user proves their app is generating matching
  /// codes. Requiring that proof is what stops an account being locked out by
  /// a mistyped secret or a phone with a wrong clock.
  Status confirmTotpEnrolment(std::string_view username, std::string_view code);

  /// Turning 2FA off requires the current password: otherwise anyone who finds
  /// an unlocked session can quietly remove the second factor.
  Status disableTotp(std::string_view username, std::string_view password);

  [[nodiscard]] bool totpEnabled(std::string_view username) const;

  bool logout(std::string_view token);
  std::size_t revokeSessionsFor(std::string_view username);

  /// Active-expiry pass, called periodically by the server. Lazy expiry alone
  /// leaks memory for sessions nobody ever presents again.
  std::size_t sweepExpired(std::int64_t now_ms);

  [[nodiscard]] std::size_t sessionCount() const;

  /// Test seam: the clock. Sessions and lockouts are time-driven, and a test
  /// that has to sleep for twelve hours is a test nobody runs.
  void setClockForTesting(std::int64_t (*clock)()) noexcept { clock_ = clock; }

 private:
  struct Session {
    std::string id;
    std::string username;
    Role role = Role::kAnonymous;
    std::int64_t created_at_ms = 0;
    std::int64_t expires_at_ms = 0;
    std::string client_id;
  };

  struct FailureRecord {
    int consecutive = 0;
    std::int64_t locked_until_ms = 0;
    /// When the most recent failure happened.
    ///
    /// Needed for the sweep. Without it, a record sitting below the lockout
    /// threshold -- one or two failures and no lockout -- has nothing to age
    /// out on, so scattered failed attempts against many usernames accumulate
    /// entries forever. That is a slow memory leak an attacker controls.
    std::int64_t last_failure_ms = 0;
  };

  [[nodiscard]] std::int64_t now() const noexcept;

  /// Requires `mutex_`. Returns nullptr when absent or expired.
  const Session* findSessionLocked(const std::string& token_hash, std::int64_t now_ms) const;

  /// Requires `mutex_`. Applies the lockout policy and returns the error to
  /// hand back, or an ok status when the caller may proceed.
  Status checkThrottleLocked(const std::string& key, std::int64_t now_ms) const;
  void recordFailureLocked(const std::string& key, std::int64_t now_ms);
  void clearFailuresLocked(const std::string& key);

  mutable std::mutex mutex_;
  AuthConfig config_;
  std::unordered_map<std::string, UserRecord> users_;

  /// TOTP state, keyed by username and held apart from UserRecord so that
  /// listUsers() cannot leak a secret even by accident: the type it returns
  /// simply has nowhere to put one.
  struct TotpState {
    std::string secret;  ///< raw bytes, not base32
    bool enabled = false;
    /// Highest time step already authenticated with. A code at or below this
    /// is refused, which is what stops an observed code being replayed inside
    /// its own window.
    std::int64_t last_step = -1;
  };

  std::unordered_map<std::string, TotpState> totp_;
  /// Keyed by hex(SHA-256(token)), never by the token itself.
  std::unordered_map<std::string, Session> sessions_;
  std::unordered_map<std::string, FailureRecord> failures_;

  /// A real PBKDF2 hash of a fixed string, verified against on every login for
  /// an unknown username. Without it, "no such user" returns in microseconds
  /// while a wrong password takes ~100 ms, and that gap enumerates the user
  /// list from the outside.
  std::string decoy_hash_;

  std::int64_t (*clock_)() = nullptr;
};

/// Validates a username against the rules `addUser` enforces. Exposed so the
/// REST layer can reject early with a useful message.
[[nodiscard]] Status validateUsername(std::string_view username);
/// Minimum length and a ban on the obvious. Not a strength meter.
[[nodiscard]] Status validatePassword(std::string_view password);

}  // namespace bourse::auth
