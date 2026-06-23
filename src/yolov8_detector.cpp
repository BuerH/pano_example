#include "yolov8_detector.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

constexpr int kColorPalette[][3] = {
    {255, 56, 56},
    {255, 157, 151},
    {255, 112, 31},
    {255, 178, 29},
    {207, 210, 49},
    {72, 249, 10},
    {146, 204, 23},
    {61, 219, 134},
    {26, 147, 52},
    {0, 212, 187},
    {44, 153, 168},
    {0, 194, 255},
    {52, 69, 147},
    {100, 115, 255},
    {0, 24, 236},
    {132, 56, 255},
    {82, 0, 133},
    {203, 56, 255},
    {255, 149, 200},
    {255, 55, 199},
};

}  // namespace

YoloV8Detector::YoloV8Detector() = default;

YoloV8Detector::YoloV8Detector(Config config) : config_(std::move(config)) {}

bool YoloV8Detector::init()
{
    if (initialized_) return true;

    if (config_.model_path.empty()) {
        std::cerr << "[YOLOv8] 未配置模型路径，请设置 YOLOV8_MODEL_PATH" << std::endl;
        return false;
    }

    if (!std::filesystem::exists(config_.model_path)) {
        std::cerr << "[YOLOv8] 模型文件不存在: " << config_.model_path << std::endl;
        return false;
    }

    try {
        net_ = cv::dnn::readNetFromONNX(config_.model_path);
        net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        load_class_names();
        initialized_ = !net_.empty();
    } catch (const cv::Exception& e) {
        std::cerr << "[YOLOv8] 模型加载失败: " << e.what() << std::endl;
        initialized_ = false;
    }

    if (initialized_) {
        std::cout << "[YOLOv8] 模型加载成功: " << config_.model_path << std::endl;
    }

    return initialized_;
}

bool YoloV8Detector::is_initialized() const
{
    return initialized_;
}

cv::Mat YoloV8Detector::infer(const cv::Mat& image)
{
    if (image.empty()) return image.clone();

    cv::Mat rendered = image.clone();
    if (!initialized_) return rendered;

    try {
        LetterBoxInfo info;
        cv::Mat input_image = preprocess(image, info);
        cv::Mat blob = cv::dnn::blobFromImage(
            input_image,
            1.0 / 255.0,
            cv::Size(config_.input_width, config_.input_height),
            cv::Scalar(),
            true,
            false
        );

        net_.setInput(blob);

        std::vector<cv::Mat> outputs;
        net_.forward(outputs, net_.getUnconnectedOutLayersNames());
        if (outputs.empty()) return rendered;

        cv::Mat detections = reshape_output(outputs[0]);
        if (detections.empty() || detections.cols < 5) return rendered;

        const bool has_objectness = (!class_names_.empty() && detections.cols == static_cast<int>(class_names_.size()) + 5)
            || (class_names_.empty() && detections.cols == 85);
        const int class_offset = has_objectness ? 5 : 4;
        const int class_count = detections.cols - class_offset;
        if (class_count <= 0) return rendered;

        std::vector<int> class_ids;
        std::vector<float> confidences;
        std::vector<cv::Rect> boxes;

        for (int row = 0; row < detections.rows; ++row) {
            const float* data = detections.ptr<float>(row);

            float objectness = has_objectness ? data[4] : 1.0f;
            if (has_objectness && objectness < config_.conf_threshold) continue;

            cv::Mat scores(1, class_count, CV_32F, const_cast<float*>(data + class_offset));
            cv::Point class_id_point;
            double max_score = 0.0;
            cv::minMaxLoc(scores, nullptr, &max_score, nullptr, &class_id_point);

            const float class_score = static_cast<float>(max_score);
            const float confidence = objectness * class_score;
            if (class_score < config_.score_threshold || confidence < config_.conf_threshold) continue;

            const float center_x = data[0];
            const float center_y = data[1];
            const float width = data[2];
            const float height = data[3];

            const float left = (center_x - width * 0.5f - static_cast<float>(info.pad_x)) / info.scale;
            const float top = (center_y - height * 0.5f - static_cast<float>(info.pad_y)) / info.scale;
            const float right = (center_x + width * 0.5f - static_cast<float>(info.pad_x)) / info.scale;
            const float bottom = (center_y + height * 0.5f - static_cast<float>(info.pad_y)) / info.scale;

            const int x1 = std::clamp(static_cast<int>(std::round(left)), 0, image.cols - 1);
            const int y1 = std::clamp(static_cast<int>(std::round(top)), 0, image.rows - 1);
            const int x2 = std::clamp(static_cast<int>(std::round(right)), 0, image.cols - 1);
            const int y2 = std::clamp(static_cast<int>(std::round(bottom)), 0, image.rows - 1);

            if (x2 <= x1 || y2 <= y1) continue;

            boxes.emplace_back(x1, y1, x2 - x1, y2 - y1);
            confidences.push_back(confidence);
            class_ids.push_back(class_id_point.x);
        }

        if (boxes.empty()) return rendered;

        std::vector<int> indices;
        cv::dnn::NMSBoxes(boxes, confidences, config_.conf_threshold, config_.nms_threshold, indices);

        for (int index : indices) {
            const cv::Rect& box = boxes[index];
            const cv::Scalar color = color_for(class_ids[index]);
            cv::rectangle(rendered, box, color, 2);

            std::ostringstream stream;
            stream << label_for(class_ids[index]) << ' ' << std::fixed << std::setprecision(2) << confidences[index];
            const std::string text = stream.str();

            int baseline = 0;
            const cv::Size text_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
            const int text_top = std::max(box.y, text_size.height + 6);
            const cv::Rect text_box(
                box.x,
                text_top - text_size.height - 6,
                text_size.width + 8,
                text_size.height + 8
            );

            cv::rectangle(rendered, text_box, color, cv::FILLED);
            cv::putText(
                rendered,
                text,
                cv::Point(box.x + 4, text_top - 4),
                cv::FONT_HERSHEY_SIMPLEX,
                0.5,
                cv::Scalar(255, 255, 255),
                1,
                cv::LINE_AA
            );
        }
    } catch (const std::exception& e) {
        std::cerr << "[YOLOv8] 推理失败: " << e.what() << std::endl;
        return rendered;
    }

    return rendered;
}

