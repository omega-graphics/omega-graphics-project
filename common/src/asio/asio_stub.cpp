// OmegaCommonASIO — reservation stub (Binary-Split Plan Phase 3).
//
// The OmegaCommon-Async-WebSocket plan replaces this translation unit with
// src/asio/io.cpp and src/asio/websocket.cpp. Until then this empty TU gives
// the linker something to build the OmegaCommonASIO shared library from, so
// the Async work has a binary to land in without a flag-day CMake change.

namespace OmegaCommon::ASIO {
    // Placeholder declaration so this TU is non-empty until the Async plan
    // lands src/asio/io.cpp + websocket.cpp here. Emits no runtime symbol
    // and carries no public API surface.
    typedef void AsioReservationTag;
}
