#include "camera_module.h"

// 相机 4：/dev/video6，总线槽位 3，裁掉顶部 20px
extern "C" Module* create() {
    return new CameraModule("/dev/video6", 4, 3, cv::Rect(0, 20, 800, 580));
}
