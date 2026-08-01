# Deploying Bourse

## The short version

**Vercel can host the dashboard. It cannot host the server.**

That is not a configuration problem to work around — it is what Vercel is. Vercel
runs *serverless functions*: a request arrives, a short-lived sandbox wakes up,
returns a response, and is frozen or discarded. There is no process that stays
resident between requests.

Bourse is the opposite of that by design:

| What Bourse needs | What Vercel provides |
|---|---|
| A process that lives for days holding an `epoll` loop | A sandbox scoped to one request |
| Long-lived TCP connections (RESP clients stay connected) | HTTP request/response only |
| A raw TCP port for `redis-cli` on 6380 | HTTP(S) on 443 only |
| Shared process memory — keyspace, order books, buffer pool | Nothing shared between invocations |
| A writable disk for the WAL and snapshots | Read-only filesystem apart from `/tmp` |
| A compiled C++ binary | Node, Python, Go, Ruby runtimes |

A cache whose contents vanish between requests is not a cache, and a matching
engine that forgets its order book between requests is not a matching engine.

So the deployment is split, which is the normal shape for this kind of system:

```
  Browser
     |
     |  HTTPS
     v
  Vercel  ──────────►  frontend/index.html    (static, global CDN, free)
     |
     |  fetch() cross-origin, CORS-approved
     v
  Render  ──────────►  bourse-server in Docker (the actual C++ server, free)
```

One file, `frontend/index.html`, is used both ways: the server binary embeds it
at build time, and `frontend/build.sh` stamps the backend URL into a copy of it
for the CDN. There is no second copy to keep in sync.

The repository has exactly two source directories:

```
Bourse/
├── frontend/          index.html, build.sh, vercel.json   -> Vercel
├── backend/           C++20 server, tests, scripts        -> Render
├── docs/              architecture, benchmarks, security, this file
├── render.yaml        must sit at the root; Render only looks there
└── docker-compose.yml
```

---

## Part 0 — Push to GitHub

Both platforms deploy from a GitHub repository, so this comes first.

### 0.1 Create the repository

Go to <https://github.com/new> and set:

- **Repository name**: `Bourse`
- **Visibility**: **Public** — recruiters have to be able to open it
- **Do not** tick "Add a README", ".gitignore", or "license". The repo already
  has all three, and an initialised remote forces a merge on the first push.

Click **Create repository**, then leave the page open — you need the URL.

### 0.2 Point the local repo at it

```bash
cd /mnt/c/Users/ayush/Github/Bourse

git remote add origin https://github.com/<your-username>/Bourse.git
git branch -M main
git push -u origin main
```

If GitHub asks for a password, it does not mean your account password —
password authentication for git was removed in 2021. Use a **Personal Access
Token**: <https://github.com/settings/tokens> → *Generate new token (classic)* →
tick **repo** → copy it and paste it as the password. To avoid re-entering it:

```bash
git config --global credential.helper store
```

### 0.3 Confirm it landed

```bash
git log --oneline -1
git remote -v
```

Then reload the GitHub page. You should see all files, and the README rendered
with the architecture diagram.

---

## Part 1 — Deploy the backend on Render

Render is used here because it builds a `Dockerfile` directly, gives free HTTPS
on a public URL, and needs no credit card. Any container host works the same
way — Fly.io, Railway, Cloud Run — and the alternatives are covered at the end.

Do this **before** the frontend: the dashboard needs the backend's URL, so
deploying it first saves a redeploy.

### 1.1 Create the service

1. Sign up at <https://render.com> with **Sign in with GitHub**.
2. Grant access to the `Bourse` repository when prompted.
3. **New +** → **Web Service** → select `Bourse`.

### 1.2 Settings

Render reads `render.yaml`, so most of this is pre-filled. Confirm:

| Field | Value |
|---|---|
| Name | `bourse` (this becomes `bourse.onrender.com`) |
| Language / Runtime | **Docker** |
| Branch | `main` |
| Region | **Singapore** (lowest latency from India) |
| Instance Type | **Free** |
| Health Check Path | `/health` |

