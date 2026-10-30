# HolyPHP feed API (twelve-dollar-server-challenge submission)

**Language:** HolyPHP 1.0.2, compiled to C11 by the compiler vendored in `toolchain/`.

**Serving mode: direct** — the server listens on `HOST:PORT` itself (the box sets
`0.0.0.0:80`), with **no Nginx in the request path**. If `:80` is already taken
(for example by Nginx), it falls back to `127.0.0.1:3000` so the service still
comes up behind the proxy. Measured on one vCPU, direct mode is ~50 % faster
than the same server behind Nginx, because the proxy costs about as much CPU as
the app itself.

## What's in here

- `server.hphp` — the entire server: HTTP/1.1 keep-alive loop, request parsing and
  routing, HS256 JWT verification on every request, strict JSON validation for
  POST bodies, all SQL, and the write policy (group commit).
- `toolchain/` — the HolyPHP compiler, vendored. Built from source by `build.sh`.
- `install.sh` / `build.sh` / `start.sh` — standard challenge scripts.

## Server-side optimizations

- **SQL renders the JSON.** `/feed` and `/posts/:id` bodies are produced by
  `json_object` / `json_group_array(...)` inside SQLite; the server never
  re-serializes rows. The feed's `ORDER BY` lives in the input subquery and
  `json_group_array` preserves that order.
- **Group commit.** Writes collected during one poll cycle are applied in a single
  `BEGIN IMMEDIATE … COMMIT`; responses are only sent after `COMMIT` succeeds
  (all flipped to 500 on rollback).
- **WAL + `synchronous=NORMAL`**, `temp_store=MEMORY`, 64 MB page cache, 256 MB
  mmap, prepared statements.
- **One write for small bodies**, header + body written separately for the feed
  (avoids one multi-KB copy).
- **`Content-Encoding: identity`** — compression is optional per SPEC.md, and the
  box is CPU-bound, so the body goes out as-is.
- **TCP_NODELAY**; idle connections held 90 s (spec asks ≥ 65 s).
- **Connection shedding** above 58,000 open fds (the box sets
  `LimitNOFILE=65535`): such responses carry `Connection: close`, so new users
  are never refused.
- A strict RFC 8259 parser for POST bodies (the runtime's `json_decode` is lenient).

## Runtime/compiler work (generic, no challenge logic)

The server is plain HolyPHP, but the language implementation was made fast and
allocation-stable for a long-running server. All of this is generic:

- **epoll readiness.** `epoll_add` / `epoll_del` / `epoll_wait_into` let the loop
  wait on thousands of client sockets in O(1); `stream_ready` (poll) is O(n).
- **O(1) socket state.** The runtime used a linear scan over every open socket on
  each `recv`/`send`; it is now indexed by descriptor.
- **Interned string literals.** Codegen emits `hp_str_intern("...")`; the runtime
  caches one `hstr` per literal address instead of allocating a copy per use.
- **Request-scope arena.** `scope_begin()` / `scope_end()` bracket a poll cycle;
  every string allocated in between is released in one shot. `str_persist()`
  copies a value out, `str_free()` / `arr_free()` / `arr_clear()` release heap
  values early. Without this the server leaked ~7.5 KB per request (3 GB in 20 s).
- **No redundant clone for array literals** (`$r = [];` used to allocate three
  arrays).
- **`stream_ready_into`** reuses the caller's array and the pollfd buffer.
- A plain SQLite binding (`sqlite_open`/`prepare`/`bind_*`/`step`/`column_*`/…),
  `stream_set_nodelay`, a JSON string-encoder fix, and `signal(SIGPIPE, SIG_IGN)`.

There is no challenge-specific C code and no `io_uring`.

## Requirements

- SQLite ≥ 3.38 (JSON1 built in; `INSERT … RETURNING` needs ≥ 3.35). Ubuntu 24.04 ships 3.45.
- `libsqlite3-dev`, `build-essential`.

## Run

```bash
bash install.sh   # as root, once
bash build.sh     # as the build user
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 ./bin/server
```

## Measurements

Docker container shaped like the box (`--cpus=1 --memory=2g`, Ubuntu 24.04,
SQLite 3.45, fresh 249 MB seed, mixed k6-style load: feed → post → 15 % like,
2 % post), load generators on separate cores:

| Mode | req/s at saturation | CPU/request | Errors | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| **direct** | **23,334** | 39.2 µs | 0 | 8.2 ms | 10.9 ms |
| behind Nginx | 15,684 | 60.7 µs (app 31.5 + proxy 29.2) | 0 | 53 ms | 56 ms |

With the k6 user model (~0.096 req/s per user), 23,334 req/s ≈ 240,000 users at
~92 % CPU; at a 70 % CPU budget ≈ 185,000. The container runs on a desktop core
that is faster than the 2.4 GHz Xeon on the benchmark box, so the scored number
will be lower — the ratio between the two modes is what the measurement is for.

`bash test/run.sh submissions/holyphp-io-uring-elifoxcoder` → `passed=42 failed=0`
(also verified behind Nginx in the container, on SQLite 3.45).
