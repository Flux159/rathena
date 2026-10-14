# Working in this repository

This is **akreao/rathena**, a fork of rAthena. You are on (or should be on) its **`rag-wrapper`** branch: the server side of [rag-wrapper](https://github.com/akreao/rag-wrapper), a Linux desktop app that builds rAthena from this branch in Docker and runs it, with MariaDB, for the real Ragnarok Online client on a LAN. It is not upstream rAthena, and it is not Ragnarok Offline.

**Not on `rag-wrapper` right now?** Run `git switch rag-wrapper` before you change anything for rag-wrapper. The fork's other branches (`ragnarokoffline` and its feature branches) belong to a different project; leave them alone.

## Pull requests go to `rag-wrapper`

| Branch | What it is | Open PRs against it? |
|---|---|---|
| `rag-wrapper` | rag-wrapper's server branch: upstream rAthena plus rag-wrapper's server changes. The app builds a pinned commit of it. | **Yes. Every rag-wrapper PR goes here.** |
| `master` | A mirror of upstream rAthena's `master`. | **No.** |
| `ragnarokoffline` | Another project's branch, unrelated to rag-wrapper. | **No.** |

GitHub suggests `master` as the base, because it is the repository's default branch. Change it to `rag-wrapper` before you open the PR.

How to work:
- Branch from `rag-wrapper`: `git fetch origin && git switch -c my-change origin/rag-wrapper`.
- Keep a PR to one change.
- `rag-wrapper` only moves forward: no force-pushes, no rebasing. The app pins commits on it, and a rewritten commit breaks that pin.
- Newer upstream comes in as a merge of `master` into `rag-wrapper`, in its own PR.
- To bring your branch up to date, merge or rebase onto `origin/rag-wrapper`, never onto `master`.

## How the app uses this branch

The app's Dockerfile (`docker/rathena/Dockerfile` in akreao/rag-wrapper) clones this repository at the commit pinned in the app's `.env.example` (`RATHENA_COMMIT`) and runs `./configure --enable-packetver=$PACKETVER && make server`. It copies `conf/`, `db/`, `npc/` and `sql-files/` into the image, and its `entrypoint.sh` writes the server configs from environment variables at start-up.

So:
- **Merging here changes nothing for players until the app's pin moves** to the new commit, in a PR to rag-wrapper. Mention the commit to pin in your PR.
- Settings the app should control belong in a config option, which `entrypoint.sh` can set, rather than a hard-coded value.
- On first start the app's `db-init` service imports `sql-files/main.sql` (and the other base schema files) into an empty database. On later starts it applies every `sql-files/upgrades/*.sql` it hasn't run yet and records each one, so an upgrade file is how a schema change reaches existing installs. Never edit an upgrade file after it is merged: installs that already ran it won't run it again.
- Passwords are stored as MD5 (`use_MD5_passwords: yes`), and the app's web portal writes accounts to the same `login` table. Don't change the account schema without a matching change in the portal.

The app's own decisions are in `docs/decisions.md` in akreao/rag-wrapper. Read it before changing behaviour the app relies on.

## Conventions

- Follow upstream rAthena conventions: coding style, the YAML DB layout, the pre-renewal/renewal split (`db/pre-re`, `db/re`, `src/config/renewal.hpp`), @command patterns.
- **A schema change** goes in `sql-files/main.sql` *and* a new `sql-files/upgrades/upgrade_YYYYMMDD.sql`.
- Keep core edits small and prefer a config option to an unconditional change, so upstream merges stay easy.

## Checking a change

- Build: `./configure && make server`, or CMake (`cmake -B build && cmake --build build`). Add `--enable-prere` for pre-renewal and `--enable-packetver=20220406` to match the app's default client.
- A quick compile check of one file: `c++ -std=c++20 -fsyntax-only -Isrc -I3rdparty -I3rdparty/rapidyaml/src -I3rdparty/rapidyaml/ext/c4core/src -I3rdparty/libconfig -I3rdparty/lua -I3rdparty/httplib -I3rdparty/json/include -I3rdparty/yaml-cpp/include -I3rdparty/mysql/include -I3rdparty/pcre/include -I3rdparty/zlib/include <file>`.
- To test inside the app, set `RATHENA_COMMIT` in the app's `.env` to your commit and run `docker compose up -d --build`.