Leave **Docker Command** empty to use `render.yaml`'s, or paste:

```
/usr/local/bin/bourse-server --host 0.0.0.0 --maxmemory 200mb --maxmemory-policy allkeys-lru --appendonly yes --dir /home/bourse/data --log-level info
```

**Docker Build Context Directory** must be `.` (the repository root) and
**Dockerfile Path** `./backend/Dockerfile`. The server embeds
`frontend/index.html`, so a context of `backend/` alone fails at the
asset-embed step. `render.yaml` already sets both.

Note there is no `--http-port`. Render assigns the public port at run time and
passes it in `$PORT`; `Config::fromArgs` reads that before parsing argv, so the
same image runs anywhere without a rebuild.

### 1.3 Deploy

Click **Create Web Service**. The first build compiles the whole project from
source inside the container and takes **8–15 minutes**. Watch the log for:

```
[build] Building CXX object ...
[build] Linking CXX executable bin/bourse-server
==> Build successful 🎉
==> Deploying...
==> Your service is live 🎉
```

### 1.4 Verify it before touching the frontend

Replace `bourse` with your service name:

```bash
curl https://bourse.onrender.com/health
# {"status":"ok",...}

curl https://bourse.onrender.com/api/stats
# {"keys":0,"memory_bytes":...,"command_latency":{...}}

curl -X POST "https://bourse.onrender.com/api/orders?symbol=AAPL&side=BUY&type=LIMIT&quantity=10&price=150.25"
# {"status":"NEW",...}

curl https://bourse.onrender.com/api/book/AAPL
# {"bids":[{"price":"150.25","quantity":10}],"asks":[]}
```

Opening `https://bourse.onrender.com/` in a browser also serves the embedded
dashboard directly — the backend is already a complete, working deployment on
its own. The Vercel step exists to put the UI on a fast global CDN and to give
you a second, frontend-shaped link.

**Copy that URL.** The next part needs it.

### 1.5 Turn on authentication

Everything above is deployed wide open: anyone who finds the URL can run
`FLUSHALL`. For anything public, turn on auth. In Render, **Environment** →
**Add Environment Variable**:

| Key | Value |
|---|---|
| `BOURSE_AUTH` | `yes` |
| `BOURSE_ADMIN_USER` | `admin` |
| `BOURSE_ADMIN_PASSWORD` | a long random string you generate |

Environment variables rather than flags on purpose: `ps` shows a process's
command line to every user on the box, and Render stores env vars as secrets.

If you set `BOURSE_AUTH=yes` and **omit** the password, the server generates one
and prints it to the log exactly once at startup:

```
[WARN ]   Generated administrator credentials -- shown once, not stored:
[WARN ]     username: admin
[WARN ]     password: 8f3a2c1e9b7d4a06
```

Copy it from the Render log immediately — it is never written anywhere else.
That is deliberate: there is no default password committed anywhere in this
repository, because a demo server with a default password is a demo server
someone else owns.

Save, let it redeploy, then verify:

```bash
curl -i https://bourse.onrender.com/api/stats
# HTTP/1.1 401 Unauthorized
# WWW-Authenticate: Bearer realm="bourse"

curl -X POST https://bourse.onrender.com/api/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"<yours>"}'
# {"token":"…","username":"admin","role":"admin","expires_at_ms":…}
```

`/health` and `/` stay public so the platform's health check keeps working and
the dashboard can load its login screen.

Roles are `viewer` (read-only), `trader` (+ writes and orders) and `admin`
(+ `FLUSHALL`, `CONFIG`, user management). Add a read-only account for anyone
you share the link with:

```bash
curl -X POST https://bourse.onrender.com/api/auth/users \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"username":"guest","password":"a-long-guest-password","role":"viewer"}'
```

Full model, including what it deliberately does *not* protect:
[docs/security.md](security.md).

---

## Part 2 — Deploy the dashboard on Vercel

### 2.1 Import the project

1. Sign up at <https://vercel.com> with **Continue with GitHub**.
2. **Add New…** → **Project** → **Import** next to `Bourse`.

