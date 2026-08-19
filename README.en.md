# project-atlas

A **C++20 MMO game-server framework** — not a game. The server core is the product: any client can be
swapped onto it, and the demo game (one character row, one item table, three equip slots) exists only
to exercise core paths that would otherwise stay unproven. What it claims is *measured* server
behaviour — a load harness, a bottleneck named with its evidence, and a gate that fails the build —
rather than described architecture. It is not a game engine, not a live service, and revenue is
explicitly not a goal.

> **Two lists, deliberately kept apart.** [What runs](#what-runs-today) is code you can start in
> five commands. [What is only designed](#not-built-yet) is at the bottom, named as design. The
> architecture doc describes the whole topology (FE / GAME / WORLD, Actor / AoI / behaviour trees,
> Relay); most of that is **not implemented**, and this README never counts a design as an asset.

## What runs today

| layer | what is implemented |
|---|---|
| `server/atlas/net` | Boost.Asio acceptor · session · io_runner. Strand-serialised sessions, idle timeout, accept backoff, `TCP_NODELAY` |
| `server/atlas/proto` | 12-byte binary frame (`length · opcode · seq · crc32`), per-field write/read (no `#pragma pack`), per-direction sequence numbers, framing checksum |
| `server/atlas/db` | Custom ORM runtime over MariaDB Connector/C: connection pool with lease timeout, prepared statements, transaction RAII, a dedicated DB thread pool with a **bounded queue + load shedding**, idempotency keys and a `None → Received → Persisted → Responded` request state machine, post-commit compensation guard |
| `server/atlas/redis` | boost.redis read cache: read-through character cache with write **invalidation**, `ZADD`/`ZREVRANGE` ranking. Redis down or unconfigured is a cache miss, never an outage. No `PUBLISH`/`SUBSCRIBE` wrapper exists — inter-server game traffic over Redis is forbidden by design |
| `server/atlas/config` · `core` | `server.ini` (committed) / `.env` (secrets) split, secret **key names** logged and values never; fixed-width type aliases, strong-typed ids, a ctx ledger (trace / account / character / transaction state) carried across thread boundaries, spdlog macros, crash diagnostics that resolve to `file:line` with the image's build-id |
| `server/game` | **The GAME binary.** Three request/response opcode pairs — character load, equip, ranking — plus the per-character lock and the equip transaction |
| `server/generated/{info,pkt,db}` | Generator output. Never hand-edited; `gen:check` is the mechanical enforcement |
| `server/console_client` · `server/loadgen` | An interactive REPL client, and the load harness (closed loop · open loop · ramp, JSONL samples, live TUI) |
| deploy | Multi-stage Dockerfile (`builder → runtime → symbols`), one image + `ATLAS_ROLE` entrypoint, `compose.yaml` with mysql + redis by default and the server behind the `app` profile |

**Server authority is real.** `shared/datas/item.csv` gives `item_id → allowed slot`, and the equip
path refuses a client's claim on both ownership and slot. The framing checksum is **not** tamper
protection and is not presented as such; the session-key HMAC layer is **not implemented** and the
header carries no placeholder field for it.

## Run it

Windows host, Docker Desktop. One command installs the toolchain (VS 2022 C++ toolset → vcpkg clone
and bootstrap → pinned baseline → `npm ci` → CMake configure). The first run compiles Boost, OpenSSL,
the MySQL client, spdlog and GoogleTest from source — **budget 20–60 minutes**. It is idempotent.

```powershell
server\setup.bat
cd server
cmake --preset windows-ci                # configure the unity-OFF tree once
cmake --build --preset windows-ci        # builds atlas_console and atlas_loadgen too
cd ..                                    # everything below runs from the repo root
```

`windows-debug` (unity ON) is the fast dev loop; `windows-ci` (unity OFF) is the tree the gate and
the load measurements use, so the paths below point at it. Both are Debug builds — there is no
Windows Release preset, and §16.1 records that as a condition of every number it reports.

### 1. Bring the stack up

Credentials live only in `server/.env`; the repo ships `.env.example` with everything but the two
passwords. `--env-file server/.env` is **not optional** — compose reads `${...}` substitutions
from the project env-file, not from a service's `env_file:`, and the required keys are `${VAR:?}` so
a missing one is a named error rather than a silent empty default.

```powershell
cp server\.env.example server\.env      # then fill ATLAS_DB_PASSWORD and ATLAS_DB_ROOT_PASSWORD

# Version stamping. Not in .env: it changes every build, and a file would freeze yesterday's value.
$env:ATLAS_GIT_SHA   = (git rev-parse --short HEAD)
$env:ATLAS_BUILD_TIME = (Get-Date).ToUniversalTime().ToString('s') + 'Z'

docker compose --env-file server\.env --profile app build server
docker compose --env-file server\.env --profile app up -d
docker run --rm --entrypoint cat project-atlas-server /app/VERSION
```

`/app/VERSION` is the guard against a Docker layer cache deploying yesterday's binary, and the same
sha feeds the crash reporter's build-id:

```
revision=d2c321a
built=2026-08-13T15:37:43Z
```

```powershell
docker logs project-atlas-server-1 --tail 12
```

```
revision=d2c321a
built=2026-08-13T15:37:43Z
[I] [crash.cpp:257] crash diagnostics ready [build=d2c321a directory=logs/crash]
[I] [secret_config.cpp:106] secrets loaded: ATLAS_DB_HOST=<set>, ATLAS_DB_PORT=<set>, ATLAS_DB_NAME=<set>, ATLAS_DB_USER=<set>, ATLAS_DB_PASSWORD=<set>, ATLAS_JWKS_URL=<empty>, ATLAS_REDIS_HOST=<set>, ATLAS_REDIS_PORT=<set>, ATLAS_REDIS_PASSWORD=<empty>
[W] [main.cpp:104] ATLAS_DB_TLS_NO_VERIFY=1 — the database connection is encrypted but the server certificate is NOT verified. Local/compose only; never production.
[I] [connection_pool.cpp:48] db connection pool ready: 4 connections
[I] [acceptor.cpp:33] acceptor listening on 0.0.0.0:7777
[I] [db_runner.cpp:32] db runner started: 2 threads, queue cap 128
[I] [handlers.cpp:232] GAME listening on 0.0.0.0:7777 (server_id=1, io_workers=16, db_threads=2)
```

Note what the log says about itself: secret **names** are printed and values never, and the TLS
relaxation this stack ships with announces itself as a warning on every boot.

### 2. Seed one character

`schema.sql` creates tables and inserts nothing — there is no account service in this repo, and the
load harness seeds and then deletes its own block. So the console demo needs one row:

```powershell
$seed = @'
REPLACE INTO characters (server_id,character_id,account_uid,name,pos_x,pos_y,level,exp,created_at)
  VALUES (1,700001,700000,'console-demo',12,34,7,4242,NOW());
REPLACE INTO character_items (server_id,character_id,item_uid,item_id,stack_count,equip_slot) VALUES
  (1,700001,7000011,1001,1,1),
  (1,700001,7000012,1002,1,0),
  (1,700001,7000013,2001,1,2);
'@
$seed | docker exec -i project-atlas-mysql-1 sh -c 'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" "$MYSQL_DATABASE"'
```

Item ids come from `shared/datas/item.csv`: `1001`/`1002` are weapons (slot 1), `2001` is armor
(slot 2).

### 3. Drive it

```powershell
server\build\windows-ci\console_client\atlas_console.exe --host 127.0.0.1 --port 7777
```

Every command is a real frame on a real socket; there is no command whose reply the client invents.
An actual session — equip a weapon (three writes in one transaction), then try to put the armor in
the weapon slot:

```
connected to 127.0.0.1:7777
atlas> load 700001
character 700001 account=700000 name=console-demo level=7 exp=4242 pos=(12,34)
inventory (3 items)
  uid=7000011 item_id=1001 stack=1 slot=1(weapon) csv_slot=1
  uid=7000012 item_id=1002 stack=1 slot=0(none) csv_slot=1
  uid=7000013 item_id=2001 stack=1 slot=2(armor) csv_slot=2
atlas> equip 7000012 1
equip ok: uid=7000012 -> slot 1 (uid=7000011 was unequipped)
atlas> inv
inventory (3 items)
  uid=7000011 item_id=1001 stack=1 slot=0(none) csv_slot=1
  uid=7000012 item_id=1002 stack=1 slot=1(weapon) csv_slot=1
  uid=7000013 item_id=2001 stack=1 slot=2(armor) csv_slot=2
atlas> equip 7000013 1
equip refused: item does not go in that slot (uid=7000013, slot 1) — connection stays up
atlas> rank 5
ranking (1 entries, highest exp first)
  #1 character=700001 exp=4242
atlas> quit
```

The refusal is the point: the slot rule lives on the server, the client sends the request unfiltered,
and a rejected request leaves the connection up. `rank` is answered from the Redis sorted set, which
is written on the line after `Commit()` and never before it.

### 4. Put load on it

The harness seeds its own character block over a direct DB connection, so it needs the same
credentials the server has. Load `server\.env` into the shell and override the host — the value in
`.env` is a compose service name and does not resolve outside the network:

```powershell
Get-Content server\.env -TotalCount 200 | Where-Object { $_ -match '^\s*[A-Z]' } |
    ForEach-Object { $k,$v = $_ -split '=',2; Set-Item "env:$k" $v }
$env:ATLAS_DB_HOST = '127.0.0.1'

server\build\windows-ci\loadgen\atlas_loadgen.exe --connections 64 --duration 45 --warmup 15 `
    --io-threads 8 --host 127.0.0.1 --port 7777 --server-id 1 --sample-jsonl reports\run.jsonl
node tools\loadreport\loadreport.js --in reports\run.jsonl
```

```
seeded 64 characters (server_id=1, character_id 900000..900063)
established=64 peak_live=64 connect_fail=0 transport_fail=0 load_fail=0
requests_sent=10255 ok=10255 unavailable=0 refused=0 load_rejected=0
steady_window_ms=30000 samples=4732 throughput_rps=157.7
p50_us=476884 p90_us=536277 p99_us=591742 max_us=600765
seeded rows removed
```

The report is invoked as `node tools\loadreport\loadreport.js`, not `npm run loadreport -- …`:
npm's PowerShell shim drops the `--` separator and the script then sees a bare path instead of
`--in`. The seeded block is deleted on exit, in MySQL and in Redis both.

`--ramp 32,64,128,192,256 --stage-seconds 20` runs the staged version instead. Read
[§16.1 of the design doc](docs/design/architecture-design.md) before trusting any number you get:
this host has two disk regimes and a run that crosses them measures neither.

## How it is verified

```powershell
powershell -NoProfile -File server\scripts\ci-gate.ps1
```

| step | what it proves |
|---|---|
| `gen:check` | Generated output still matches its input. The mechanical enforcement of "never edit generated output" — a doc rule alone always ends with a hand-edited generated file committed |
| `core-purity` | `server/atlas/**` includes no game-contract header and contains no demo-game vocabulary. The mechanical enforcement of "the core must not know the game" |
| `format-check` | `clang-format` |
| `clang-tidy` | **Skipped locally, with a printed reason.** clang-tidy cannot read the MSVC precompiled header the Ninja/`cl` tree emits, so this step grades on Linux CI only |
| `build` (unity ON) | The fast dev configuration |
| `build` (unity OFF) | Missing `#include`s and ODR collisions. Unity builds hide both by letting a sibling in the same batch supply a header — never skip this one |
| `test` | GoogleTest via `ctest`. The gate loads `server/.env` and **fails when a test was skipped while the database was reachable** — `ctest` exits 0 on a skip, so without this it printed PASS having proven nothing |

Last local run: **140/140 passed, 0 skipped, `[gate] PASS`.**

**The two gates are not equivalent, in both directions.**

- **CI has no MySQL service.** Every DB and Redis suite skips there, so the persistence axis is
  proven by a **local** gate run against a reachable database — that is what the skip-is-failure rule
  above exists for. A green CI badge says nothing about the ORM.
- **The local gate never runs `clang-tidy`.** Naming, `bugprone` and `modernize` violations surface
  only in CI. `server\scripts\tidy-prefilter.ps1` is a local pre-filter over a PCH-stripped copy of
  `compile_commands.json`, but it is deliberately **not** a gate: it always exits 0, and the two
  toolchains disagree both ways — CI's libstdc++ reports optional-access cases MSVC's STL does not,
  and CI never compiles the `#if defined(_WIN32)` branches only this sweep can grade.
- Even `format-check`, which runs in both, can disagree: the local `clang-format` ships with VS and
  CI's comes from `apt.llvm.org`.

A green local gate is not evidence of a green CI, and neither is evidence of the other.

## Load measurement

Numbers below are quoted from
[§16.1 of the design doc](docs/design/architecture-design.md); **the conditions are part of the
result and are not separable from it.**

**Conditions.** i5-12600KF (10 cores / 16 logical) · 32 GB · Windows 11. Server in a container on
Docker Desktop / WSL2 (16 vCPU), image built from the `linux-release` preset (optimised). MySQL
8.4.10 in a container on the same host at image defaults — `innodb_flush_log_at_trx_commit=1`,
`sync_binlog=1`, `log_bin=ON`, `innodb_flush_method=O_DIRECT`. Server runtime `io_workers=16`,
`db_threads=2`, `db_pool_size=4`, DB queue cap 128. **The load client is a Debug, non-optimised
build** (`windows-ci` is the only Windows preset; there is no Windows Release preset) running
natively **on the same host as the server and the database**, over loopback. Every table is a
median over ≥3 independent runs with the observed min–max, and a host `fsync` probe is taken before
and after each run; runs that crossed a regime mid-flight were discarded and recorded as discarded.

**① There are two disk regimes on this host, and that fact dominates everything else.**

| regime | fsync probe | 64-connection closed-loop ceiling | entered by |
|---|---:|---:|---|
| settled | 3.7 – 4.9 ms | **129.6 – 142.3 req/s** (median 139.5, 9 runs) | ~35–40 s of saturating load |
| idle-recovered | 1.0 – 1.5 ms | **357 – 463 req/s** (median 445, 6 runs) | minutes idle, or sustained below capacity |

Probe ratio 3.7×, throughput ratio 3.3× — the same size. The server did not change; the time one
commit waits on disk did. **Settled is the honest number**, because idle-recovered burns off in
under a minute of real load. Everything below is settled-regime.

**② Throughput does not respond to concurrency; latency is pure queueing.**

| concurrent connections | throughput (median) | p50 | p99 | `N ÷ throughput` |
|---:|---:|---:|---:|---:|
| 1 | 92.7 req/s | 10.2 ms | 17.5 ms | (unsaturated) |
| 2 | 127.8 req/s | 15.1 ms | 24.2 ms | 15.6 ms |
| 8 | 129.5 req/s | 60.4 ms | 79.0 ms | 61.8 ms |
| 64 | 139.5 req/s | 454 ms | 581 ms | 459 ms |
| 256 | 134.5 req/s | 1883 ms | 2214 ms | 1903 ms |

64× the concurrency, ±2 % the throughput, 63× the latency. Little's Law closes to within **2.1 %**
across the whole sweep — the knee is at concurrency **2**, and everything past it is queue.

**③ The bottleneck, named with its evidence.** Not CPU (request-side CPU is identical in both
regimes and totals 0.4 of 16 cores), not lock contention (`Innodb_row_lock_waits = 0` across all 54
runs), not InnoDB stalls (`Innodb_log_waits = 0`, flat dirty pages). It is
**`db_threads = 2` × 2.1 fsyncs per commit × the fsync latency of the moment**: `redo` and `binlog`
each sync, and MySQL never sees more than 2 concurrent transactions, so group commit has nothing to
batch. **No tuning round was run and none is claimed** — this is a diagnosis, not an optimisation.

**④ What a user experiences at a fixed rate** (open loop, 64 connections):

| target | achieved | p50 |
|---:|---:|---:|
| 110 req/s (83 % of capacity) | 110.1 | 13.1 ms |
| 130 req/s (98 % of capacity) | 130.0 | 22.7 ms |

**⑤ Past the ceiling, the queue cap turns latency divergence into rejection** (ramp, 4 runs):

| stage | connections | effective throughput | rejection rate | accepted p50 |
|---:|---:|---:|---:|---:|
| 2 | 128 | 129.5 req/s | 0.00 % | 972.9 ms |
| 3 | 192 | 114.9 req/s | **99.28 %** | 1111 ms |
| 4 | 256 | 111.0 req/s | **99.43 %** | 1145 ms |

Above the ceiling, accepted p50 stops tracking `N` and pins to `queue cap ÷ throughput` (0.3 % and
0.7 % error) — the queue got a lid. **Load shedding is not free**: effective throughput fell 14 %,
because a rejection still costs frame encoding and a socket write, and this harness has no backoff
so it answers each rejection instantly with another request. Client-counted and server-counted
rejections matched **exactly, 0 discrepancy, across all four runs** (676 875 · 646 641 · 613 163 ·
674 276).

## Code generation — the templatization seam

Packets, DB access and static data are generated, which is what lets a different game be dropped on
the same core. From the repo root:

```powershell
npm run gen:all      # info → db → pkt: input data, then schema, then packets
npm run gen:check    # drift gate; the first step of the CI gate
```

Nothing under `server/generated/` is edited by hand. Change `shared/contracts/*.cs`,
`shared/datas/*.csv` or `server/db/schema.json` and re-run the generator.

## Not built yet

Named here so that nothing above has to be discounted.

| not implemented | status |
|---|---|
| **FE and WORLD binaries** | Designed (topology, registry, heartbeat, dynamic attach/detach). The entrypoint maps `ATLAS_ROLE=fe`/`world` to a binary that does not exist and exits 69 rather than pretending |
| **Actor / AoI / behaviour-tree world loop** | Designed only. There is no tick loop, no spatial model and no BT engine in this repo. Movement and chat exist as generated packet contracts with no handler behind them |
| **Relay / Matching / InterWorld** | Phase 3. Only the seam is fixed: WORLD ↔ WORLD direct traffic is forbidden, and Redis pub/sub is forbidden as the bypass around it |
| **Session-key HMAC (integrity layer 2)** | Needs a session key, which needs the auth handshake. The frame header has no reserved field — a zero-filled one would look like protection |
| **JWKS / platform-auth integration** | Designed; `ATLAS_JWKS_URL` is read and unused |
| **Server cheat console · packet log** | Designed, not built |
| **Idempotency store persistence** | In memory. Production puts a table here; the demo table budget was already spent |
| **CD** | Out of scope for Phase 1 by decision. Deployment ends at `compose` on a single host |

## Documentation

Everything under `docs/` is **Korean**. `AGENTS.md` (English) is the routing index.

| doc | what |
|---|---|
| [`docs/design/architecture-design.md`](docs/design/architecture-design.md) | Architecture SoT — scope, topology, identity, protocol, thread model, ORM and cache, logging/exception policy, build chain, and §16.1: the full load measurement including the runs that were thrown away and why |
| [`docs/conventions/cpp-style.md`](docs/conventions/cpp-style.md) | Coding convention SoT and its three-layer mechanical enforcement |
| [`AGENTS.md`](AGENTS.md) | Repository map, mandates, and which document a given change is required to read first |

If a document and the code disagree, the code is the truth and the document is the bug.

## License

[MIT](LICENSE).
