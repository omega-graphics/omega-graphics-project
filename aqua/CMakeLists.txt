omega_graphics_project(AQUA VERSION 0.1 LANGUAGES CXX)

# AQUA physics library — glob per src component so adding a new file inside
# src/ doesn't require touching this file (matches the suite convention used
# by kreate/ and va/). AQUA is currently standalone: it has no link-time
# dependency on a graphics engine, so its public math types stay its own.
file(GLOB AQUA_SRCS_TOPLEVEL CONFIGURE_DEPENDS src/*.cpp)

add_omega_graphics_module(AQUA SHARED
    HEADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    SOURCES
        ${AQUA_SRCS_TOPLEVEL}
)
target_compile_definitions(AQUA PRIVATE AQUA__BUILD__)

# NOTE: No third-party physics SDK is currently vendored. Whether AQUA grows its
# own solver or integrates an external SDK is an open decision — if it integrates
# one, add the dependency here and the clone entry to AUTOMDEPS, and keep the
# backend types out of include/aqua/.
