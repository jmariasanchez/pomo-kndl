#include "net.hpp"

#include <netdb.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace pk {

// The Kindle links against KOReader's LibreSSL, built with OpenSSL 1.0 headers. Use only
// symbols both it and OpenSSL 3 provide.
#if !defined(SSL_CTX_set_min_proto_version)
extern "C" int SSL_CTX_set_min_proto_version(SSL_CTX*, uint16_t);
#endif

static std::string ssl_err(const char* what) {
    char buf[256];
    unsigned long e = ERR_get_error();
    ERR_error_string_n(e, buf, sizeof buf);
    return std::string(what) + ": " + (e ? buf : "unknown");
}

static int connect_tcp(const std::string& host, int port, int timeout) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        throw std::runtime_error("cannot resolve " + host);
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{timeout, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) throw std::runtime_error("computer unreachable at " + host + ":" + std::to_string(port));
    return fd;
}

static std::string cert_sha256(X509* cert) {
    unsigned char* der = nullptr;
    int len = i2d_X509(cert, &der);
    if (len <= 0) throw std::runtime_error("bad server certificate");
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    EVP_Digest(der, (size_t)len, md, &mdlen, EVP_sha256(), nullptr);
    OPENSSL_free(der);
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (unsigned i = 0; i < mdlen; i++) {
        s += hx[md[i] >> 4];
        s += hx[md[i] & 15];
    }
    return s;
}

nlohmann::json tls_exchange(const nlohmann::json& p, const nlohmann::json& req, int timeout) {
    const std::string host = p.at("host"), pin = p.at("server_sha256");
    const int port = p.value("port", 7419);

    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(SSL_CTX_new(SSLv23_client_method()), SSL_CTX_free);
    if (!ctx) throw std::runtime_error(ssl_err("SSL_CTX_new"));
    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) != 1)
        throw std::runtime_error("TLS 1.2 not supported by this SSL library");
    // Trust comes from the pin, not a CA. Chain checks are replaced by the exact match below.
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);

    int fd = connect_tcp(host, port, timeout);
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.get()), SSL_free);
    struct Fd { int f; ~Fd() { close(f); } } guard{fd};
    SSL_set_fd(ssl.get(), fd);
    SSL_set_tlsext_host_name(ssl.get(), "pomo-kndl");
    if (SSL_connect(ssl.get()) != 1) throw std::runtime_error(ssl_err("TLS handshake failed"));

    // OpenSSL 3 maps this to SSL_get1_peer_certificate; both return a reference we free.
    X509* cert = SSL_get_peer_certificate(ssl.get());
    if (!cert) throw std::runtime_error("server sent no certificate");
    std::string got = cert_sha256(cert);
    X509_free(cert);
    if (got != pin) throw std::runtime_error("server identity mismatch: refusing to sync (re-pair?)");

    std::string out = req.dump() + "\n";
    size_t off = 0;
    while (off < out.size()) {
        int n = SSL_write(ssl.get(), out.data() + off, (int)(out.size() - off));
        if (n <= 0) throw std::runtime_error(ssl_err("send failed"));
        off += (size_t)n;
    }
    std::string in;
    char buf[8192];
    while (in.find('\n') == std::string::npos) {
        int n = SSL_read(ssl.get(), buf, sizeof buf);
        if (n <= 0) break;
        in.append(buf, (size_t)n);
        if (in.size() > (4u << 20)) throw std::runtime_error("reply too large");
    }
    SSL_shutdown(ssl.get());
    auto nl = in.find('\n');
    if (nl == std::string::npos) throw std::runtime_error("connection closed before reply");
    return nlohmann::json::parse(in.substr(0, nl));
}

}  // namespace pk
