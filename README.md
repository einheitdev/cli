# einheit-cli

Shared C++ framework for every Einheit Networks product CLI.

One framework, one wire protocol, many product adapters.

Licensed under the MIT License — see [LICENSE](LICENSE).

## Build

```bash
sudo apt install -y libzmq3-dev libsodium-dev libyaml-cpp-dev \
  libreadline-dev pkg-config cmake ninja-build clang

cmake --preset default
cmake --build build --parallel
ctest --preset default
```

## Layout

- Framework core lives under `include/einheit/cli/` and `src/`.
- Each product adapter is a self-contained subdirectory under
  `adapters/` that links against `einheit_cli`.
- The top-level `einheit` binary is assembled in
  `binaries/einheit/src/main.cc`.

See `CLAUDE.md` for conventions and `EINHEIT_CLI_FRAMEWORK.md` for
the design spec.
