#include "module.h"
#include "yolov8_detector.h"
#include "mjpeg_server.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>
#include "panorama_api.h"
#include <iostream>
#include <chrono>
#include <ctime>
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
    unique_ptr<YoloV8Detector> detector;
    unique_ptr<MjpegServer> stream;
    bool window_open = false;

    cv::VideoWriter recorder;
    bool recording = false;
    cv::Size record_size;
    static constexpr double RECORD_FPS = 25.0;

    static std::string timestamp_filename()
    {
        time_t now = time(nullptr);
        tm tm_buf;
        localtime_r(&now, &tm_buf);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm_buf);
        return std::string(buf) + ".avi";
    }

    void toggle_recording(const cv::Mat& frame)
    {
        if (!recording)
        {
            std::string filename = timestamp_filename();
            // MJPG + avi：不依赖外部编码器，OpenCV 各版本都可用
            if (recorder.open(filename, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                              RECORD_FPS, frame.size()))
            {
                record_size = frame.size();
                recording = true;
                cout << "[display] 开始录制: " << filename << endl;
            }
            else
            {
                cerr << "[display] 录制启动失败，无法打开 " << filename << endl;
            }
        }
        else
        {
            recorder.release();
            recording = false;
            cout << "[display] 录制结束" << endl;
        }
    }

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
    bool init() override
    {
        frames.resize(4);
        if (options.yolo) {
            detector = make_unique<YoloV8Detector>(load_detector_config());
            if (!detector->init()) return false;
        }
        cout << "[display] YOLO: " << (options.yolo ? "开启" : "关闭") << endl;
        if (options.stream) {
            stream = make_unique<MjpegServer>(options.port);
            cout << "[display] HTTP MJPEG: http://0.0.0.0:" << options.port
                 << "/stream（客户端请使用本机实际 IP）" << endl;
        } else {
            cv::namedWindow("panorama", cv::WINDOW_NORMAL);
            window_open = true;
        }
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
        if (panorama.empty()) return;
        cv::Mat detected_panorama = detector ? detector->infer(panorama) : panorama;

        const int roi_y = std::min(30, std::max(0, detected_panorama.rows - 1));
        const int roi_height = detected_panorama.rows - roi_y;
        if (roi_height <= 0) return;

        const int crop_height = std::min(510, roi_height);
        Mat small_panorama = detected_panorama(Rect(0, roi_y, detected_panorama.cols, crop_height));

        // 先写入录制文件，再画 REC 标记，避免标记被录进视频
        if (recording)
        {
            cv::Mat record_frame = small_panorama;
            if (record_frame.size() != record_size)
            {
                // 拼接尺寸偶发变化时缩放到录制尺寸，保证文件可播放
                cv::resize(small_panorama, record_frame, record_size);
            }
            recorder.write(record_frame);

            cv::circle(small_panorama, cv::Point(25, 25), 8, cv::Scalar(0, 0, 255), cv::FILLED);
            cv::putText(small_panorama, "REC", cv::Point(40, 32),
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);
        }

        if (stream) {
            vector<unsigned char> jpeg;
            if (cv::imencode(".jpg", small_panorama, jpeg, {cv::IMWRITE_JPEG_QUALITY, 85}))
                stream->publish(jpeg);
            return;
        }
        cv::imshow("panorama", small_panorama);
        int key = cv::waitKey(1);
        if (key == 'r' || key == 'R')
            toggle_recording(small_panorama);
    }

    ~DisplayModule() override
    {
        stream.reset();
        if (window_open) cv::destroyWindow("panorama");
        if (recording)
        {
            recorder.release();
            cout << "[display] 录制结束（退出时自动保存）" << endl;
        }
    }
};

extern "C" Module* create()
{
    return new DisplayModule();
}
