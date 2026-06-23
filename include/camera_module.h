#pragma once
#include "module.h"
#include <opencv2/opencv.hpp>
#include <chrono>
#include <string>
#include <utility>

// 通用相机采集模块：所有相机共用同一份逻辑，仅通过构造参数区分
// 设备路径、相机编号、总线槽位，以及可选的 ROI 裁剪区域。
// 这样 4 路相机不再需要 4 份重复代码，新增相机只是多一行实例化。
class CameraModule : public Module {
public:
    CameraModule(std::string device, int cam_id, int bus_index,
                 cv::Rect roi = cv::Rect())
        : device_(std::move(device)), cam_id_(cam_id),
          bus_index_(bus_index), roi_(roi) {}

    bool init() override {
        cap_.open(device_);
        cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        cap_.set(cv::CAP_PROP_FRAME_WIDTH, kFrameWidth);
        cap_.set(cv::CAP_PROP_FRAME_HEIGHT, kFrameHeight);
        cap_.set(cv::CAP_PROP_FPS, kFps);
        return cap_.isOpened();
    }

    void run() override {
        cv::Mat frame;
        cap_.read(frame);
        if (frame.empty()) return;

        cv::Mat out = frame;
        if (roi_.width > 0 && roi_.height > 0) {
            // 与画面相交做裁剪，避免 ROI 越界导致崩溃
            cv::Rect safe = roi_ & cv::Rect(0, 0, frame.cols, frame.rows);
            if (safe.width > 0 && safe.height > 0) out = frame(safe);
        }

        CameraFrame f;
        f.ts = std::chrono::system_clock::now().time_since_epoch().count();
        f.cam_id = cam_id_;
        f.image = out;
        bus->push(f, bus_index_);
    }

private:
    static constexpr int kFrameWidth = 800;
    static constexpr int kFrameHeight = 600;
    static constexpr int kFps = 30;

    cv::VideoCapture cap_;
    std::string device_;
    int cam_id_;
    int bus_index_;
    cv::Rect roi_;
};
