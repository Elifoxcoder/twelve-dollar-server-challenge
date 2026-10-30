# HolyPHP feed API (twelve-dollar-server-challenge submission)

**Language:** HolyPHP 1.0.2, compiled to C11 by the compiler vendored in `toolchain/`.

**Serving mode:** behind the challenge Nginx on `127.0.0.1:3000` (64 keepalive upstream connections).

## What's in here

- `server.hphp` — the entire server: HTTP/1.1 keep-alive loop on top of the runtime's non-blocking socket primitives, request parsing and routing, HS256 JWT verification on every request, strict JSON validation for POST bodies, all SQL (response bodies are rendered by SQLite's JSON1 functions), and the write policy (group commit).
- `toolchain/` — the HolyPHP compiler, vendored. Built from source by `build.sh`.
- `install.sh` / `build.sh` / `start.sh` — standard challenge scripts.

## The native side is generic only

The runtime additions are generic primitives, not challenge logic:

- a plain SQLite binding (`sqlite_open`, `sqlite_prepare`, `sqlite_bind_*`, `sqlite_step`, `sqlite_column_*`, `sqlite_exec`, …),
- `stream_set_nodelay` (TCP_NODELAY on a connected socket),
- a correctness fix to the JSON string encoder (`\r`, `\b`, `\f`, and every `< 0x20` as `\u00XX`),
- `signal(SIGPIPE, SIG_IGN)` in `hp_net_init`.

There is no challenge-specific C code and no `io_uring`; the `-luring` link dependency is gone.

## Optimizations

- **SQL renders the JSON.** `/feed` and `/posts/:id` bodies are produced by `json_object` / `json_group_array(...)` inside SQLite, so the server never re-serializes rows. The feed's `ORDER BY` lives in the input subquery and `json_group_array` preserves that order, which avoids a second sort.
- **Group commit.** Writes collected during one poll cycle are applied in a single `BEGIN IMMEDIATE … COMMIT`; responses are only sent after `COMMIT` succeeds (all flipped to 500 on rollback). Reads are answered immediately.
- **WAL + `synchronous=NORMAL`**, `temp_store=MEMORY`, a 64 MB page cache, 256 MB mmap.
- **TCP_NODELAY** on accepted sockets; idle connections are held for 90 s (spec asks ≥ 65 s).
- A strict RFC 8259 parser for POST bodies (the runtime's `json_decode` is lenient).

## Requirements

- SQLite ≥ 3.44 (for `ORDER BY` inside `json_group_array`'s input and JSON1). Ubuntu 24.04 ships 3.45.
- `libsqlite3-dev`, `build-essential`.

## Run

```bash
bash install.sh   # as root, once
bash build.sh     # as the build user
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 ./bin/server
```
