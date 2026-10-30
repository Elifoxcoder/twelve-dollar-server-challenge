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

- **The feed and a single post are read from SQLite's B-tree cursors.** `/feed`
  walks the `posts` rowid index backwards twenty rows while a second cursor
  walks `likes_post_id_idx` in the same order, so each post's likes are the
  next index entries; the authors are twenty rowid seeks in `users`. Records
  are decoded in place (serial types, payload offsets) and escaped straight
  into the response buffer, so a feed row allocates no strings and no arrays.
  `/posts/:id` uses the same cursors and one index range scan for its count.
  This is a generic driver binding (`bt_root` / `bt_open` / `bt_last` / …),
  not challenge logic, and it falls back to the SQL path when the pinned
  SQLite could not be built (the binding is a weak reference).
- **A string builder with a JSON escaper.** `sb_new` / `sb_put` / `sb_int` /
  `sb_json` / `sb_take` append into one buffer; the escaper copies runs of
  ordinary bytes with one `memcpy` and uses a 256-entry table per byte.
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
- **B-tree cursor binding.** `toolchain/src/hp_btree.c` is appended to the pinned
  amalgamation by `build.sh` (it needs SQLite's internals) and exports
  `hp_bt_*_impl`: root page by table/index name, cursor open with the index's
  `KeyInfo`, `first`/`last`/`next`/`prev`/`seek`, rowid and payload. The runtime
  declares those as weak references, so a build against the system library still
  links and the server uses its SQL path.
- **Pointer arithmetic and typed dereference** in the compiler (`ptr + int`,
  `*u8` reads one byte), and a cache key that includes `HPHP_SQLITE_LIB` (a
  cached binary must not silently switch SQLite builds).

There is no challenge-specific C code and no `io_uring`.

## Requirements

- `build-essential`, `curl`, `unzip`, `libsqlite3-dev`.
- `build.sh` builds the pinned SQLite amalgamation (3.46.1, sha256-checked) with
  `-DSQLITE_THREADSAFE=0`: the server is single-threaded, so the serialized
  build only pays mutex overhead on every step/column call. If the download or
  the build fails, it falls back to the system library, so an offline box still
  produces a working server. JSON1 and `INSERT … RETURNING` need SQLite ≥ 3.38.

## Run

```bash
bash install.sh   # as root, once
bash build.sh     # as the build user
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 ./bin/server
```

## Measurements

On a VPS shaped like the box (1 vCPU AMD EPYC Rome, 2 GB, Ubuntu 26.04), with
the mixed k6 request model (feed → post → 15 % like, 2 % post), four runs, load
generator on the same machine so the server's own CPU time is what is measured:

| Mode | req/s at saturation | CPU/request | Errors |
|---|---:|---:|---:|
| **direct, steady state** | **26,832** | **34.3 µs** | 0 |
| behind Nginx | 15,684 | 60.7 µs (app 31.5 + proxy 29.2) | 0 |

At the k6 model's 0.096 req/s per user, 26,832 req/s at 92 % of one core is
**~279,500 users** (four consecutive runs on the final build: 277,800–285,900).
(The feed in the steady state is ~3.5 KB, not the 5.1 KB of the golden seed:
after a few seconds the twenty newest posts are the ones the load test created.
Measuring against a fresh database overstates the cost.)

The Nginx row is from the earlier SQL-only build; the B-tree rows were measured
in direct mode only.

`bash test/run.sh submissions/holyphp-io-uring-elifoxcoder` → `passed=42 failed=0`
(also verified behind Nginx in the container, on SQLite 3.45).
