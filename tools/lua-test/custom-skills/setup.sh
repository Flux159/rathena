# Sourced by run.sh. The custom-skills example (a Guillotine Cross rework)
# and three test mods loaded after it:
#   zz-observe  hooks at the end of the example's chains log what they return
#   zz-first    hooks at the start of the chains log what the example is given
#   zz-probe    probe skills exercise the queries, actions and hooks the
#               example has no use for
# Pre-renewal has no Guillotine Cross: there the tree goes on the Assassin
# Cross (prere_tree.py, reset-prere.sql).
MOD=$APP/examples/mods/custom-skills
[ -d "$MOD/lua" ] || die "no example mod at $MOD: set APP to a ragnarokoffline.app checkout"

# The mod's server side, as the app installs it: db/ into db/import, lua/
# under db/import/lua/<mod>, npc/ loaded by the map server.
cp "$MOD"/db/*.yml "$WORK/db/import/"
[ "$ERA" = re ] || python3 "$SUITE_DIR/prere_tree.py" "$MOD/db/skill_tree.yml" "$WORK/db/import/skill_tree.yml"
append_body "$SUITE_DIR/test_skills.yml" "$WORK/db/import/skill_db.yml"
cp "$SUITE_DIR/mob_db.yml" "$WORK/db/import/mob_db.yml"
cp "$SUITE_DIR/item_randomopt_db.yml" "$WORK/db/import/item_randomopt_db.yml"
install_mod custom-skills "$MOD/lua"
install_mod zz-observe "$SUITE_DIR/zz-observe"
install_mod zz-first "$SUITE_DIR/zz-first"
install_mod zz-probe "$SUITE_DIR/zz-probe"
add_npc "$MOD/npc/refund.txt"
add_npc "$SUITE_DIR/scenario.txt"

# The client learns two of the tree's skills the way a player does:
# Contact Poison (3100) and Phantom Menace (2034).
CLIENT_ARGS=(--skillup 3100 --skillup 2034)
