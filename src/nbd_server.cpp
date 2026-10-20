// CloudBlock NBD fixed-newstyle adapter. Loopback ONLY; no TLS or remote exposure.
// NBD protocol: https://github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md
#ifndef __linux__
#error "cloudblock-nbd is Linux only"
#endif
#include "block_store.hpp"
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace {
using Buf = std::vector<std::uint8_t>;
constexpr u64 INIT_MAGIC = 0x4e42444d41474943ULL;
constexpr u64 OPT_MAGIC = 0x49484156454f5054ULL;
constexpr u64 OPT_REPLY_MAGIC = 0x0003e889045565a9ULL;
constexpr std::uint32_t REQUEST_MAGIC = 0x25609513;
constexpr std::uint32_t SIMPLE_REPLY_MAGIC = 0x67446698;
constexpr std::uint32_t MAX_OPTION = 64 * 1024;
constexpr std::uint32_t MAX_IO = 8 * 1024 * 1024;
constexpr std::uint16_t TX_FLAGS = 1 | 4 | 8; // HAS_FLAGS, SEND_FLUSH, SEND_FUA
constexpr std::uint32_t NBD_ERR_UNSUP = 0x80000001u;
constexpr std::uint32_t NBD_ERR_INVALID = 0x80000003u;
constexpr std::uint32_t NBD_ERR_UNKNOWN = 0x80000006u;
constexpr int NBD_IO_ERROR = 5;
constexpr int NBD_INVALID = 22;

std::uint16_t read16(const std::uint8_t* p) {
    return std::uint16_t(p[0] << 8 | p[1]);
}
std::uint32_t read32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
u64 read64(const std::uint8_t* p) {
    return (u64(read32(p)) << 32) | read32(p + 4);
}
void append16(Buf& b, std::uint16_t n) {
    b.push_back(std::uint8_t(n >> 8));
    b.push_back(std::uint8_t(n));
}
void append32(Buf& b, std::uint32_t n) {
    b.push_back(std::uint8_t(n >> 24));
    b.push_back(std::uint8_t(n >> 16));
    b.push_back(std::uint8_t(n >> 8));
    b.push_back(std::uint8_t(n));
}
void append64(Buf& b, u64 n) {
    append32(b, std::uint32_t(n >> 32));
    append32(b, std::uint32_t(n));
}
void write_all(int fd, const std::uint8_t* data, std::size_t len) {
    while (len > 0) {
        auto n = ::send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("NBD peer write failed");
        data += n; len -= static_cast<std::size_t>(n);
    }
}
void write_all(int fd, const Buf& b) { write_all(fd, b.data(), b.size()); }
bool read_all(int fd, std::uint8_t* data, std::size_t len) {
    bool got_any = false;
    while (len > 0) {
        auto n = ::recv(fd, data, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0 && !got_any) return false;
        if (n <= 0) throw std::runtime_error("NBD peer closed mid-packet");
        data += n; len -= static_cast<std::size_t>(n); got_any = true;
    }
    return true;
}
void option_reply(int fd, std::uint32_t opt, std::uint32_t type, const Buf& payload = {}) {
    Buf b;
    append64(b, OPT_REPLY_MAGIC);
    append32(b, opt);
    append32(b, type);
    append32(b, static_cast<std::uint32_t>(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    write_all(fd, b);
}
void simple_reply(int fd, std::uint32_t error, const std::uint8_t* cookie) {
    Buf b;
    append32(b, SIMPLE_REPLY_MAGIC);
    append32(b, error);
    b.insert(b.end(), cookie, cookie + 8);
    write_all(fd, b);
}
void handshake(int fd) {
    Buf hello;
    append64(hello, INIT_MAGIC);
    append64(hello, OPT_MAGIC);
    append16(hello, 3);  // FIXED_NEWSTYLE | NO_ZEROES
    write_all(fd, hello);
    std::array<std::uint8_t, 4> flags{};
    if (!read_all(fd, flags.data(), flags.size()))
        throw std::runtime_error("NBD peer left before client flags");
    auto client = read32(flags.data());
    check((client & ~3u) == 0, "unknown NBD client flags");
    check((client & 1u) != 0, "fixed-newstyle client required");
    while (true) {
        std::array<std::uint8_t, 16> h{};
        if (!read_all(fd, h.data(), h.size()))
            throw std::runtime_error("NBD peer left during option negotiation");
        check(read64(h.data()) == OPT_MAGIC, "invalid NBD option magic");
        auto opt = read32(h.data() + 8);
        auto len = read32(h.data() + 12);
        check(len <= MAX_OPTION, "NBD option too large");
        Buf data(len);
        if (len && !read_all(fd, data.data(), len))
            throw std::runtime_error("NBD option payload missing");

        if (opt == 1) { // NBD_OPT_EXPORT_NAME (legacy)
            const std::string name(data.begin(), data.end());
            check(name.empty() || name == "cloudblock", "unknown NBD export");
            Buf reply;
            append64(reply, VOLUME);
            append16(reply, TX_FLAGS);
            if (!(client & 2u)) reply.insert(reply.end(), 124, 0);
            write_all(fd, reply);
            return;
        }
        if (opt == 2) { // ABORT
            option_reply(fd, opt, 1);
            throw std::runtime_error("NBD client aborted negotiation");
        }
        if (opt == 3) { // LIST
            if (len != 0) { option_reply(fd, opt, NBD_ERR_INVALID); continue; }
            Buf payload;
            constexpr char name[] = "cloudblock";
            append32(payload, sizeof(name) - 1);
            payload.insert(payload.end(), name, name + sizeof(name) - 1);
            option_reply(fd, opt, 2, payload); // NBD_REP_SERVER
            option_reply(fd, opt, 1); // ACK
            continue;
        }
        if (opt == 6 || opt == 7) { // INFO / GO
            if (len < 6) { option_reply(fd, opt, NBD_ERR_INVALID); continue; }
            auto nlen = read32(data.data());
            if (nlen > len - 6) { option_reply(fd, opt, NBD_ERR_INVALID); continue; }
            auto off = std::size_t(4) + nlen;
            auto count = read16(data.data() + off);
            if (off + 2 + std::size_t(count) * 2 != data.size()) {
                option_reply(fd, opt, NBD_ERR_INVALID); continue;
            }
            const std::string name(data.begin() + 4, data.begin() + off);
            if (!(name.empty() || name == "cloudblock")) {
                option_reply(fd, opt, NBD_ERR_UNKNOWN); continue;
            }
            Buf export_info;
            append16(export_info, 0); // NBD_INFO_EXPORT
            append64(export_info, VOLUME);
            append16(export_info, TX_FLAGS);
            option_reply(fd, opt, 3, export_info); // NBD_REP_INFO
            for (std::size_t i = 0; i < count; ++i) {
                std::uint16_t request = read16(data.data() + off + 2 + i * 2);
                if (request == 3) { // NBD_INFO_BLOCK_SIZE
                    Buf block;
                    append16(block, 3);
                    append32(block, 1);     // min
                    append32(block, 4096);  // preferred
                    append32(block, MAX_IO);
                    option_reply(fd, opt, 3, block);
                } else if (request == 1) { // NBD_INFO_NAME
                    Buf b;
                    append16(b, 1);
                    constexpr char name_value[] = "cloudblock";
                    b.insert(b.end(), name_value, name_value + sizeof(name_value) - 1);
                    option_reply(fd, opt, 3, b);
                }
            }
            option_reply(fd, opt, 1); // ACK
            if (opt == 7) return;
            continue;
        }
        option_reply(fd, opt, NBD_ERR_UNSUP);
    }
}
void transmit(int fd, Store& store) {
    while (true) {
        std::array<std::uint8_t, 28> h{};
        if (!read_all(fd, h.data(), h.size())) return;
        check(read32(h.data()) == REQUEST_MAGIC, "bad NBD request magic");
        const auto flags = read16(h.data() + 4);
        const auto type = read16(h.data() + 6);
        const auto off = read64(h.data() + 16);
        const auto len = read32(h.data() + 24);
        if (type == 2) return; // NBD_CMD_DISC, no reply
        check(len <= MAX_IO, "oversized request; close rather than allocate");
        // Read write payload even if it is outside export, to keep stream framing.
        Buf payload;
        if (type == 1) {
            payload.resize(len);
            if (len && !read_all(fd, payload.data(), len))
                throw std::runtime_error("short NBD write data");
        }
        const bool bounds = off <= VOLUME && len <= VOLUME - off;
        const bool commands_ok =
            (type == 0 && flags == 0) ||
            (type == 1 && (flags & ~1u) == 0) ||
            (type == 3 && flags == 0 && len == 0 && off == 0);
        if (!commands_ok || !bounds) {
            simple_reply(fd, NBD_INVALID, h.data() + 8);
            continue;
        }
        try {
            if (type == 0) { // READ
                payload.resize(len);
                if (len) store.read(off, reinterpret_cast<char*>(payload.data()), len);
                simple_reply(fd, 0, h.data() + 8);
                write_all(fd, payload);
            } else if (type == 1) { // WRITE
                if (len) store.write(off, reinterpret_cast<const char*>(payload.data()), len);
                if (flags & 1u) store.flush(); // FUA: force backing commit
                simple_reply(fd, 0, h.data() + 8);
            } else { // FLUSH
                store.flush();
                simple_reply(fd, 0, h.data() + 8);
            }
        } catch (const std::exception& e) {
            std::cerr << "NBD I/O failed: " << e.what() << '\n';
            simple_reply(fd, NBD_IO_ERROR, h.data() + 8);
        }
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: cloudblock-nbd ROOT [--port 10809] [--cache-mib 1024]\n";
            return 2;
        }
        fs::path root = argv[1];
        u64 capacity = CAPACITY;
        std::uint16_t port = 10809;
        for (int i = 2; i < argc; i += 2) {
            check(i + 1 < argc, "missing option value");
            std::string key = argv[i];
            u64 n = parse(argv[i + 1]);
            if (key == "--port") {
                check(n >= 1024 && n <= 65535, "port must be 1024..65535");
                port = static_cast<std::uint16_t>(n);
            } else if (key == "--cache-mib") {
                check(n <= UINT64_MAX / MIB, "overflow");
                capacity = n * MIB;
            } else throw std::runtime_error("unrecognized argument: " + key);
        }
        std::signal(SIGPIPE, SIG_IGN);
        Store store(root, capacity);
        int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        check(listen_fd >= 0, "cannot create TCP socket");
        int one = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("cannot bind NBD loopback socket: " +
                                     std::string(std::strerror(errno)));
        check(::listen(listen_fd, 4) == 0, "cannot listen on NBD TCP port");
        std::cout << "NBD listening on 127.0.0.1:" << port
                  << " size=" << VOLUME << " cache_limit=" << capacity
                  << " root=" << root << std::endl;
        while (true) {
            int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0 && errno == EINTR) continue;
            check(fd >= 0, "NBD accept failed");
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            try {
                handshake(fd);
                transmit(fd, store);
            } catch (const std::exception& e) {
                std::cerr << "NBD session ended: " << e.what() << '\n';
            }
            ::close(fd);
        }
    } catch (const std::exception& e) {
        std::cerr << "NBD fatal: " << e.what() << '\n';
        return 1;
    }
}
