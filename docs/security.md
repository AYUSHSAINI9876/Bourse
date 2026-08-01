# Security model

What Bourse's authentication actually guarantees, what it deliberately does
not, and why each choice was made. Written to be argued with — every claim
below is either enforced by a test or listed as a known limitation.

---

## Threat model

Bourse is a portfolio and demo system that can be exposed on the public
internet. The threats it is built to resist:

| Threat | Defence |
|---|---|
| Anyone finding the URL and running `FLUSHALL` | Role-based access control, deny by default |
| Password list stuffed against the login endpoint | PBKDF2 work factor + per-account and per-address lockout |
| Password database stolen | Per-credential salt, 210,000-iteration PBKDF2 |
| Session table stolen from memory or a log | Only SHA-256(token) is stored; raw tokens exist solely in transit |
| Username enumeration | Identical error text and identical cost for "no such user" and "wrong password" |
| Token guessing | 256 bits of CSPRNG entropy per token |
| Timing attacks on token/hash comparison | Constant-time comparison |
| A new endpoint added without a permission check | Allowlist of public paths; everything else denied |

Explicitly **out of scope**: a determined attacker with local access to the
server process, side-channel attacks against the PBKDF2 implementation, and
denial of service beyond the login endpoint.

---

## Roles

Four levels, ordered, so a permission check is one numeric comparison:

| Role | Can |
|---|---|
| `anonymous` | `PING`, `QUIT`, `AUTH`, `WHOAMI`, `GET /`, `GET /health`, `GET /api/auth/me`, `POST /api/auth/login` |
| `viewer` | Everything above, plus every read: `GET`, `BOOK`, `TRADES`, `SELECT`, `/api/stats` |
| `trader` | Everything above, plus writes: `SET`, `ORDER`, `CANCEL`, `INSERT`, `UPDATE` |
| `admin` | Everything above, plus `FLUSHALL`, `CONFIG`, `USER`, `/api/auth/users` |

```cpp
enum class Role : std::uint8_t { kAnonymous = 0, kViewer = 1, kTrader = 2, kAdmin = 3 };
bool can(Role required) const noexcept { return role >= required; }
```

Ordering the enum rather than keeping a set of booleans is what keeps the whole
check to one line in one place.

### Where it is enforced

**One place**, for both protocols — `CommandRegistry::dispatch`:

```cpp
if (context.server.auth != nullptr && context.server.auth->enabled()) {
  if (!context.principal.can(command->requiredRole())) {
    return context.principal.authenticated()
        ? Reply::error("NOPERM …") : Reply::error("NOAUTH …");
  }
}
```

This is the same design as the write-ahead journal hook. A verb added tomorrow
is covered automatically, and RESP and REST cannot drift apart on what a role
may do, because there is only one decision behind both.

The required role is *derived*, never set by hand:

```cpp
auth::Role requiredRole() const noexcept {
  if (isNoAuth()) return auth::Role::kAnonymous;
  if (isAdmin())  return auth::Role::kAdmin;
  return isWrite() ? auth::Role::kTrader : auth::Role::kViewer;
}
```

The default is the safe one and a verb opts *down*. `backend/tests/test_auth.cpp` asserts
that the set of commands reaching `kAnonymous` is exactly `{PING, QUIT, AUTH,
WHOAMI}` — adding a fifth fails the build's test step rather than quietly
opening a hole.

`NOAUTH` and `NOPERM` are kept distinct on purpose: the first tells a client to
authenticate and retry, the second tells it that retrying will not help.
Collapsing them sends clients into login loops.

---

## Passwords

Stored as `pbkdf2_sha256$<iterations>$<salt_hex>$<hash_hex>` — the format
Django and passlib use. Self-describing, so the cost factor can be raised later
without invalidating existing credentials, and `needsRehash()` can report which
stored hashes are stale.

- **PBKDF2-HMAC-SHA256**, 210,000 iterations by default.
- **16 random bytes of salt** per credential, from `/dev/urandom`.
- **32-byte derived key**, compared in constant time.

### Why 210,000 and not OWASP's 600,000

600,000 costs roughly a second of CPU per attempt on one shared core of a
free-tier container. That turns the login endpoint into its own denial of
service: a handful of concurrent attempts saturate the instance. 210,000 with a
lockout in front of it is the trade this deployment makes. `--auth-iterations`
raises it where the CPU exists.

