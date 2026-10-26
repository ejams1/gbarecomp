#include "gba_bus.h"
#include "tcp_debug_server.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
using socket_length_t = int;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(socket_t socket) { closesocket(socket); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
using socket_length_t = socklen_t;
static constexpr socket_t kInvalidSocket = -1;
static void close_socket(socket_t socket) { close(socket); }
#endif

namespace {
constexpr std::uint32_t kMailboxAddress = 0x02000100u;
constexpr std::size_t kMailboxOffset = 0x100u;
constexpr std::size_t kMailboxBytes = 40;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint16_t read16(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::uint32_t read32(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void write16(std::uint8_t* bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void write32(std::uint8_t* bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::string hex(const std::uint8_t* bytes, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
}

std::uint16_t available_port() {
    socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(socket != kInvalidSocket, "could not create port-probe socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(socket, reinterpret_cast<sockaddr*>(&address),
                   static_cast<socket_length_t>(sizeof(address))) == 0,
            "could not bind port-probe socket");
    socket_length_t address_size = static_cast<socket_length_t>(sizeof(address));
    require(::getsockname(socket, reinterpret_cast<sockaddr*>(&address),
                          &address_size) == 0,
            "could not query ephemeral port");
    const auto port = ntohs(address.sin_port);
    close_socket(socket);
    return port;
}

socket_t connect_local(std::uint16_t port,
                       const char* host = "127.0.0.1") {
    for (int attempt = 0; attempt < 200; ++attempt) {
        socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket != kInvalidSocket) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = inet_addr(host);
            address.sin_port = htons(port);
            if (::connect(socket, reinterpret_cast<sockaddr*>(&address),
                          static_cast<socket_length_t>(sizeof(address))) == 0) {
#if defined(_WIN32)
                DWORD timeout_ms = 3000;
                setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&timeout_ms),
                           sizeof(timeout_ms));
#else
                timeval timeout{3, 0};
                setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout));
#endif
                return socket;
            }
            close_socket(socket);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("TCP debug server did not accept a loopback client");
}

bool connects_to_other_loopback_alias(std::uint16_t port) {
    socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.2");
    address.sin_port = htons(port);
    const bool connected = ::connect(
        socket, reinterpret_cast<sockaddr*>(&address),
        static_cast<socket_length_t>(sizeof(address))) == 0;
    close_socket(socket);
    return connected;
}

std::string request(socket_t socket, const std::string& json) {
    const std::string line = json + "\n";
    std::size_t sent = 0;
    while (sent < line.size()) {
        const int count = ::send(socket, line.data() + sent,
                                 static_cast<int>(line.size() - sent), 0);
        require(count > 0, "TCP debug request send failed");
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[1024];
    while (response.find('\n') == std::string::npos) {
        const int count = ::recv(socket, buffer, sizeof(buffer), 0);
        require(count > 0, "TCP debug response receive failed");
        response.append(buffer, static_cast<std::size_t>(count));
    }
    response.resize(response.find('\n'));
    return response;
}

void expect_ok(const std::string& response) {
    require(response.find("\"ok\":true") != std::string::npos,
            "expected successful TCP response");
}
}  // namespace

int main() {
#if defined(_WIN32)
    WSADATA winsock{};
    require(WSAStartup(MAKEWORD(2, 2), &winsock) == 0,
            "test WSAStartup failed");
#endif
    try {
        gba::GbaBus bus;
        std::uint64_t frames = 0;
        std::atomic<std::uint32_t> acknowledgements{0};
        std::atomic<bool> mailbox_valid{true};
        std::atomic<bool> keyinput_valid{true};

        gbarecomp::debug::TcpDebugServer server;
        gbarecomp::debug::TcpDebugServer::Context context{};
        context.bus = &bus;
        context.sync_frames = &frames;
        context.step = [&]() {
            ++frames;
            const std::uint16_t expected_keyinput =
                frames == 1 ? 1021 : 1018;
            if (bus.io().host_keyinput() != expected_keyinput)
                keyinput_valid.store(false);
            auto* mailbox = bus.ewram_ptr() + kMailboxOffset;
            const std::uint32_t sequence = read32(mailbox, 0);
            if (sequence != 0 && sequence != read32(mailbox, 4)) {
                const auto version = read16(mailbox, 8);
                const auto command = read16(mailbox, 10);
                const auto variable = read16(mailbox, 16);
                const auto value = read16(mailbox, 18);
                if (version != 1 || command != 2)
                    mailbox_valid.store(false);
                // Mimic the M1-5 frame-boundary service: publish status and
                // results first, then acknowledge the request sequence.
                write16(mailbox, 12, 1);
                write16(mailbox, 32, variable);
                write16(mailbox, 34, value);
                write32(mailbox, 4, sequence);
                acknowledgements.fetch_add(1);
            }
            return true;
        };

        const auto port = available_port();
        std::thread server_thread([&] { server.run(port, context); });
        socket_t client = kInvalidSocket;
        try {
            client = connect_local(port);
            require(!connects_to_other_loopback_alias(port),
                    "default listener accepted a non-127.0.0.1 loopback address");
            require(request(client, "{\"cmd\":\"ping\"}").find(
                        "gbarecomp_native") != std::string::npos,
                    "ping did not identify the native host");

            expect_ok(request(client,
                "{\"cmd\":\"set_keyinput\",\"value\":1021}"));
            expect_ok(request(client,
                "{\"cmd\":\"run_frames\",\"n\":1}"));
            require(keyinput_valid.load(),
                    "set_keyinput did not preserve active-low GBA bits before stepping");

            // A single bounded frame request applies its requested key state
            // before invoking the frame callback.
            const auto run = request(client,
                "{\"cmd\":\"run_frames\",\"n\":1,\"keyinput\":1018}");
            expect_ok(run);
            require(run.find("\"frames\":1") != std::string::npos &&
                    run.find("\"frame\":2") != std::string::npos,
                    "run_frames did not report its frame-boundary result");
            require(keyinput_valid.load(),
                    "run_frames did not apply keyinput before stepping");

            const auto oversize = request(client,
                "{\"cmd\":\"run_frames\",\"n\":1000001}");
            require(oversize.find("\"ok\":false") != std::string::npos &&
                    frames == 2,
                    "run_frames accepted an over-limit request");

            std::array<std::uint8_t, kMailboxBytes> mailbox{};
            write16(mailbox.data(), 8, 1);       // M1-5 protocol version
            write16(mailbox.data(), 10, 2);      // generic SET_VAR command
            write16(mailbox.data(), 16, 0x1234); // variable id
            write16(mailbox.data(), 18, 0xBEEF); // requested value
            expect_ok(request(client,
                "{\"cmd\":\"write_mem\",\"addr\":" +
                std::to_string(kMailboxAddress + 4) + ",\"hex\":\"" +
                hex(mailbox.data() + 4, mailbox.size() - 4) + "\"}"));
            // Sequence is published last, so a running guest never sees a
            // partially populated mailbox as a new request.
            expect_ok(request(client,
                "{\"cmd\":\"write_mem\",\"addr\":" +
                std::to_string(kMailboxAddress) +
                ",\"hex\":\"01000000\"}"));
            expect_ok(request(client,
                "{\"cmd\":\"run_frames\",\"n\":1}"));
            require(mailbox_valid.load() && acknowledgements.load() == 1,
                    "mock M1-5 service did not acknowledge at a frame boundary");

            const auto ack = request(client,
                "{\"cmd\":\"read_ewram\",\"addr\":" +
                std::to_string(kMailboxAddress + 4) + ",\"len\":4}");
            require(ack.find("\"data\":\"01000000\"") != std::string::npos,
                    "read_ewram did not return the acknowledged sequence");
            const auto result = request(client,
                "{\"cmd\":\"read_ewram\",\"addr\":" +
                std::to_string(kMailboxAddress + 32) + ",\"len\":4}");
            require(result.find("\"data\":\"3412efbe\"") != std::string::npos,
                    "read_ewram did not return mailbox result fields");

            const auto out_of_range = request(client,
                "{\"cmd\":\"read_ewram\",\"addr\":33554431,\"len\":4}");
            require(out_of_range.find("\"ok\":false") != std::string::npos,
                    "read_ewram accepted an out-of-range read");
            const auto bad_write = request(client,
                "{\"cmd\":\"write_mem\",\"addr\":33554431,\"hex\":\"01020304\"}");
            require(bad_write.find("\"ok\":false") != std::string::npos,
                    "write_mem accepted an out-of-range write");

            (void)request(client, "{\"cmd\":\"quit\"}");
            close_socket(client);
            client = kInvalidSocket;
            server_thread.join();
            require(frames == 3, "unexpected guest frame count");

            gbarecomp::debug::TcpDebugServer container_server;
            const auto container_port = available_port();
            std::thread container_thread([&] {
                container_server.run(
                    container_port, context,
                    gbarecomp::debug::TcpDebugServer::BindAddress::AnyInterface);
            });
            socket_t container_client = kInvalidSocket;
            try {
                container_client = connect_local(container_port, "127.0.0.2");
                require(request(container_client, "{\"cmd\":\"ping\"}")
                            .find("gbarecomp_native") != std::string::npos,
                        "explicit any-interface listener was not reachable");
                (void)request(container_client, "{\"cmd\":\"quit\"}");
                close_socket(container_client);
                container_client = kInvalidSocket;
                container_thread.join();
            } catch (...) {
                if (container_client != kInvalidSocket)
                    close_socket(container_client);
                container_server.stop();
                if (container_thread.joinable()) container_thread.join();
                throw;
            }
        } catch (...) {
            if (client != kInvalidSocket) close_socket(client);
            server.stop();
            if (server_thread.joinable()) server_thread.join();
            throw;
        }
#if defined(_WIN32)
        WSACleanup();
#endif
        std::cout << "TCP agent contract checks passed (frame/input/memory/mailbox).\n";
        return 0;
    } catch (const std::exception& error) {
#if defined(_WIN32)
        WSACleanup();
#endif
        std::cerr << error.what() << '\n';
        return 1;
    }
}
