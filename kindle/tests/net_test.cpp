// Standalone host regression test (do not also link net.cpp):
// c++ -std=c++17 -pthread -I$(brew --prefix openssl@3)/include \
//   kindle/tests/net_test.cpp -L$(brew --prefix openssl@3)/lib -lssl -lcrypto -o "$JCODE_SCRATCH_DIR/net_test"
// Linux: omit the Homebrew include/library flags. Uses ephemeral loopback sockets only.
#include <sys/socket.h>
#include <poll.h>
#include <cerrno>
#include <chrono>
#include <thread>

// A controlled pending connection tests the deadline without a public blackhole host.
static bool stalled_connect = false;
static int interrupted_polls = 0;
static int test_connect(int fd, const sockaddr* addr, socklen_t len) {
    if (!stalled_connect) return ::connect(fd, addr, len);
    errno = EINPROGRESS;
    return -1;
}
static int test_poll(pollfd* fds, nfds_t count, int timeout) {
    if (!stalled_connect) return ::poll(fds, count, timeout);
    const int delay = timeout > 100 ? 100 : timeout;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    ++interrupted_polls;
    errno = EINTR;
    return -1;
}
#define connect test_connect
#define poll test_poll
#include "../src/net.cpp"
#undef poll
#undef connect
#include <arpa/inet.h>
#include <openssl/rsa.h>
#include <csignal>
#include <iostream>

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

template<class F> static void fails(F fn, const char* expected) {
    try { fn(); }
    catch (const std::runtime_error& e) {
        check(std::string(e.what()).find(expected) != std::string::npos, e.what());
        return;
    }
    throw std::runtime_error("expected failure");
}

