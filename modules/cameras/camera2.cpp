#include "camera_module.h"

// 相机 2：/dev/video2，总线槽位 1，使用完整画面（不裁剪）
extern "C" Module* create() {
    return new CameraModule("/dev/video2", 2, 1);
}
