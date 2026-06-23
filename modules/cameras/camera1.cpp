#include "camera_module.h"

// 相机 1：/dev/video0，总线槽位 0，裁掉顶部 10px
extern "C" Module* create() {
    return new CameraModule("/dev/video0", 1, 0, cv::Rect(0, 10, 800, 580));
}
