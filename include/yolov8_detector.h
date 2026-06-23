#pragma once

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <string>
#include <vector>

class YoloV8Detector {
public:
    struct Config {
        std::string model_path;
        std::string classes_path;
        float conf_threshold = 0.5f;
        float score_threshold = 0.5f;
        float nms_threshold = 0.5f;
        int input_width = 640;
        int input_height = 640;
    };

    YoloV8Detector();
    explicit YoloV8Detector(Config config);

    bool init();
    bool is_initialized() const;
    cv::Mat infer(const cv::Mat& image);

private:
    struct LetterBoxInfo {
        float scale = 1.0f;
        int pad_x = 0;
        int pad_y = 0;
    };

    cv::Mat preprocess(const cv::Mat& image, LetterBoxInfo& info) const;
    cv::Mat reshape_output(const cv::Mat& output) const;
    void load_class_names();
    std::string label_for(int class_id) const;
    cv::Scalar color_for(int class_id) const;

    Config config_;
    cv::dnn::Net net_;
    std::vector<std::string> class_names_;
    bool initialized_ = false;
};
