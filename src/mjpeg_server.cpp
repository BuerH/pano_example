#include "mjpeg_server.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <system_error>

namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t MAX_CLIENTS = 8;
const auto response = std::make_shared<const std::string>(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=pano-frame\r\n"
    "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
struct Client {
    int fd;
    std::string request;
    bool ready = false;
    bool close_after_send = false;
    std::shared_ptr<const std::string> pending, last;
    size_t offset = 0;
    Clock::time_point progress = Clock::now();
};
}

MjpegServer::MjpegServer(unsigned short port) {
    listener_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener_ < 0) throw std::system_error(errno, std::generic_category(), "MJPEG socket");
    int yes = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listener_, 8) < 0) {
        int error = errno;
        close(listener_);
        listener_ = -1;
        throw std::system_error(error, std::generic_category(), "MJPEG listen");
    }
    try { worker_ = std::thread(&MjpegServer::serve, this); }
    catch (...) { close(listener_); throw; }
}

MjpegServer::~MjpegServer() {
    stopping_ = true;
    if (worker_.joinable()) worker_.join();
    if (listener_ >= 0) close(listener_);
}

void MjpegServer::publish(const std::vector<unsigned char>& jpeg) {
    if (jpeg.empty()) return;
    auto part = std::make_shared<std::string>(
        "--pano-frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
        std::to_string(jpeg.size()) + "\r\n\r\n");
    part->append(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
    part->append("\r\n");
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = std::move(part);
}

void MjpegServer::serve() {
    std::vector<Client> clients;
    while (!stopping_) {
        std::shared_ptr<const std::string> latest;
        { std::lock_guard<std::mutex> lock(mutex_); latest = latest_; }
        std::vector<pollfd> sockets{{listener_, POLLIN, 0}};
        for (auto& client : clients) {
            if (client.ready && !client.pending && latest && client.last != latest) {
                client.pending = latest;
                client.last = latest;
                client.offset = 0;
                client.progress = Clock::now();
            }
            sockets.push_back({client.fd, static_cast<short>(
                client.pending ? POLLOUT : POLLIN), 0});
        }
        int result = poll(sockets.data(), sockets.size(), 50);
        if (result < 0) { if (errno == EINTR) continue; break; }
        for (size_t i = 0; i < clients.size(); ++i) {
            auto& client = clients[i];
            short events = sockets[i + 1].revents;
            bool dead = events & (POLLERR | POLLHUP | POLLNVAL);
            if (!dead && (events & POLLIN)) {
                char buffer[2048];
                ssize_t n = recv(client.fd, buffer, sizeof(buffer), 0);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) dead = true;
                if (n > 0 && !client.ready) {
                    client.request.append(buffer, static_cast<size_t>(n));
                    if (client.request.size() > 4096) dead = true;
                    else if (client.request.find("\r\n\r\n") != std::string::npos) {
                        bool valid = client.request.rfind("GET /stream HTTP/1.", 0) == 0 ||
                                     client.request.rfind("GET / HTTP/1.", 0) == 0;
                        client.pending = valid ? response : std::make_shared<const std::string>(
                            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        client.close_after_send = !valid;
                        client.ready = true;
                        client.request.clear();
                        client.progress = Clock::now();
                    }
                }
            }
            if (!dead && (events & POLLOUT) && client.pending) {
                ssize_t n = send(client.fd, client.pending->data() + client.offset,
                                 client.pending->size() - client.offset, MSG_NOSIGNAL);
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) dead = true;
                if (n > 0) {
                    client.offset += static_cast<size_t>(n);
                    client.progress = Clock::now();
                    if (client.offset == client.pending->size()) {
                        client.pending.reset();
                        client.offset = 0;
                        dead = client.close_after_send;
                    }
                }
            }
            if ((!client.ready || client.pending) && Clock::now() - client.progress > std::chrono::seconds(10)) dead = true;
            if (dead) { close(client.fd); client.fd = -1; }
        }
        clients.erase(std::remove_if(clients.begin(), clients.end(),
                      [](const Client& c) { return c.fd < 0; }), clients.end());
        if (sockets[0].revents & POLLIN) {
            // Accept at most a bounded batch so connection floods cannot starve existing clients.
            for (size_t i = 0; i < MAX_CLIENTS; ++i) {
                int fd = accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) break;
                if (clients.size() >= MAX_CLIENTS) close(fd);
                else { Client client{}; client.fd = fd; clients.push_back(std::move(client)); }
            }
        }
    }
    for (const auto& client : clients) close(client.fd);
}