### Why the crypto is implemented here rather than linked

The usual advice — "don't write your own crypto" — warns against *inventing* a
scheme. SHA-256, HMAC and PBKDF2 are published standards (FIPS 180-4, RFC 2104,
RFC 8018) whose correctness is **decidable**:

1. `test_auth.cpp` checks every primitive against the vectors published in
   those documents.
2. `backend/scripts/verify-crypto.sh` cross-checks all three against **Python's
   hashlib** over hundreds of randomised inputs — including the shapes that
   break naive implementations: messages at 55/56/63/64/65 bytes where SHA-256
   padding branches, HMAC keys longer than the block size, multi-block derived
   keys, and empty keys and salts.

```
$ bash backend/scripts/verify-crypto.sh
=== Cross-checking crypto against Python hashlib (400 cases) ===
  generated 850 cases
  850/850 digests match Python hashlib
  PASS
```

That is the whole argument: an implementation that agrees with an independent
reference on random input is either right, or wrong in a way that is visible.
That is not true of a scheme someone made up.

**The honest limitation:** this is a straightforward implementation with no
cache-timing hardening. Password comparison is constant-time and PBKDF2's cost
is dominated by its iteration count rather than data-dependent branching, so
the realistic exposure is low — but a deployment holding *real* user accounts
should link libsodium and use Argon2id. That is a one-file change behind
`hashPassword`/`verifyPassword`.

---

## Sessions

- **256 bits** of entropy per token, from `/dev/urandom`. A short read is a hard
  error, never a silent fallback — silently degrading a CSPRNG produces
  guessable tokens and nothing else looks wrong.
- Stored keyed by **`hex(SHA-256(token))`**, never by the token. Stealing the
  session table yields hashes that cannot be presented.
- 12-hour default TTL, `--session-ttl` to change.
- **Lazy expiry** on lookup plus an **active sweep** on the background cron.
  Lazy alone leaks: a session nobody presents again is never noticed.
- Changing a password or deleting a user **revokes that user's live sessions**.
  Leaving them valid is the classic mistake — the account is gone from the list
  and still works.
- Demoting a user **updates live sessions**, so an ex-admin does not keep admin
  rights for the remaining 12 hours.
- Hard ceiling of 10,000 live sessions; expired entries are reclaimed before
  the limit is reported as reached.

### Why sessions are not stored in the keyspace

The keyspace is right there, and it has TTL support. But under `--maxmemory`
with an `allkeys-*` policy it evicts whatever it likes — so sessions would be
dropped at random under load, and *the harder the demo is hit, the more often
users would be logged out*. Session storage needs a different eviction rule
from cache storage, so it gets its own store.

---

## Login throttling

Both the **account** and the **source address** are throttled, because either
alone leaves a hole: per-account only lets one host spray the whole user list;
per-address only lets a botnet grind a single account.

Default: 5 consecutive failures → 60-second lockout. During a lockout even the
*correct* password is refused — otherwise the lockout is decorative, and an
attacker who guesses right on attempt six still wins.

`X-Forwarded-For` is used for the address, and is trusted only because the
supported deployment shape puts exactly one reverse proxy in front. Exposed
directly to the internet that header is client-controlled and worthless — which
is why it is used **solely for throttling and never for an authorization
decision**.

---

## Username enumeration

When the username does not exist, the service verifies the password against a
**decoy hash** built at startup with the same iteration count, instead of
returning early:

```cpp
const std::string_view hash_to_check =
    user_exists ? std::string_view(stored_hash) : std::string_view(decoy_hash_);
```

Without it, "no such user" returns in microseconds while a wrong password takes
~100 ms, and that gap enumerates the user list from the outside. The error text
is identical in both cases too, and a test asserts the two messages are equal.

---

## What is *not* protected

Stated plainly, because a security document that only lists strengths is
marketing.

**Users are not persisted.** They live in memory and are seeded at startup from
`$BOURSE_ADMIN_USER` / `$BOURSE_ADMIN_PASSWORD`. Users created at runtime with
`USER ADD` are lost on restart.

This is deliberate. Users *could* be journalled through the existing WAL hook —
`USER` would only have to answer `isWrite()` honestly — but replaying
`USER ADD alice hunter2` means **a plaintext password in a file on disk**. That
is worse than not persisting users at all. Persisting the *hash* is the correct
fix and is the next change if user management ever needs to survive a restart.

