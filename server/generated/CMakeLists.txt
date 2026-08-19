# Generator output root (architecture-design.md §14).
# Nothing under generated/ is ever hand-edited. Change the input (shared/datas/*.csv,
# shared/contracts/*.cs, server/db/schema.json) and re-run the generator — `npm run gen:all` from
# the repo root.
#
# info/, pkt/ and db/ each own their own CMakeLists.txt on purpose: the three generator work items
# then never write the same file, so they can run in parallel without conflicting.
#
# The order below mirrors `gen:all` (info -> db -> pkt): static data comes before the schema and the
# packets. CMake does not require it — the three targets are independent — but two orderings of the
# same three generators in one repository is exactly the kind of detail that drifts.
add_subdirectory(info)
add_subdirectory(pkt)
add_subdirectory(db)
