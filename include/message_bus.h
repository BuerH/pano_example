#pragma once
#include <mutex>
#include <vector>
#include "common_def.h"

// 线程安全的「最新帧覆盖」总线：
// 每个相机一个槽位，生产者只保留最新一帧（不排队、不积压延迟），
// 消费者随时取到最新帧。相比原来的 FIFO 队列，延迟更低且无数据竞争。
class MessageBus {
private:
    struct Slot {
        std::mutex mtx;
        CameraFrame frame;
        bool has_frame = false; // 是否收到过帧
        bool is_fresh = false;  // 自上次 pop 后是否有新帧
    };
    std::vector<Slot> slots_;

public:
    explicit MessageBus(size_t num = 4) : slots_(num) {}

    // 生产者：最新帧覆盖（Mat 为引用计数浅拷贝，开销很小）
    void push(const CameraFrame& frame, const int idx) {
        Slot& s = slots_[idx];
        std::lock_guard<std::mutex> lock(s.mtx);
        s.frame = frame;
        s.has_frame = true;
        s.is_fresh = true;
    }

    // 消费者：取最新帧，返回 true 表示有帧可用。
    // fresh_out（可选）返回这是否是自上次取走后的新帧，用于跳过重复拼接。
    bool pop(CameraFrame& frame, const int idx, bool* fresh_out = nullptr) {
        Slot& s = slots_[idx];
        std::lock_guard<std::mutex> lock(s.mtx);
        if (!s.has_frame) return false;
        frame = s.frame;
        if (fresh_out) *fresh_out = s.is_fresh;
        s.is_fresh = false;
        return true;
    }
};