### 2.2 Settings

`vercel.json` supplies the build configuration, so leave **Framework Preset** at
**Other** and do not override the build or output settings.

The one thing you must set is the environment variable. Expand
**Environment Variables** and add:

| Key | Value |
|---|---|
| `BOURSE_API_BASE` | `https://bourse.onrender.com` |

Also set **Root Directory** to `frontend` in the project's settings. That is
what makes `frontend/vercel.json` the configuration Vercel reads, and it keeps
the whole C++ tree out of the build.

Use your real Render URL. **No trailing slash**, and it must be `https://` — a
page served over HTTPS cannot call an HTTP backend, browsers block it as mixed
content. `frontend/build.sh` refuses both mistakes rather than shipping a page that
silently fails.

### 2.3 Deploy

Click **Deploy**. This build takes seconds — it copies one HTML file and
rewrites one line. Look for:

```
Running "bash build.sh"
build: wrote /vercel/path0/frontend/dist/index.html -> backend https://bourse.onrender.com
Build Completed
```

You now have two links:

- **Frontend** — `https://bourse.vercel.app`
- **Backend** — `https://bourse.onrender.com`

### 2.4 Verify

Open the Vercel URL. Within a second the dot next to "Bourse" should turn green
and read **connected**, the stat tiles should fill in, and the latency
histogram should start moving.

