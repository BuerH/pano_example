#include "camera_module.h"

// 相机 3：/dev/video4，总线槽位 2，裁掉顶部 20px
extern "C" Module* create() {
    return new CameraModule("/dev/video4", 3, 2, cv::Rect(0, 20, 800, 550));
}
