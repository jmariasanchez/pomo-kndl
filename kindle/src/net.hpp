// Pinned TLS client for one landing. OpenSSL; certificate pinned by SHA-256 of its DER.
#pragma once
#include "../third_party/json.hpp"

namespace pk {
// Sends one request line and returns the parsed reply. Throws std::runtime_error on any
// failure, including a pin mismatch. timeout_sec must be positive, and port 1..65535.
// After blocking DNS resolution, TCP connect shares one steady-clock timeout_sec
// deadline across all candidate addresses. TLS uses blocking sockets with per-I/O
// read/write timeouts, not a total handshake or exchange deadline. DNS is unbounded,
// and repeated TLS I/O (including shutdown) can exceed timeout_sec in aggregate.
nlohmann::json tls_exchange(const nlohmann::json& pairing, const nlohmann::json& request,
                            int timeout_sec = 15);
}
