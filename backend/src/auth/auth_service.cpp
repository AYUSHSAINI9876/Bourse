#include "bourse/auth/auth_service.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "bourse/auth/crypto.hpp"
#include "bourse/auth/totp.hpp"
#include "bourse/core/clock.hpp"
#include "bourse/core/logger.hpp"

namespace bourse::auth {
namespace {

/// Sessions are keyed by the hash of the token, so the raw token exists only
/// in transit and in the client. Stealing the server's session table gets you
/// a list of hashes you cannot present.
std::string tokenKey(std::string_view token) {
  return toHex(Sha256::hash(token));
}

constexpr std::size_t kMaxUsernameLength = 64;
constexpr std::size_t kMinPasswordLength = 8;
constexpr std::size_t kMaxPasswordLength = 256;

/// A handful of passwords that show up in every credential-stuffing list.
/// Not a substitute for a real strength policy -- it exists so a demo
/// deployment cannot be seeded with `password`.
constexpr std::string_view kBannedPasswords[] = {
    "password", "password1", "12345678", "123456789", "qwertyui",
    "letmein1", "admin123",  "changeme", "bourse123", "iloveyou",
};

}  // namespace

Status validateUsername(std::string_view username) {
  if (username.empty()) {
    return Status::invalidArgument("username must not be empty");
  }
  if (username.size() > kMaxUsernameLength) {
    return Status::invalidArgument("username must be at most 64 characters");
  }
  for (const char c : username) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                         c == '_' || c == '-' || c == '.';
    if (!allowed) {
      return Status::invalidArgument("username may only contain letters, digits, '_', '-' and '.'");
    }
  }
  return Status::success();
}

Status validatePassword(std::string_view password) {
  if (password.size() < kMinPasswordLength) {
    return Status::invalidArgument("password must be at least 8 characters");
  }
  if (password.size() > kMaxPasswordLength) {
    // Not arbitrary: PBKDF2 cost is independent of password length, but an
    // unbounded field is still an unbounded allocation per login attempt.
    return Status::invalidArgument("password must be at most 256 characters");
  }
  for (const std::string_view banned : kBannedPasswords) {
    if (password == banned) {
      return Status::invalidArgument("password is on the list of most-guessed passwords");
    }
  }
  return Status::success();
}

AuthService::AuthService(AuthConfig config) : config_(config) {
  // Built once at startup so the per-login cost of the decoy path matches the
  // real path exactly, including the iteration count.
  Result<std::string> decoy = hashPassword("bourse-decoy-credential", config_.iterations);
  if (decoy.ok()) {
    decoy_hash_ = std::move(decoy).value();
  }
}

std::int64_t AuthService::now() const noexcept {
  return clock_ != nullptr ? clock_() : nowMillis();
}

// ---------------------------------------------------------------------------
// Users
// ---------------------------------------------------------------------------

