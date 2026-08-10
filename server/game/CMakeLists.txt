# The demo game (architecture-design.md §14 — the "game (replaceable)" layer).
#
# 🔴 THIS DIRECTORY EXISTS BECAUSE THE CORE-PURITY GATE (§15.4) FORBIDS IT FROM BEING IN atlas/.
# `item`, `inventory` and `equip` are on tools/core_purity/denylist.txt, so a game with an inventory
# cannot be written into server/atlas/** — the build fails. Swapping the game means replacing this
# directory plus the generator inputs; the framework targets below are linked, never edited.
#
# 🔴 The dependency edge points game -> core, and there is no edge back. No atlas_* target links
# atlas_demo, and none ever should: the moment one does, the framework knows about this game and
# §14's replacement claim stops being true.

# The domain + handlers, as a library rather than as sources on the executable. The end-to-end test
# links this and stands up a real GameServer, which is only possible because main() is not in it.
add_library(atlas_demo STATIC
    character.cpp
    equip_service.cpp
    handlers.cpp
    inventory.cpp)

# PUBLIC throughout: handlers.h names Session / Endpoint / DbRunner and the generated row structs in
# its own signatures, so anything linking this library needs those include paths and the same
# standard and definitions (cpp-style.md §7.2 — atlas_core propagates them).
#   atlas_proto  frames (and, transitively, atlas_net)
#   atlas_db     the ORM runtime
#   atlas_config server.ini / .env
#   atlas_generated_{db,pkt}  the generated row structs and the LE codec
target_link_libraries(atlas_demo
    PUBLIC atlas_proto atlas_db atlas_config atlas_generated_db atlas_generated_pkt)

# The role=game binary. scripts/entrypoint.sh maps ATLAS_ROLE=game to /app/atlas_game, so the target
# name is part of the deploy contract (§15.3) — renaming it silently breaks the image.
add_executable(atlas_game main.cpp)
target_link_libraries(atlas_game PRIVATE atlas_demo)
