#pragma once
#include <opencv2/opencv.hpp>
#include <chrono>

using namespace cv;
using namespace std;

struct CameraFrame {
    uint64_t ts;
    int cam_id;
    Mat image;
};