Status AuthService::addUser(std::string_view username, std::string_view password, Role role) {
  if (const Status valid = validateUsername(username); !valid.ok()) {
    return valid;
  }
  if (const Status valid = validatePassword(password); !valid.ok()) {
    return valid;
  }
  if (role == Role::kAnonymous) {
    return Status::invalidArgument("cannot create a user with the anonymous role");
  }

  Result<std::string> hash = hashPassword(password, config_.iterations);
  if (!hash.ok()) {
    return hash.status();
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  const std::string key(username);
  if (users_.find(key) != users_.end()) {
    return Status::alreadyExists("user already exists: " + key);
  }
  UserRecord record;
  record.username = key;
  record.role = role;
  record.created_at_ms = now();
  record.password_hash = std::move(hash).value();
  users_.emplace(key, std::move(record));
  return Status::success();
}

Status AuthService::setPassword(std::string_view username, std::string_view password) {
  if (const Status valid = validatePassword(password); !valid.ok()) {
    return valid;
  }
  Result<std::string> hash = hashPassword(password, config_.iterations);
  if (!hash.ok()) {
    return hash.status();
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = users_.find(std::string(username));
  if (it == users_.end()) {
    return Status::notFound("no such user: " + std::string(username));
  }
  it->second.password_hash = std::move(hash).value();

  // A password change must invalidate existing sessions. Otherwise the reason
  // people change passwords -- someone else has one -- is not addressed.
  for (auto session = sessions_.begin(); session != sessions_.end();) {
    session =
        (session->second.username == it->second.username) ? sessions_.erase(session) : std::next(session);
  }
  failures_.erase(std::string(username));
  return Status::success();
}

Status AuthService::setRole(std::string_view username, Role role) {
  if (role == Role::kAnonymous) {
    return Status::invalidArgument("cannot assign the anonymous role");
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = users_.find(std::string(username));
  if (it == users_.end()) {
    return Status::notFound("no such user: " + std::string(username));
  }
  it->second.role = role;
  // Live sessions carry a copy of the role, so they have to be refreshed for
  // a demotion to take effect immediately rather than at next login.
  for (auto& [ignored, session] : sessions_) {
    if (session.username == it->second.username) {
      session.role = role;
    }
  }
  return Status::success();
}

Status AuthService::removeUser(std::string_view username) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = users_.find(std::string(username));
  if (it == users_.end()) {
    return Status::notFound("no such user: " + std::string(username));
  }
  const std::string removed = it->second.username;
  users_.erase(it);
  totp_.erase(removed);
  for (auto session = sessions_.begin(); session != sessions_.end();) {
    session = (session->second.username == removed) ? sessions_.erase(session) : std::next(session);
  }
  failures_.erase(removed);
  return Status::success();
}

std::vector<UserRecord> AuthService::listUsers() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<UserRecord> out;
  out.reserve(users_.size());
  for (const auto& [ignored, record] : users_) {
    // Password hashes are stripped rather than trusted not to be printed.
    // The caller is a REST handler; the hash has no business leaving here.
    UserRecord copy;
    copy.username = record.username;
    copy.role = record.role;
    copy.created_at_ms = record.created_at_ms;
    // Whether a second factor is on is exactly the sort of thing an admin
    // audits; the shared secret lives in a side table, so saying so here
    // cannot leak it.
    copy.totp_enabled = record.totp_enabled;
    out.push_back(std::move(copy));
  }
  std::sort(out.begin(), out.end(),
            [](const UserRecord& a, const UserRecord& b) { return a.username < b.username; });
  return out;
}

bool AuthService::hasUser(std::string_view username) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return users_.find(std::string(username)) != users_.end();
}

std::size_t AuthService::userCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return users_.size();
}

// ---------------------------------------------------------------------------
// Throttling
// ---------------------------------------------------------------------------

Status AuthService::checkThrottleLocked(const std::string& key, std::int64_t now_ms) const {
  const auto it = failures_.find(key);
  if (it == failures_.end()) {
    return Status::success();
  }
  if (now_ms < it->second.locked_until_ms) {
    const std::int64_t remaining = (it->second.locked_until_ms - now_ms + 999) / 1000;
    return Status(ErrorCode::kUnsupported,
                  "too many failed attempts; retry in " + std::to_string(remaining) + "s");
  }
  return Status::success();
}

void AuthService::recordFailureLocked(const std::string& key, std::int64_t now_ms) {
  FailureRecord& record = failures_[key];
  if (now_ms >= record.locked_until_ms && record.locked_until_ms != 0) {
    // The previous lockout has elapsed; start counting again rather than
    // locking out instantly on the first attempt after it expires.
    record.consecutive = 0;
    record.locked_until_ms = 0;
  }
  record.last_failure_ms = now_ms;
  ++record.consecutive;
  if (record.consecutive >= config_.max_failures) {
    record.locked_until_ms = now_ms + config_.lockout_ms;
    record.consecutive = 0;
  }
}

