#!/usr/bin/env bash
# Plays one suite of the Lua hook tests against this checkout's servers.
#
#   tools/lua-test/run.sh custom-skills
#   tools/lua-test/run.sh third-class-sampler
#   ERA=prere BIN=/path/to/pre-renewal/binaries tools/lua-test/run.sh custom-skills
#
# It builds a throwaway world in $WORK (this checkout's conf/ and db/, with the
# suite's example mod and test mods in db/import), resets the test account in
# the database, starts the login, char and map servers, logs in with a
# headless client, waits for the scenario NPC to finish, stops the servers and
# checks the map server's log. Exits 0 when every check passed. README.md has
# the setup.
set -euo pipefail

KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RATHENA=${RATHENA:-$(cd "$KIT/../.." && pwd)}
BIN=${BIN:-$RATHENA}
APP=${APP:-$(dirname "$RATHENA")/ragnarokoffline.app}
WORK=${WORK:-$KIT/work}
ERA=${ERA:-re}
MYSQL=${MYSQL:-mysql -h 127.0.0.1 -u ragnarok -pragnarok ragnarok}
SECONDS_LIMIT=${SECONDS_LIMIT:-300}
SUITE=${1:-}
SUITE_DIR=$KIT/$SUITE

die() { echo "lua-test: $*" >&2; exit 2; }

suites() { for f in "$KIT"/*/setup.sh; do basename "$(dirname "$f")"; done | paste -sd' ' -; }
[ -n "$SUITE" ] && [ -f "$SUITE_DIR/setup.sh" ] || die "usage: $0 <suite>, one of: $(suites)"
case $ERA in re|prere) ;; *) die "ERA is re or prere, not '$ERA'" ;; esac
for b in login-server char-server map-server; do
	[ -x "$BIN/$b" ] || die "no $BIN/$b: build the servers (README.md) or point BIN at them"
done
command -v python3 > /dev/null || die "python3 is needed"
$MYSQL -e "SELECT 1" > /dev/null 2>&1 || die "cannot reach the database with: $MYSQL"

# Helpers for the suites' setup.sh.

# install_mod <name> <dir>: a mod's Lua files, loaded in the order installed.
install_mod() {
	mkdir -p "$WORK/db/import/lua/$1"
	for f in "$2"/*.lua; do
		cp "$f" "$WORK/db/import/lua/$1/"
		printf '%s\tlua/%s/%s\n' "$1" "$1" "$(basename "$f")" >> "$WORK/db/import/lua/load.txt"
	done
}

# add_npc <file>: an NPC script the map server loads, in either era.
add_npc() {
	cp "$1" "$WORK/npc_test/"
	for era in re pre-re; do
		printf 'npc: npc_test/%s\n' "$(basename "$1")" >> "$WORK/npc/$era/scripts_main.conf"
	done
}

# append_body <src> <dst>: the entries under src's Body: go at the end of dst.
append_body() {
	grep -q '^Body:' "$2" || printf '\nBody:\n' >> "$2"
	sed '1,/^Body:/d' "$1" >> "$2"
}

# The world: this checkout's binaries, conf and db, and a db/import of its own.
if [ -e "$WORK" ]; then
	[ -f "$WORK/.lua-test-world" ] || [ -z "$(ls -A "$WORK")" ] \
		|| die "$WORK is not a world this script made; remove it or set WORK"
	rm -rf "$WORK"
fi
mkdir -p "$WORK/conf" "$WORK/db/import/lua" "$WORK/npc/re" "$WORK/npc/pre-re" "$WORK/npc_test" "$WORK/log"
touch "$WORK/.lua-test-world" "$WORK/db/import/lua/load.txt"
for b in login-server char-server map-server; do ln -s "$BIN/$b" "$WORK/$b"; done
cp -r "$RATHENA/conf/." "$WORK/conf/"
mkdir -p "$WORK/conf/import"
for f in "$RATHENA"/conf/import-tmpl/*; do
	[ -e "$WORK/conf/import/$(basename "$f")" ] || cp -r "$f" "$WORK/conf/import/"
done
for e in "$RATHENA"/db/*; do
	case $(basename "$e") in import|import-tmpl) ;; *) ln -s "$e" "$WORK/db/" ;; esac
done
cp "$RATHENA"/db/import-tmpl/* "$WORK/db/import/" 2> /dev/null || true
for era in re pre-re; do : > "$WORK/npc/$era/scripts_main.conf"; done
# Appended, so a conf/import of this checkout's own (database login, IPs) still counts.
printf 'pincode_enabled: no\nchar_maintenance: 0\n' >> "$WORK/conf/import/char_conf.txt"
printf 'new_account: no\n' >> "$WORK/conf/import/login_conf.txt"
# The tester learns from the job's own tree, whatever was spent before, and
# leaves at once when the client logs out, though it has just fought.
printf 'player_skillup_limit: no\nprevent_logout: 0\n' >> "$WORK/conf/import/battle_conf.txt"
touch "$WORK/conf/import/map_conf.txt"

CLIENT_ARGS=()
# shellcheck source=/dev/null
. "$SUITE_DIR/setup.sh"

$MYSQL < "$SUITE_DIR/reset.sql"
[ ! -f "$SUITE_DIR/reset-$ERA.sql" ] || $MYSQL < "$SUITE_DIR/reset-$ERA.sql"

cd "$WORK"
pids=()
stop() {
	[ ${#pids[@]} -gt 0 ] || return 0
	for p in "${pids[@]}"; do kill "$p" 2> /dev/null || true; done
	sleep 2
	for p in "${pids[@]}"; do kill -9 "$p" 2> /dev/null || true; done
	pids=()
}
trap stop EXIT
./login-server > log/login.out 2>&1 & pids+=($!)
./char-server > log/char.out 2>&1 & pids+=($!)
./map-server > log/map.out 2>&1 & pids+=($!)
echo "lua-test: $SUITE ($ERA), servers starting in $WORK"
for _ in $(seq 1 180); do
	grep -q "now online" log/map.out 2> /dev/null && break
	sleep 1
done
grep -q "now online" log/map.out || { tail -30 log/map.out; die "the map server did not come up (log in $WORK/log)"; }
built=re
grep -q "db/pre-re/" log/map.out && built=prere
[ "$built" = "$ERA" ] || die "the servers in $BIN are a $built build, but ERA=$ERA"
sleep 2
echo "lua-test: playing the scenario"
python3 "$KIT/client.py" --seconds "$SECONDS_LIMIT" --log log/map.out ${CLIENT_ARGS[@]+"${CLIENT_ARGS[@]}"} > log/client.out 2>&1 \
	|| echo "lua-test: the client stopped early, see $WORK/log/client.out"
stop
grep -q "S|end" log/map.out || echo "lua-test: the scenario did not finish within $SECONDS_LIMIT s"

ERA=$ERA python3 "$SUITE_DIR/check.py" "$WORK/log/map.out" && status=0 || status=$?
echo "lua-test: logs in $WORK/log"
exit $status
