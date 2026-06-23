#include "module.h"
#include "yolov8_detector.h"
#include <opencv2/highgui.hpp>
#include "panorama_api.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <thread>
#include <cstdlib>
#include <filesystem>
using namespace std;

class DisplayModule : public Module
{
private:
    PanoramaAPI api;
    vector<cv::Mat> frames;  // 只用于临时存4路帧，本地变量，无多线程竞争
    YoloV8Detector detector;

    static YoloV8Detector::Config load_detector_config()
    {
        YoloV8Detector::Config config;
        const std::string default_model_path = "./models/yolov8n.onnx";
        const std::string default_classes_path = "./models/coco.names";

        const char* model_path = getenv("YOLOV8_MODEL_PATH");
        const char* classes_path = getenv("YOLOV8_CLASSES_PATH");
        const char* conf_threshold = getenv("YOLOV8_CONF_THRESHOLD");
        const char* score_threshold = getenv("YOLOV8_SCORE_THRESHOLD");
        const char* nms_threshold = getenv("YOLOV8_NMS_THRESHOLD");
        const char* input_width = getenv("YOLOV8_INPUT_WIDTH");
        const char* input_height = getenv("YOLOV8_INPUT_HEIGHT");

        if (model_path) {
            config.model_path = model_path;
        } else if (std::filesystem::exists(default_model_path)) {
            config.model_path = default_model_path;
        }

        if (classes_path) {
            config.classes_path = classes_path;
        } else if (std::filesystem::exists(default_classes_path)) {
            config.classes_path = default_classes_path;
        }

        if (conf_threshold) config.conf_threshold = static_cast<float>(atof(conf_threshold));
        if (score_threshold) config.score_threshold = static_cast<float>(atof(score_threshold));
        if (nms_threshold) config.nms_threshold = static_cast<float>(atof(nms_threshold));
        if (input_width) config.input_width = atoi(input_width);
        if (input_height) config.input_height = atoi(input_height);

        return config;
    }

public:
    DisplayModule() : detector(load_detector_config()) {}

    bool init() override
    {
        cv::namedWindow("PANO", cv::WINDOW_NORMAL);
        frames.resize(4);
        detector.init();
        return true;
    }

    void run() override
    {
        CameraFrame frame;

        // 从总线取最新帧（非阻塞）。only 在有新帧时才更新本地缓存，
        // 这样可以判断本轮是否真的有新数据，避免对相同帧重复拼接。
        bool any_fresh = false;
        for(int i = 0; i < 4; i++) {
            bool fresh = false;
            if (bus->pop(frame, i, &fresh) && fresh) {
                frames[i] = frame.image.clone();
                any_fresh = true;
            }
        }

        // 检查4帧是否都齐了
        bool all_ready = true;
        for (auto& f : frames)
        {
            if (f.empty())
            {
                all_ready = false;
                break;
            }
        }

        if (!all_ready)
        {
            // 还没集齐 4 路，短暂让出 CPU，避免空转抢占拼接/推理线程的核
            this_thread::sleep_for(chrono::milliseconds(5));
            return;
        }

        // 没有任何新帧就不重复拼接，省下一次 stitch + YOLO 的开销
        if (!any_fresh)
        {
            this_thread::sleep_for(chrono::milliseconds(1));
            return;
        }

        // 拼接 + 显示
        cv::Mat panorama = api.stitch(frames);
        cv::Mat detected_panorama = detector.infer(panorama);

        const int roi_y = std::min(30, std::max(0, detected_panorama.rows - 1));
        const int roi_height = detected_panorama.rows - roi_y;
        if (roi_height <= 0) return;

        const int crop_height = std::min(510, roi_height);
        Mat small_panorama = detected_panorama(Rect(0, roi_y, detected_panorama.cols, crop_height));

        // cv::imshow("PANO", panorama);
        cv::imshow("panorama", small_panorama);
        cv::waitKey(1);
    }
};

extern "C" Module* create()
{
    return new DisplayModule();
}