void AuthService::clearFailuresLocked(const std::string& key) {
  failures_.erase(key);
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

Result<Principal> AuthService::verifyCredentials(std::string_view username, std::string_view password,
                                                 std::string_view client_id) {
  const std::int64_t now_ms = now();
  std::string stored_hash;
  Role role = Role::kAnonymous;
  std::string resolved_username;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    // Both the account and the source address are throttled. Per-account
    // alone lets one host spray the whole user list; per-address alone lets a
    // botnet grind a single account.
    const std::string user_key = "user:" + std::string(username);
    const std::string client_key = "client:" + std::string(client_id);
    if (const Status throttled = checkThrottleLocked(user_key, now_ms); !throttled.ok()) {
      return throttled;
    }
    if (!client_id.empty()) {
      if (const Status throttled = checkThrottleLocked(client_key, now_ms); !throttled.ok()) {
        return throttled;
      }
    }

    const auto it = users_.find(std::string(username));
    if (it != users_.end()) {
      stored_hash = it->second.password_hash;
      role = it->second.role;
      resolved_username = it->second.username;
    }
  }

  // PBKDF2 runs outside the lock -- it is ~100 ms of pure CPU, and holding the
  // mutex across it would serialise every other auth operation behind each
  // login attempt.
  //
  // When the user does not exist, the decoy hash is verified instead of
  // returning early, so both paths cost the same and neither reveals whether
  // the username is real.
  const bool user_exists = !stored_hash.empty();
  const std::string_view hash_to_check =
      user_exists ? std::string_view(stored_hash) : std::string_view(decoy_hash_);
  bool matched = false;
  if (!hash_to_check.empty()) {
    const Result<bool> verified = verifyPassword(password, hash_to_check);
    if (!verified.ok()) {
      if (user_exists) {
        BOURSE_LOG_ERROR("stored password hash for '", resolved_username,
                         "' is unusable: ", verified.status().message());
      }
      matched = false;
    } else {
      matched = verified.value() && user_exists;
    }
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string user_key = "user:" + std::string(username);
    const std::string client_key = "client:" + std::string(client_id);
    if (!matched) {
      recordFailureLocked(user_key, now_ms);
      if (!client_id.empty()) {
        recordFailureLocked(client_key, now_ms);
      }
      // Deliberately identical for "no such user" and "wrong password".
      return Status(ErrorCode::kInvalidArgument, "invalid username or password");
    }
    clearFailuresLocked(user_key);
    if (!client_id.empty()) {
      clearFailuresLocked(client_key);
    }
  }

  Principal principal;
  principal.username = resolved_username;
  principal.role = role;
  return principal;
}

Result<LoginResult> AuthService::login(std::string_view username, std::string_view password,
                                       std::string_view client_id, std::string_view totp_code) {
  Result<Principal> principal = verifyCredentials(username, password, client_id);
  if (!principal.ok()) {
    return principal.status();
  }

  // The second factor is checked only after the password, so a wrong password
  // never reveals whether the account has 2FA enabled.
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = totp_.find(principal.value().username);
    if (it != totp_.end() && it->second.enabled) {
      if (totp_code.empty()) {
        // Deliberately distinct from a credential failure: the client needs to
        // know to ask for a code, not to tell the user their password is wrong.
        return Status(ErrorCode::kInvalidArgument, "two-factor code required");
      }
      const TotpVerification check =
          verifyTotp(it->second.secret, totp_code, now() / 1000, it->second.last_step);
      if (!check.accepted) {
        return Status(ErrorCode::kInvalidArgument, "invalid two-factor code");
      }
      // Remember the step so the same code cannot be presented twice.
      it->second.last_step = check.step;
    }
  }

  Result<std::string> token = randomToken(32);
  if (!token.ok()) {
    return token.status();
  }

  Result<std::string> session_id = randomToken(8);
  if (!session_id.ok()) {
    return session_id.status();
  }

  const std::int64_t now_ms = now();
  Session session;
  session.id = std::move(session_id).value();
  session.username = principal.value().username;
  session.role = principal.value().role;
  session.created_at_ms = now_ms;
  session.expires_at_ms = now_ms + config_.session_ttl_ms;
  session.client_id = std::string(client_id);

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.size() >= config_.max_sessions) {
      // Try to make room from genuinely dead entries before refusing. Only if
      // the table is full of *live* sessions is this a real capacity problem.
      for (auto it = sessions_.begin(); it != sessions_.end();) {
        it = (it->second.expires_at_ms <= now_ms) ? sessions_.erase(it) : std::next(it);
      }
      if (sessions_.size() >= config_.max_sessions) {
        return Status(ErrorCode::kOutOfMemory, "session table is full; try again shortly");
      }
    }
    sessions_.emplace(tokenKey(token.value()), std::move(session));
  }

  LoginResult result;
  result.token = std::move(token).value();
  result.username = principal.value().username;
  result.role = principal.value().role;
  result.expires_at_ms = now_ms + config_.session_ttl_ms;
  return result;
}

