# Generator output root (architecture-design.md §14).
# 🔴 Nothing under generated/ is ever hand-edited. Change the input (shared/contracts/*.cs,
# server/db/schema.json) and re-run the generator — `npm run gen:all` from the repo root.
#
# pkt/ and db/ each own their own CMakeLists.txt on purpose: the two generator work items then
# never write the same file, so they can run in parallel without conflicting.
add_subdirectory(pkt)
add_subdirectory(db)
