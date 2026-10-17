# Lua hook tests

End-to-end tests for the Lua layer (`src/map/skill_lua.cpp`). Each run starts
this checkout's login, char and map servers with one of the example mods from
[ragnarokoffline.app](https://github.com/Flux159/ragnarokoffline.app), and a
headless client logs one character in. A scenario NPC then plays the test:
it grants skills, spawns test monsters and makes the character use its
skills. Test mods loaded after the example log what the hooks are given and
what they return. A checker reads the map server's log.

Nothing here is built, installed or loaded by the servers. CI doesn't run it,
since it needs a database and the app checkout.

| Suite | Loads | Covers | Checks |
|---|---|---|---|
| `custom-skills` | `examples/mods/custom-skills` (a Guillotine Cross rework), and the test mods `zz-observe`, `zz-first` and `zz-probe` | taking skills away and refunding them; each kind of custom skill; passives; a mod's statuses with stacks, decay and caps; the hook chains and their priorities; and, through `zz-probe`'s probe skills, the queries, actions and hooks the example has no use for, down to a status kept over a logout (the client logs out and back in) | 106, renewal and pre-renewal |
| `third-class-sampler` | `examples/mods/third-class-sampler` (about twenty skills of the thirteen third classes with a new effect on their own IDs), and the test mod `zz-watch` | each of those effects, from the outside | 43, renewal only |

## What you need

- This checkout, built: `login-server`, `char-server` and `map-server` in its
  root (`./configure && make server`, or CMake). Keep the default
  `PACKETVER`, since the client speaks 20211103.
- MySQL or MariaDB with rAthena's database: `sql-files/main.sql` loaded into
  `ragnarok`, reachable the way `conf/inter_athena.conf` says (127.0.0.1,
  `ragnarok`/`ragnarok`). If your checkout has a `conf/import/inter_conf.txt`,
  the test world uses it too. **A run replaces account 2000000 (`tester`) and
  character 150000 (`Tester`)**, so point it at a test database, not a live
  server's.
- bash, the `mysql` client and Python 3 (standard library only).
- A ragnarokoffline.app checkout, by default next to this one
  (`../ragnarokoffline.app`).
- The servers' default ports (6900, 6121, 5121) free.

## Running

```sh
tools/lua-test/run.sh custom-skills
tools/lua-test/run.sh third-class-sampler
```

Each suite takes about two minutes. The output is one line per check and a
total (`106 passed, 0 failed`, `43/43 passed`). The exit status is 0 when every
check passed. The world and its logs stay in `tools/lua-test/work`
(`log/map.out` is what the checker reads) until the next run.

Settings come from the environment:

| Variable | Default | What it is |
|---|---|---|
| `RATHENA` | this checkout | where `conf/` and `db/` come from |
| `BIN` | `$RATHENA` | where `login-server`, `char-server` and `map-server` are |
| `APP` | `../ragnarokoffline.app` | the app checkout with `examples/mods` |
| `WORK` | `tools/lua-test/work` | the throwaway world, rebuilt on each run |
| `ERA` | `re` | `prere` for a pre-renewal build |
| `MYSQL` | `mysql -h 127.0.0.1 -u ragnarok -pragnarok ragnarok` | the command that resets the test character |
| `SECONDS_LIMIT` | `300` | how long the client waits for the scenario to end |

### Pre-renewal

Build pre-renewal servers in a directory of their own, then point `BIN` at
it:

```sh
cmake -B build-prere -DCMAKE_CXX_FLAGS=-DPRERE
cmake --build build-prere --target login-server char-server map-server
mkdir -p ../bin-prere && cp login-server char-server map-server ../bin-prere/
ERA=prere BIN=../bin-prere tools/lua-test/run.sh custom-skills
```

CMake builds into the checkout's root and `lib/` for every build directory.
Rebuild the renewal servers afterwards, and run `rm -f lib/*.a` before
switching eras, or the libraries of the other era are linked in. The run
checks which era the servers were built for and stops if `ERA` doesn't
match. Pre-renewal has no Guillotine Cross, so the `custom-skills` suite puts
the example's tree on the Assassin Cross there (`prere_tree.py`,
`reset-prere.sql`).

## How a run works

1. `run.sh` builds the world in `WORK`. It links the binaries and `db/`,
   copies `conf/`, and gives the world a `db/import` of its own, laid out the
   way the app installs a mod: its `db/` files in `db/import`, its `lua/`
   under `db/import/lua/<mod>`, and a `load.txt` in load order. The suite's
   `setup.sh` adds the example mod, the test mods, the test skills
   (`test_skills.yml`), monsters (`mob_db.yml`) and the scenario NPC.
2. It resets the test account and character (`reset.sql`, then
   `reset-<era>.sql` when the suite has one).
3. It starts the servers. `client.py` logs in as `tester`/`tester123`, selects
   `Tester` and enters the map. The scenario NPC's `OnPCLoginEvent` runs
   the test. When a step needs something only a player can do (use an item,
   pick from a crafting menu, log out and back in), the scenario asks the
   client in the log (`S|client use 1`, `S|client relog`).
4. The scenario logs `S|` lines (`debugmes`). The test mods log `T|`, `P|`,
   `V|` or `W|` lines through `log()`. After the servers stop, the suite's
   `check.py` reads them from `log/map.out`. It also fails on any Lua error.

## Adding a test

- **A scenario step**: `debugmes "S|step <name>";`, then the actions, then
  `debugmes "S|<key> <values>";` for what the checker compares. A skill with
  no cast time runs inside `unitskilluseid`, so its results can be read on
  the next line.
- **A probe**: when a script can't reach something, add a test skill to
  `test_skills.yml` and hook it in a test mod. The scenario casts it at the
  level that picks the probe. The kit's skill IDs are 3188–3199; the example
  mods use 3100–3106.
- **A check**: `check(name, ok, detail)` in the suite's `check.py`.