const AuthService::Session* AuthService::findSessionLocked(const std::string& token_hash,
                                                           std::int64_t now_ms) const {
  const auto it = sessions_.find(token_hash);
  if (it == sessions_.end() || it->second.expires_at_ms <= now_ms) {
    return nullptr;
  }
  return &it->second;
}

Result<Principal> AuthService::authenticate(std::string_view token) {
  if (token.empty()) {
    return Status(ErrorCode::kInvalidArgument, "missing session token");
  }
  const std::string key = tokenKey(token);
  const std::int64_t now_ms = now();

  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sessions_.find(key);
  if (it == sessions_.end()) {
    return Status(ErrorCode::kInvalidArgument, "invalid or expired session");
  }
  if (it->second.expires_at_ms <= now_ms) {
    sessions_.erase(it);
    return Status(ErrorCode::kInvalidArgument, "invalid or expired session");
  }

  Principal principal;
  principal.username = it->second.username;
  principal.role = it->second.role;
  return principal;
}

Result<SessionInfo> AuthService::describeSession(std::string_view token) {
  const std::string key = tokenKey(token);
  const std::int64_t now_ms = now();
  const std::lock_guard<std::mutex> lock(mutex_);
  const Session* session = findSessionLocked(key, now_ms);
  if (session == nullptr) {
    return Status(ErrorCode::kInvalidArgument, "invalid or expired session");
  }
  SessionInfo info;
  info.username = session->username;
  info.role = session->role;
  info.created_at_ms = session->created_at_ms;
  info.expires_at_ms = session->expires_at_ms;
  return info;
}

std::vector<SessionInfo> AuthService::listSessions(std::string_view username,
                                                   std::string_view current_token) {
  const std::int64_t now_ms = now();
  const std::string current_key = current_token.empty() ? std::string() : tokenKey(current_token);

  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SessionInfo> out;
  for (const auto& [key, session] : sessions_) {
    if (session.username != username || session.expires_at_ms <= now_ms) {
      continue;
    }
    SessionInfo info;
    info.id = session.id;
    info.username = session.username;
    info.role = session.role;
    info.created_at_ms = session.created_at_ms;
    info.expires_at_ms = session.expires_at_ms;
    info.client_id = session.client_id;
    info.current = !current_key.empty() && key == current_key;
    out.push_back(std::move(info));
  }
  // Newest first: the session someone wants to revoke is almost always one
  // they do not recognise, and those are the recent ones.
  std::sort(out.begin(), out.end(),
            [](const SessionInfo& a, const SessionInfo& b) { return a.created_at_ms > b.created_at_ms; });
  return out;
}

bool AuthService::revokeSessionById(std::string_view username, std::string_view session_id) {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
    // The username is part of the match, not an assumption. Without it any
    // signed-in user could revoke anyone else's session by guessing an id.
    if (it->second.id == session_id && it->second.username == username) {
      sessions_.erase(it);
      return true;
    }
  }
  return false;
}