If it says **server unreachable**, see [Troubleshooting](#troubleshooting).

---

## How the frontend finds the backend

`frontend/index.html` resolves its API origin at load time, most specific
source first:

1. **`?api=` in the URL** — `https://bourse.vercel.app/?api=https://other.example`.
   Useful for pointing the deployed dashboard at a different backend without a
   redeploy. The value is remembered.
2. **The header "backend" field** — type a URL, press Enter. Stored in
   `localStorage`. Clearing the field reverts to the deploy-time default.
3. **`window.BOURSE_API_BASE`** — the line `frontend/build.sh` rewrites from
   `BOURSE_API_BASE` at build time. This is the normal path.
4. **Same origin** — the fallback, and what the binary-embedded copy uses.

Because of (1) and (2), a Vercel deploy with the environment variable missing is
recoverable without a rebuild: type the backend URL into the header field.

Cross-origin calls work because the server installs a CORS middleware
([`backend/src/net/router.cpp`](../backend/src/net/router.cpp)) that answers `OPTIONS`
pre-flights with `204` and attaches `Access-Control-Allow-Origin: *` to every
response. `backend/scripts/smoke-deploy.sh` asserts this against a running server,
because in a browser a missing CORS header shows up only as an empty page and a
console message.

---

## Testing the split locally before deploying

You do not have to deploy to find out whether the split works:

```bash
bash backend/scripts/smoke-deploy.sh
```

This starts the server, builds the static bundle, serves it from a *different*
origin, and asserts the whole path — `$PORT` handling, pre-flight, cross-origin
`GET` and `POST`, the injected URL, and that the embedded copy still defaults to
same-origin. 16 assertions, all of which must pass.

To look at it yourself:

```bash
./backend/build/bin/bourse-server --http-port 8080 &
BOURSE_API_BASE=http://localhost:8080 bash frontend/build.sh
python3 -m http.server -d frontend/dist 3000
# open http://localhost:3000 -- a dashboard on :3000 driving a server on :8080
```

---

## What the free tiers cost you

Worth knowing before someone else clicks the link, and worth being able to
explain if asked.

**Render free tier spins down after 15 minutes of inactivity.** The next request
wakes it, which takes **around 50 seconds**. A recruiter who opens your link
cold sees "server unreachable" until it boots. Mitigations:

- Say so on the page you link from: *"first load takes ~50s, the free instance
  is asleep."* Better than looking broken.
- Open the link yourself a minute before a demo.
- Ping `/health` every 10 minutes from a free cron service to keep it warm.
- $7/month on Render removes the spin-down entirely.

**No persistent disk on the free tier.** The WAL and snapshots are written to
the container filesystem, so recovery genuinely works across a process restart —
but not across the instance being replaced. Add a Render disk mounted at
`/home/bourse/data` on a paid plan to make it durable. This does not affect
`backend/scripts/smoke-persistence.sh`, which tests recovery properly by `SIGKILL`ing a
local server.

**Only the HTTP port is public.** Render publishes one port per web service, so
`redis-cli -h bourse.onrender.com -p 6380` cannot reach the deployed instance.
The RESP listener is still running inside the container, and every command is
reachable over `POST /api/command`. If exposing RESP publicly matters, use Fly.io
instead — see below.

**512 MB of RAM.** Hence `--maxmemory 200mb --maxmemory-policy allkeys-lru`:
under load the cache evicts cold keys instead of being OOM-killed.

---

## Alternatives to Render

The Dockerfile is standard, so any container host works. Only the port
convention differs, and `$PORT` covers all of these.

### Fly.io — if you want `redis-cli` to reach the deployed server

Fly can publish raw TCP, which Render's free tier cannot. Roughly $2–3/month for
a shared-cpu-1x/256MB machine.

```bash
curl -L https://fly.io/install.sh | sh
fly auth login
fly launch --dockerfile backend/Dockerfile --no-deploy
```

Then in the generated `fly.toml`:

```toml
[[services]]
  internal_port = 8080
  protocol = "tcp"
  [[services.ports]]
    port = 443
    handlers = ["tls", "http"]

[[services]]
  internal_port = 6380
  protocol = "tcp"
  [[services.ports]]
    port = 6380          # RESP, reachable by redis-cli
```

```bash
fly deploy
redis-cli -h bourse.fly.dev -p 6380 PING     # actually works here
```

### Railway

**New Project** → **Deploy from GitHub repo** → picks up the Dockerfile
automatically, injects `$PORT`. $5 of trial credit, then usage-based.

### Google Cloud Run

Generous free tier and scales to zero, but needs a billing account and the
`gcloud` CLI.

```bash
gcloud run deploy bourse --source . --region asia-south1 --allow-unauthenticated
```

Cloud Run also passes `$PORT`, so no changes are needed.

---

## Troubleshooting

**Dashboard says "server unreachable".**
Open the browser console (F12).

- *CORS error* — you are hitting something that is not Bourse. Check
  `BOURSE_API_BASE` points at the Render URL, not the Vercel one.
- *Mixed content blocked* — `BOURSE_API_BASE` is `http://`. It must be `https://`.
- *`net::ERR_CONNECTION_TIMED_OUT` or a 502* — the free instance is asleep. Wait
  50 seconds and reload.
- *404s on `/api/stats`* — the variable was never set, so the page is calling
  Vercel. Confirm by viewing source: line 12 should name your backend. Fix the
  variable and **redeploy** — Vercel bakes environment variables in at build
  time, so saving one changes nothing until you rebuild.

**Vercel build fails with `BOURSE_API_BASE must be a plain http(s) URL`.**
Working as intended: the value is interpolated into a JavaScript string, so only
`https://host[:port][/path]` is accepted. Check for a stray quote, space, or a
copied `"` from the docs.

**Render build fails at `Embedding frontend/index.html`.**
The build context was set to `backend/` instead of the repository root. The
server embeds the dashboard, so the context has to span both halves:
`dockerContext: .` and `dockerfilePath: ./backend/Dockerfile`.

**Render build times out or OOMs.**
The free build container compiles the whole project. If it runs out of memory,
add `-DCMAKE_BUILD_PARALLEL_LEVEL=1` to the build stage to compile serially.

**`git push` rejected as non-fast-forward.**
You initialised the GitHub repo with a README. Either recreate it empty, or:

```bash
git pull --rebase origin main
git push -u origin main
```

---

## After the first deploy

Both platforms watch `main`. Once set up:

```bash
git add -A
git commit -m "..."
git push
```

Render rebuilds the backend, Vercel rebuilds the frontend, both automatically.
Changing `BOURSE_API_BASE` is the one case that needs a manual **Redeploy** on
Vercel, since it is applied at build time rather than read at run time.
