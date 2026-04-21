# CLAUDE.md

Guidance for Claude Code working in this repository.

## Project overview

`einheit-cli` is the shared C++ CLI framework every Einheit Networks
product's command-line interface links against. See the full design
in `EINHEIT_CLI_FRAMEWORK.md` (in the project parent).

The framework is a protocol client. Each product daemon owns config
state, commit lifecycle, and audit logging; the CLI sends requests
over ZMQ and renders responses.

## Build

```bash
cmake --preset default        # Ninja, Release
cmake --build build --parallel

# Debug
cmake --preset debug
cmake --build build-debug --parallel

# Tests
ctest --preset default
```

System dependencies (install before `cmake`):

```bash
sudo apt install -y \
  libzmq3-dev libsodium-dev libyaml-cpp-dev \
  libreadline-dev pkg-config cmake ninja-build clang
```

FetchContent pulls: cppzmq, msgpack-cxx, FTXUI, spdlog, CLI11,
GoogleTest.

## Layout

- `include/einheit/cli/` — public framework headers
  - `protocol/envelope.h` — `Request` / `Response` / `Event`
  - `protocol/msgpack_codec.h` — codec free functions
  - `transport/transport.h` — abstract `Transport`
  - `transport/zmq_local.h` — `ipc://` + SO_PEERCRED
  - `transport/zmq_remote.h` — `tcp://` + CurveZMQ (Phase 4)
  - `render/terminal_caps.h`, `render/table.h` — semantic cells
  - `schema.h`, `command_tree.h`, `adapter.h` — schema / dispatch
  - `shell.h`, `history.h`, `aliases.h`, `auth.h`, `audit.h`
  - `error.h` — `Error<E>` template paired with `std::expected`
- `src/` — implementations mirror headers
- `adapters/example/` — copy/paste template for product adapters
- `binaries/einheit/src/main.cc` — appliance entry point
- `tests/` — GoogleTest; each `test_*.cc` is its own executable
- `cmake/` — sub-library definitions + third-party fetches

## Code conventions

- **C++23**, Google Style Guide, 80-char lines, 2-space indent.
- `clang` (not gcc) selected in `CMakeLists.txt:5-19`.
- **Trailing return types** on every function:
  `auto Foo() -> std::expected<T, Error<E>>`.
- **Data-oriented**: plain structs for data; free functions operate
  on references. Only `Transport` and `ProductAdapter` have virtuals
  (they define cross-module contracts).
- **Error handling**: `std::expected<T, Error<E>>` everywhere.
  Declare a module-local `enum class FooError` and pair with `Error<>`.
- **Namespaces**: `einheit::cli::`, `einheit::cli::protocol::`,
  `einheit::cli::transport::`, `einheit::cli::render::`,
  `einheit::cli::schema::`, `einheit::cli::shell::`,
  `einheit::cli::auth::`, `einheit::cli::audit::`. Adapters use
  `einheit::adapters::<name>::`.
- **Doxygen**: `/// @file`, `/// @brief`, `///` on every public
  header member. Use `///`, never `/** */`.
- Include guards: `INCLUDE_EINHEIT_CLI_<MODULE>_<FILE>_H_`.

## Testing

- Every new module gets a `tests/test_<module>.cc` executable.
- Round-trip tests for codecs, golden-file tests for rendering,
  fake-daemon fixture tests for end-to-end CLI/daemon flows.
- Use `--color=never --width=80 --ascii` for deterministic golden
  files.

## Build anti-patterns

- **Never FetchContent libzmq** — use `libzmq3-dev`. Source builds
  explode compile time and cause CMake export conflicts.
- **libsodium stays system** — operators want crypto primitives
  patched by the OS distribution, not frozen at build time.
- **Compiler selection must be before `project()`**
  (`CMakeLists.txt:5-19`). Setting `CMAKE_CXX_COMPILER` after
  `project()` breaks toolchain detection.
- **No `// removed` comments, no unused `_foo` renames.** If code is
  gone, delete it.

## Phases

Phase 1 — protocol + framework core + one adapter. Current state.

Phase 2 — add HD, F adapters.
Phase 3 — `commit confirmed N`, `watch`, `logs --follow`, roles.
Phase 4 — remote CurveZMQ transport (`transport/zmq_remote.cc`).
Phase 5 — external auth, SIEM, commit signing.
