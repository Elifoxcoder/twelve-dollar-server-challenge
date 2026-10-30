# HolyPHP feed API (twelve-dollar-server-challenge submission)

**Language:** HolyPHP 1.0.2, compiled to C11 by the compiler vendored in `toolchain/`.

**Serving mode:** behind the challenge Nginx on `127.0.0.1:3000` (64 keepalive upstream connections).

## What's in here

- `server.hphp` — the entire server: HTTP/1.1 keep-alive loop, request parsing and routing, HS256 JWT verification on every request, strict JSON validation for POST bodies, all SQL, and the write policy (group commit).
- `toolchain/` — the HolyPHP compiler, vendored. Built from source by `build.sh`.
- `install.sh` / `build.sh` / `start.sh` — standard challenge scripts.

## Server-side optimizations

- **SQL renders the JSON.** `/feed` and `/posts/:id` bodies are produced by `json_object` / `json_group_array(...)` inside SQLite; the server never re-serializes rows. The feed's `ORDER BY` lives in the input subquery and `json_group_array` preserves that order.
- **Group commit.** Writes collected during one poll cycle are applied in a single `BEGIN IMMEDIATE … COMMIT`; responses are only sent after `COMMIT` succeeds (all flipped to 500 on rollback).
- **WAL + `synchronous=NORMAL`**, `temp_store=MEMORY`, 64 MB page cache, 256 MB mmap, prepared statements.
- **One write for small bodies**, header + body written separately for the 5 KB feed (avoids one 5 KB copy).
- **TCP_NODELAY**; idle connections held 90 s (spec asks ≥ 65 s).
- A strict RFC 8259 parser for POST bodies (the runtime's `json_decode` is lenient).

## Runtime/compiler work (generic, no challenge logic)

The server is plain HolyPHP, but the language implementation was made fast and
allocation-stable for a long-running server. All of this is generic:

- **Interned string literals.** Codegen emits `hp_str_intern("...")`; the runtime
  caches one `hstr` per literal address instead of allocating a copy on every
  evaluation. A hot loop evaluating 15 literals per request went from 15
  allocations to 15 hash probes.
- **Request-scope arena.** `scope_begin()` / `scope_end()` bracket a poll cycle;
  every string allocated in between is released in one shot. `str_persist()`
  copies a value out of the scope, `str_free()` / `arr_free()` / `arr_clear()`
  release heap values early. Without this the server leaked ~7.5 KB per request
  (3 GB in 20 s); it is now flat.
- **No redundant clone for array literals.** `$r = [];` used to allocate three
  arrays (`hp_arr_new`, `hp_arr_of`, then a clone); the fresh literal is no
  longer cloned.
- **`stream_ready_into($out, $handles, $ms)`** reuses the caller's array and keeps
  the pollfd buffer between calls, so a poll round allocates nothing.
- A plain SQLite binding (`sqlite_open`/`prepare`/`bind_*`/`step`/`column_*`/…),
  `stream_set_nodelay`, a JSON string-encoder correctness fix, and
  `signal(SIGPIPE, SIG_IGN)`.

There is no challenge-specific C code and no `io_uring`.

## Requirements

- SQLite ≥ 3.38 (JSON1 built in; `INSERT … RETURNING` needs ≥ 3.35). Ubuntu 24.04 ships 3.45.
- `libsqlite3-dev`, `build-essential`.

## Run

```bash
bash install.sh   # as root, once
bash build.sh     # as the build user
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 ./bin/server
```

## Local measurements

Single-threaded server, 64 keepalive connections, 249 MB seed database, measured
with a keep-alive HTTP client on a desktop core (not the benchmark box):

| Endpoint | req/s | CPU per request | Body |
|---|---:|---:|---:|
| `GET /feed` | ~22,500 | ~40 µs | 5,102 B |
| `GET /posts/:id` | ~28,000 | ~35 µs | 146 B |

The feed query itself is ~21 µs of that (SQLite + JSON1 rendering of 20 posts);
the rest is the HTTP path. Memory is flat under sustained load (5.8 MB after
500k requests).
