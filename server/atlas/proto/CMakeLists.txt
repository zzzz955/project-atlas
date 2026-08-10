# architecture-design.md §8.1 — the frame layer, in a directory of its own rather than inside
# atlas/net. session.h declares a byte boundary and cpp-style.md §8 lists the three net files as
# framework assets precisely because they know nothing about frames; folding framing into them would
# collapse that boundary.
add_library(atlas_proto STATIC crc32.cpp frame.cpp frame_reader.cpp session_framing.cpp)

# 🔴 Dependency direction: proto -> net, and never the reverse.
# Frames sit ABOVE the byte stream, so the upper layer is the one that names the lower. Linking the
# other way (atlas_net -> atlas_proto) would let session.h reach for a FrameReader and would make
# the byte boundary a comment instead of a compile-time fact. The cost of this direction is that
# session_framing.cpp — the one file here that touches Session — drags asio in at link time;
# crc32/frame/frame_reader stay asio-free and are usable with no I/O at all, which is what the unit
# tests exercise.
# PUBLIC: session_framing.h names Session in its signatures, so a consumer of this library links the
# net symbols too.
target_link_libraries(atlas_proto PUBLIC atlas_net)

# 🔴 No PCH here on purpose. atlas_net puts <boost/asio.hpp> behind a PRIVATE PCH because every one
# of its sources needs it; here only session_framing.cpp does, and a target-wide PCH would make the
# three parsing sources parse asio for nothing — the exact opposite of what §15.1 bought.
