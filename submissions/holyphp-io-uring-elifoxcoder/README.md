# HolyPHP — io_uring — @Elifoxcoder

A server for the [$12 Server Challenge](https://github.com/arjaythedev/twelve-dollar-server-challenge)
written in **HolyPHP**, a statically-typed, PHP-flavoured language that compiles
to C11. The HTTP loop, routing, JWT verification, JSON rendering and SQLite
access are native code; there is no interpreter, VM, PHP-FPM, FastCGI or
CGI layer involved at run time.

| Item | Version / choice |
|---|---|
| Language | HolyPHP 1.0.2 (compiled to C11) |
| HTTP server | custom Linux `io_uring` engine (no framework) |
| SQLite driver | C API, system `libsqlite3` (Ubuntu 24.04: 3.45.x) |
| Compiler | `gcc` 13 (Ubuntu 24.04), built in `build.sh` |
| `io_uring` | system `liburing` (Ubuntu 24.04: 2.5.x) |
| Serving mode | **behind the challenge Nginx** on `127.0.0.1:3000` |

The submission is self-contained: `toolchain/` holds the HolyPHP 1.0.2 compiler
and runtime sources (MIT, see `toolchain/LICENSE`), `server.hphp` is the
program, and `install.sh` / `build.sh` build both on the box. Nothing prebuilt
is committed and the build needs no network access beyond `install.sh`.

## Build and run

```bash
sudo bash install.sh          # once, as root on Ubuntu 24.04
bash build.sh                 # as the benchmark user
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
HOST=127.0.0.1 PORT=3000 bash start.sh
```

`start.sh` runs in the foreground and reads only the four environment
variables from `SPEC.md`.

## What the engine does

- **`io_uring`, not an event-loop library.** Multi-shot accept, single-shot
  receive straight into the connection's own buffer, `DEFER_TASKRUN |
  SINGLE_ISSUER`, one 4096-entry ring, batched CQE harvesting and SQ-full
  recovery.
- **Fixed connection records.** 2048 slots of 16 KiB each hold the request and
  response buffers; there is no per-request heap allocation and idle
  connections keep no buffers. This is why the server is run behind Nginx,
  which keeps at most 64 upstream connections, instead of being exposed
  directly to ~15,000 client connections.
- **One SQLite connection, prepared statements, WAL.** `synchronous=NORMAL`,
  `foreign_keys=ON`, `mmap_size=1 GiB`, `cache_size=-64000`,
  `busy_timeout=5000`, `wal_autocheckpoint=4096`.
- **Group commit.** Every write harvested in one CQE batch shares one
  `BEGIN IMMEDIATE … COMMIT`. The response SQEs for the batch are only
  submitted after that commit succeeds, so a `201` means the row is committed.
- **No caching across requests.** `/feed`, `/posts/:id`, authors and
  `like_count` are read from SQLite during the request. The feed is never
  cached, because the test likes a post and immediately re-reads it. Prepared
  statements retrieve fresh rows; they only cache the query plan. JWTs are
  verified on every request.
- **JWT HS256 in-process.** Base64url, HMAC-SHA256 and the small JSON payload
  parser are implemented in the engine, so there is no OpenSSL call or
  allocation on the auth path. Signature and `exp` are checked on every
  request; only HS256 is accepted.
- **JSON straight from SQLite column bytes.** The post object is rendered
  directly into the response buffer in the exact key order from `SPEC.md`, with
  string escaping and body trimming/500-character validation done in place.
- **Auth before id parsing** on authenticated routes, matching the spec's
  `401`/`400` ordering.

## Correctness

The submission was checked with the challenge's own suite:

```bash
bash seed/make-seed.sh
bash test/run.sh submissions/holyphp-io-uring-elifoxcoder
```

All **42/42** checks in `test/test.sh` pass on a fresh copy of `seed/feed.db`.
The suite runs directly against `HOST:PORT`, so no Nginx is needed for CI; the
Nginx choice only affects the benchmark deployment described above.

## Layout

```
install.sh             apt packages needed to build (gcc, SQLite, liburing)
build.sh               builds toolchain/hphp, then bin/server
start.sh               runs bin/server in the foreground
server.hphp            the program: reads PORT / SQLITE_PATH and calls the engine
toolchain/             HolyPHP compiler + runtime source (source only)
.gitignore             keeps build/, bin/ and generated headers out of git
```

The engine lives in `toolchain/src/hphpc/runtime/hphp_std.c`
(`holy_challenge_start()`); `server.hphp` is the thin HolyPHP entry point.