cv::Mat YoloV8Detector::preprocess(const cv::Mat& image, LetterBoxInfo& info) const
{
    const float scale_x = static_cast<float>(config_.input_width) / static_cast<float>(image.cols);
    const float scale_y = static_cast<float>(config_.input_height) / static_cast<float>(image.rows);
    info.scale = std::min(scale_x, scale_y);

    const int resized_width = static_cast<int>(std::round(static_cast<float>(image.cols) * info.scale));
    const int resized_height = static_cast<int>(std::round(static_cast<float>(image.rows) * info.scale));
    info.pad_x = (config_.input_width - resized_width) / 2;
    info.pad_y = (config_.input_height - resized_height) / 2;

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(resized_width, resized_height));

    cv::Mat output(config_.input_height, config_.input_width, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(output(cv::Rect(info.pad_x, info.pad_y, resized_width, resized_height)));
    return output;
}

cv::Mat YoloV8Detector::reshape_output(const cv::Mat& output) const
{
    if (output.dims == 2) {
        if (output.rows < output.cols) {
            cv::Mat transposed;
            cv::transpose(output, transposed);
            return transposed;
        }
        return output;
    }

    if (output.dims != 3) {
        throw std::runtime_error("[YOLOv8] 不支持的输出维度");
    }

    const int size1 = output.size[1];
    const int size2 = output.size[2];

    if (size1 > size2) {
        return cv::Mat(size1, size2, CV_32F, const_cast<float*>(output.ptr<float>()));
    }

    cv::Mat transposed(size2, size1, CV_32F);
    cv::Mat original(size1, size2, CV_32F, const_cast<float*>(output.ptr<float>()));
    cv::transpose(original, transposed);
    return transposed;
}

void YoloV8Detector::load_class_names()
{
    class_names_.clear();

    if (config_.classes_path.empty()) return;
    if (!std::filesystem::exists(config_.classes_path)) {
        std::cerr << "[YOLOv8] 类别文件不存在: " << config_.classes_path << std::endl;
        return;
    }

    std::ifstream file(config_.classes_path);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) class_names_.push_back(line);
    }
}

std::string YoloV8Detector::label_for(int class_id) const
{
    if (class_id >= 0 && class_id < static_cast<int>(class_names_.size())) {
        return class_names_[class_id];
    }

    return "class_" + std::to_string(class_id);
}

cv::Scalar YoloV8Detector::color_for(int class_id) const
{
    const int palette_size = static_cast<int>(sizeof(kColorPalette) / sizeof(kColorPalette[0]));
    const int color_index = class_id >= 0 ? class_id % palette_size : 0;
    return cv::Scalar(
        kColorPalette[color_index][0],
        kColorPalette[color_index][1],
        kColorPalette[color_index][2]
    );
}
