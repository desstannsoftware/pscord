# Contributing to PSCord

Thanks for helping. The primary build path is **GitHub Actions** — you do not need
to install a PS5 SDK or Docker on your machine.

## How to contribute

1. Open an issue (optional) or a pull request against `main`.
2. Keep the change focused; unrelated work belongs in another PR.
3. Let CI finish. The **Build** workflow runs lint, host tests, and packages
   `PPSA99190.zip`. Download that artifact to test on a console.
4. Fill in the PR template. Agree to license your contribution under this
   repository's licence (GPL-3.0-or-later).

See [docs/PULL_REQUEST_BUILDS.md](docs/PULL_REQUEST_BUILDS.md) for artifact names
and how reviewers install a PR build.

Do **not** commit `.env`, `build/`, `dist/`, `.deps/`, `.home/`, proprietary PRXs,
SDK binaries, console dumps, keys, credentials, or personal paths and emails.

## Optional local checks

If you already have a Linux/WSL host with the native toolchain, you may run:

```bash
make lint
make test-unit
make app   # produces dist/PPSA99190/
```

This is optional. A green Actions run is enough for review. Platform docs under
[docs/platform/](docs/platform/) describe the toolchain for maintainers who need
it; contributors should prefer CI.

## Code style

Files that already carry a project header keep it. New PSCord-owned files can use:

```cpp
// PSCord - One-line description of the file.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: GPL-3.0-or-later
```

Files inherited from `ps5-native-app-boilerplate` / `ps5-homebrew-ui` keep their
original BlackBearReloaded headers. Vendored code under `src/third_party/` and
`third_party/` keeps upstream licences and is listed in `THIRD_PARTY_NOTICES.md`.

## Releases (maintainers)

- Tag `NN.NNN.NNN` matching `sce_sys/param.json` `contentVersion` → CI publishes
  a GitHub Release with `PPSA99190.zip`.
- OTA to desstannsoftware.com (`tools/release.sh --publish`) needs maintainer
  secrets and is **not** run from public CI.
