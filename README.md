# project-atlas

A **C++20 MMO game-server framework** — not a game. The server core is the product; a client can be
swapped onto it, and the demo game exists only to exercise the core.

The core is what is being built: async TCP I/O on Boost.Asio, a generated binary protocol, a strand
based thread model, a custom ORM over MySQL, and an Actor / AoI / behaviour-tree world loop —
proven under load rather than described.

> **Status: implementation just started.** The build chain, the type-alias layer and the test gate
> are in place. The servers themselves are not built yet.

## Layout

```
server/     C++ framework. CMake root, setup.bat, db/schema.json, generated/{pkt,db}
  atlas/core/   fixed-width type aliases, chrono aliases      (include root is server/)
  atlas/net/    asio aliases
  generated/    generator output — 🔴 never hand-edited
  tests/        GoogleTest
shared/     contracts/ (*.cs → packet input) · datas/ (*.csv → static-data input)
tools/      Node generators (pkt, db) + the normalized-type SoT
docs/       design/architecture-design.md · conventions/cpp-style.md  (Korean)
```

## Getting started (Windows)

One command. It finds the Visual Studio 2022 C++ toolset, installs vcpkg, pins the dependency
baseline, installs the Node tooling and configures CMake:

```
server\setup.bat
```

🔴 The first run compiles Boost, OpenSSL, MySQL client, spdlog and GoogleTest from source —
**budget 20-60 minutes.** Later runs hit vcpkg's binary cache and finish in seconds. The script is
idempotent, so re-running it after a failure is safe.

Requirements: Visual Studio 2022 with *Desktop development with C++*, *C++ CMake tools for Windows*
and *C++ Clang tools for Windows*; Node.js 22+; git.

## Build and test

Run from `server/`:

```
cmake --preset windows-ci               # configure once per tree (setup.bat did windows-debug)

cmake --build --preset windows-debug    # unity build ON  — the fast dev loop
cmake --build --preset windows-ci       # unity build OFF — the missing-include / ODR gate
ctest  --preset windows-ci --output-on-failure
```

Presets: `windows-debug` · `windows-ci` · `linux-release` · `linux-ci`. All use the Ninja generator,
because only Ninja emits the `compile_commands.json` that clang-tidy needs.

🔴 Both unity modes are kept on purpose. Unity build hides missing `#include`s by letting a sibling
file in the same batch supply the header; the unity-OFF build is what catches that, plus ODR
collisions. Never skip it.

## The gate

```
powershell -NoProfile -File server\scripts\ci-gate.ps1
```

Runs `gen:check → format-check → clang-tidy → build(unity ON) → build(unity OFF) → test`.
`.github/workflows/ci.yml` is the Linux transcription of the same order.

🔴 `clang-tidy` is the one step this local gate does not enforce — it is skipped on Windows (with a
message saying so) and runs only on `linux-ci`. clang-tidy cannot read the MSVC precompiled header
the Ninja/`cl` tree produces. See `docs/conventions/cpp-style.md §7.3`.

## Code generation

Packets, DB access and static data are **generated**, which is the seam that lets a different game
be dropped onto the same core. Run from the repo root:

```
npm run gen:all      # regenerate everything
npm run gen:check    # drift gate — fails if generated output no longer matches its input
```

🔴 Nothing under `server/generated/` is ever edited by hand. Change the input
(`shared/contracts/*.cs`, `server/db/schema.json`) and re-run the generator.

## Documentation

| doc | what |
|---|---|
| [`docs/design/architecture-design.md`](docs/design/architecture-design.md) | architecture SoT — topology, protocol, thread model, ORM, build chain |
| [`docs/conventions/cpp-style.md`](docs/conventions/cpp-style.md) | coding convention SoT and its mechanical enforcement |

## License

[MIT](LICENSE).