std::size_t AuthService::revokeOtherSessions(std::string_view username, std::string_view keep_token) {
  const std::string keep = keep_token.empty() ? std::string() : tokenKey(keep_token);

  const std::lock_guard<std::mutex> lock(mutex_);
  std::size_t removed = 0;
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->second.username == username && (keep.empty() || it->first != keep)) {
      it = sessions_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

// ---------------------------------------------------------------------------
// Two-factor
// ---------------------------------------------------------------------------

Result<TotpEnrolment> AuthService::beginTotpEnrolment(std::string_view username, std::string_view issuer) {
  Result<std::string> base32 = generateTotpSecret();
  if (!base32.ok()) {
    return base32.status();
  }
  Result<std::string> raw = base32Decode(base32.value());
  if (!raw.ok()) {
    return raw.status();
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  const auto user = users_.find(std::string(username));
  if (user == users_.end()) {
    return Status::notFound("no such user: " + std::string(username));
  }

  // Re-enrolling on an account that already has 2FA would replace a working
  // secret and, because login enforces on `enabled`, switch 2FA off -- without
  // the password check disableTotp() deliberately requires. That would leave
  // one live session able to strip the second factor, so turning it off stays
  // a single password-guarded path.
  const auto existing = totp_.find(std::string(username));
  if (existing != totp_.end() && existing->second.enabled) {
    return Status(ErrorCode::kInvalidArgument,
                  "two-factor is already enabled; disable it first to enrol a new device");
  }

  // Stored but not enabled. Enabling on a secret nobody has proved they can
  // generate codes from is how an account gets locked out by a typo.
  TotpState& state = totp_[std::string(username)];
  state.secret = std::move(raw).value();
  state.enabled = false;
  state.last_step = -1;

  TotpEnrolment out;
  out.base32_secret = base32.value();
  out.provisioning_uri = totpProvisioningUri(issuer, username, base32.value());
  return out;
}

Status AuthService::confirmTotpEnrolment(std::string_view username, std::string_view code) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto user = users_.find(std::string(username));
  if (user == users_.end()) {
    return Status::notFound("no such user: " + std::string(username));
  }
  const auto it = totp_.find(std::string(username));
  if (it == totp_.end() || it->second.secret.empty()) {
    return Status::invalidArgument("start enrolment before confirming it");
  }

  const TotpVerification check = verifyTotp(it->second.secret, code, now() / 1000, it->second.last_step);
  if (!check.accepted) {
    return Status::invalidArgument("that code does not match; check your device's clock");
  }

  it->second.enabled = true;
  it->second.last_step = check.step;
  user->second.totp_enabled = true;
  BOURSE_LOG_INFO("two-factor enabled for '", username, "'");
  return Status::success();
}

Status AuthService::disableTotp(std::string_view username, std::string_view password) {
  std::string stored;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto user = users_.find(std::string(username));
    if (user == users_.end()) {
      return Status::notFound("no such user: " + std::string(username));
    }
    stored = user->second.password_hash;
  }

  // Re-checking the password outside the lock, because PBKDF2 is ~100ms and
  // holding the mutex across it would stall every other auth operation.
  const Result<bool> verified = verifyPassword(password, stored);
  if (!verified.ok() || !verified.value()) {
    return Status(ErrorCode::kInvalidArgument, "password does not match");
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  totp_.erase(std::string(username));
  const auto user = users_.find(std::string(username));
  if (user != users_.end()) {
    user->second.totp_enabled = false;
  }
  BOURSE_LOG_WARN("two-factor disabled for '", username, "'");
  return Status::success();
}

bool AuthService::totpEnabled(std::string_view username) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = totp_.find(std::string(username));
  return it != totp_.end() && it->second.enabled;
}

bool AuthService::logout(std::string_view token) {
  const std::string key = tokenKey(token);
  const std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.erase(key) > 0;
}

std::size_t AuthService::revokeSessionsFor(std::string_view username) {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::size_t removed = 0;
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->second.username == username) {
      it = sessions_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

std::size_t AuthService::sweepExpired(std::int64_t now_ms) {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::size_t removed = 0;
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->second.expires_at_ms <= now_ms) {
      it = sessions_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  // Failure records are small but unbounded in count, and an attacker chooses
  // how many exist: one failed attempt against each of a million usernames
  // creates a million entries. Both shapes have to age out --
  //
  //   * a lockout that has elapsed, and
  //   * a partial count that never reached the threshold, which has no lockout
  //     to expire and so would otherwise live forever.
  //
  // The retention window is generous relative to the lockout so that a
  // genuinely-throttled attacker cannot reset their own counter by waiting for
  // the sweep.
  const std::int64_t retention_ms = std::max<std::int64_t>(config_.lockout_ms * 10, 60 * 1000);
  for (auto it = failures_.begin(); it != failures_.end();) {
    const bool lockout_elapsed = it->second.locked_until_ms != 0 && now_ms >= it->second.locked_until_ms;
    const bool gone_quiet = now_ms - it->second.last_failure_ms >= retention_ms;
    it = (lockout_elapsed && gone_quiet) || (it->second.locked_until_ms == 0 && gone_quiet)
             ? failures_.erase(it)
             : std::next(it);
  }
  return removed;
}

std::size_t AuthService::sessionCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

}  // namespace bourse::auth