struct Listener {
    int fd = -1;
    int port = 0;
    explicit Listener(bool listening = true) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        check(fd >= 0, "socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0, "bind");
        socklen_t len = sizeof addr;
        check(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname");
        port = ntohs(addr.sin_port);
        if (listening) check(listen(fd, 4) == 0, "listen");
    }
    ~Listener() { close(fd); }
};

static nlohmann::json pairing(int port, std::string pin = "not-the-pin") {
    return {{"host", "127.0.0.1"}, {"port", port}, {"server_sha256", pin}};
}

struct Identity {
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx{SSL_CTX_new(TLS_server_method()), SSL_CTX_free};
    std::string pin;
    Identity() {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> gen(
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        check(gen && EVP_PKEY_keygen_init(gen.get()) == 1, "keygen init");
        check(EVP_PKEY_CTX_set_rsa_keygen_bits(gen.get(), 2048) == 1, "key bits");
        EVP_PKEY* raw = nullptr;
        check(EVP_PKEY_keygen(gen.get(), &raw) == 1, "keygen");
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        check(cert && ctx, "TLS allocation");
        X509_set_version(cert.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
        X509_gmtime_adj(X509_get_notBefore(cert.get()), -60);
        X509_gmtime_adj(X509_get_notAfter(cert.get()), 3600);
        X509_set_pubkey(cert.get(), key.get());
        auto name = X509_get_subject_name(cert.get());
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("loopback-test"), -1, -1, 0);
        X509_set_issuer_name(cert.get(), name);
        check(X509_sign(cert.get(), key.get(), EVP_sha256()) > 0, "sign");
        check(SSL_CTX_use_certificate(ctx.get(), cert.get()) == 1 &&
              SSL_CTX_use_PrivateKey(ctx.get(), key.get()) == 1, "server identity");
        pin = pk::cert_sha256(cert.get());
    }
};

static void exchange_test(Identity& identity, bool wrong_pin) {
    Listener listener;
    std::exception_ptr server_error;
    std::thread server([&] {
        try {
            pollfd pending{listener.fd, POLLIN, 0};
            check(poll(&pending, 1, 5000) == 1, "accept deadline");
            int fd = accept(listener.fd, nullptr, nullptr);
            check(fd >= 0, "accept");
            struct Guard { int fd; ~Guard() { close(fd); } } guard{fd};
            timeval tv{3, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(identity.ctx.get()), SSL_free);
            SSL_set_fd(ssl.get(), fd);
            check(SSL_accept(ssl.get()) == 1, "server handshake");
            char buf[1024];
            std::string request;
            int n;
            while ((n = SSL_read(ssl.get(), buf, sizeof buf)) > 0) {
                request.append(buf, n);
                if (request.find('\n') != std::string::npos) break;
            }
            if (wrong_pin) check(request.empty(), "request sent before pin verification");
            else {
                check(nlohmann::json::parse(request) == nlohmann::json{{"ping", true}}, "request mismatch");
                const std::string reply = "{\"ok\":true}\n";
                check(SSL_write(ssl.get(), reply.data(), reply.size()) == int(reply.size()), "server write");
                SSL_shutdown(ssl.get());
            }
        } catch (...) { server_error = std::current_exception(); }
    });
    std::exception_ptr client_error;
    try {
        auto p = pairing(listener.port, wrong_pin ? "incorrect" : identity.pin);
        if (wrong_pin) fails([&] { pk::tls_exchange(p, {{"ping", true}}, 2); }, "identity mismatch");
        else check(pk::tls_exchange(p, {{"ping", true}}, 2) == nlohmann::json{{"ok", true}}, "reply mismatch");
    } catch (...) { client_error = std::current_exception(); }
    server.join();
    if (client_error) std::rethrow_exception(client_error);
    if (server_error) std::rethrow_exception(server_error);
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    try {
        for (int timeout : {0, -1})
            fails([&] { pk::tls_exchange(nlohmann::json::object(), {}, timeout); }, "timeout must be positive");
        for (int port : {-1, 0, 65536})
            fails([&] { pk::tls_exchange(pairing(port), {}, 1); }, "port must be between");
        std::cout << "PASS invalid timeout and port\n";
        {
            Listener reserved(false);
            auto start = std::chrono::steady_clock::now();
            fails([&] { pk::tls_exchange(pairing(reserved.port), {}, 1); }, "computer unreachable");
            check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "refusal too slow");
        }
        std::cout << "PASS localhost refusal\n";
        {
            Listener listener;
            stalled_connect = true;
            const auto start = std::chrono::steady_clock::now();
            fails([&] { pk::connect_tcp("localhost", listener.port, 1); }, "computer unreachable");
            const auto elapsed = std::chrono::steady_clock::now() - start;
            stalled_connect = false;
            check(interrupted_polls >= 2, "poll interruption not exercised");
            check(elapsed >= std::chrono::milliseconds(900) && elapsed < std::chrono::milliseconds(1800),
                  "interrupted connect did not respect shared deadline");
        }
        std::cout << "PASS pending connect deadline despite repeated EINTR (simulated)\n";
        {
            Listener listener;
            int fd = pk::connect_tcp("localhost", listener.port, 1);
            check((fcntl(fd, F_GETFL, 0) & O_NONBLOCK) == 0, "blocking flags not restored");
            timeval tv{};
            socklen_t len = sizeof tv;
            check(getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, &len) == 0 && tv.tv_sec == 1, "read timeout missing");
            len = sizeof tv;
            check(getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, &len) == 0 && tv.tv_sec == 1, "write timeout missing");
            close(fd);
        }
        std::cout << "PASS localhost connect and blocking flags\n";
        {
            // A listening socket completes TCP but never responds to the TLS ClientHello.
            Listener listener;
            auto start = std::chrono::steady_clock::now();
            fails([&] { pk::tls_exchange(pairing(listener.port), {}, 1); }, "TLS handshake failed");
            const auto elapsed = std::chrono::steady_clock::now() - start;
            check(elapsed >= std::chrono::milliseconds(700) && elapsed < std::chrono::seconds(3), "TLS timeout duration");
        }
        std::cout << "PASS stalled TLS timeout\n";
        Identity identity;
        exchange_test(identity, false);
        std::cout << "PASS pinned TLS JSON exchange\n";
        exchange_test(identity, true);
        std::cout << "PASS wrong pin rejects before request\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
