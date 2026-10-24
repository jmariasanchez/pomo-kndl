// Pinned TLS client for one landing. OpenSSL; certificate pinned by SHA-256 of its DER.
#pragma once
#include "../third_party/json.hpp"

namespace pk {
// Sends one request line and returns the parsed reply. Throws std::runtime_error on any
// failure, including a pin mismatch. timeout_sec bounds connect, read and write.
nlohmann::json tls_exchange(const nlohmann::json& pairing, const nlohmann::json& request,
                            int timeout_sec = 15);
}