**There is no TLS.** The server speaks plain HTTP and plain RESP. In the
supported deployment the platform (Render, Fly, Cloud Run) terminates TLS at
its edge and the hop to the container is inside their network. Exposing this
server directly to the internet without a TLS-terminating proxy would put
bearer tokens and passwords on the wire in clear text. Don't.

**Tokens live in `localStorage`.** That makes them readable by any script that
achieves XSS on the dashboard's origin. `httpOnly` cookies would resist that,
but they cannot be sent cross-origin from a CDN-hosted page to a separately
hosted API without `SameSite=None` plus a CSRF token scheme. Given the
dashboard renders no user-supplied HTML — every interpolation goes through
`esc()` — and ships no third-party script, `localStorage` is the reasonable
trade here. It would not be for an application with real user data.

**CORS is `Access-Control-Allow-Origin: *`.** Any origin may call the API. With
bearer tokens rather than cookies this is not a CSRF vector — a cross-origin
page cannot read the token to attach it — but it does mean the API is callable
from anywhere by anyone holding a token. Restricting the origin is a one-line
change in `makeCorsMiddleware`.

**`/metrics` requires authentication when auth is on.** It is not on the public
allowlist, because the Prometheus exposition includes keyspace sizes, command
counts and latency distributions — a reasonable amount of information about a
system you are not logged into. The consequence is that a scraper needs a
bearer token like any other client. If you would rather have it open, add
`/metrics` to `isPublicPath` in `backend/src/server/rest_api.cpp`; that is the
one line, and it is a deliberate decision either way.

Only `/`, `/health` and `/api/auth/login` are public: the dashboard has to load
in order to show a login screen, the platform's health check has to work
without credentials, and you cannot log in if the login endpoint needs a login.

**No audit log.** Failed logins are counted but individual attempts are not
recorded with timestamps and addresses.

**No password-reset flow.** An admin sets passwords with `USER PASSWD`.

---

## Enabling authentication

Off by default, so a local run and every existing smoke suite work without
credentials.

```bash
# Explicit password
./backend/build/bin/bourse-server --auth yes --admin-user admin --admin-password 'a-strong-one'

# Preferred: the environment. argv is visible to every user on the box via `ps`.
BOURSE_AUTH=yes BOURSE_ADMIN_USER=admin BOURSE_ADMIN_PASSWORD='a-strong-one' \
  ./backend/build/bin/bourse-server

# No password given -- one is generated and logged once, never stored
./backend/build/bin/bourse-server --auth yes
```

The last form prints:

```
[WARN ] =====================================================================
[WARN ]   Generated administrator credentials -- shown once, not stored:
[WARN ]     username: admin
[WARN ]     password: 8f3a2c1e9b7d4a06
[WARN ]   Set --admin-password or $BOURSE_ADMIN_PASSWORD to choose your own.
[WARN ] =====================================================================
```

Deploying with a *default* password is how demo servers get taken over.
Generating one and showing it once is what Jupyter and Grafana do, and it is
why there is no committed default anywhere in this repository.

---

## Verifying all of it

```bash
bash backend/scripts/verify-crypto.sh # crypto vs. Python hashlib
bash backend/scripts/smoke-deploy.sh  # 50 assertions, auth over both protocols
./backend/build/bin/bourse_tests --gtest_filter='*Auth*:*Sha256*:*Hmac*:*Pbkdf2*:*Password*:*Role*:*Json*'
```

`smoke-deploy.sh` starts a real server with auth enabled and asserts, over the
wire:

- `/health` and `/` stay public; every API path returns 401 without a token
- the 401 carries a `WWW-Authenticate: Bearer` challenge
- a wrong password is refused; a right one returns a token and a role
- a viewer can read, and is refused `SET`, `FLUSHALL` and the user list
- logout genuinely revokes — the token stops working
- a forged token is refused
- the CORS pre-flight allows `Authorization`, without which cross-origin auth
  silently fails in a browser
- **the same policy over RESP through stock `redis-cli`**: `PING` before
  `AUTH`, `NOAUTH` on `GET`, `WRONGPASS` on a bad password, and `NOPERM` when a
  viewer tries to write

That last group is what proves "one authorization decision shared by both
protocols" is true on the wire and not just in a unit test.
