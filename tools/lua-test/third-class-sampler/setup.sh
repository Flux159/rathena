# Sourced by run.sh. The third-class-sampler example (a new effect for about
# twenty skills of the third classes) and zz-watch, a test mod that logs what
# the player and the monsters around carry, every blow and every craft.
MOD=$APP/examples/mods/third-class-sampler
[ -d "$MOD/lua" ] || die "no example mod at $MOD: set APP to a ragnarokoffline.app checkout"
[ "$ERA" = re ] || die "third-class-sampler is renewal only: pre-renewal's skill tree has no third classes"

# The mod's server side, as the app installs it.
cp "$MOD"/db/*.yml "$WORK/db/import/"
append_body "$SUITE_DIR/test_skills.yml" "$WORK/db/import/skill_db.yml"
# The custom-skills suite's test monsters, and a hitter and a brute of its own.
append_body "$KIT/custom-skills/mob_db.yml" "$WORK/db/import/mob_db.yml"
append_body "$SUITE_DIR/mob_db.yml" "$WORK/db/import/mob_db.yml"
install_mod third-class-sampler "$MOD/lua"
install_mod zz-watch "$SUITE_DIR/zz-watch"
add_npc "$SUITE_DIR/scenario.txt"
