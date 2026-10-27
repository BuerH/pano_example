#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// HTTP MJPEG: one shared latest frame, bounded clients, nonblocking socket I/O.
class MjpegServer {
public:
    explicit MjpegServer(unsigned short port);
    ~MjpegServer();
    MjpegServer(const MjpegServer&) = delete;
    MjpegServer& operator=(const MjpegServer&) = delete;
    void publish(const std::vector<unsigned char>& jpeg);
private:
    void serve();
    int listener_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::shared_ptr<const std::string> latest_;
};
