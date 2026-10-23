# panorama_pipeline

多路相机实时全景拼接 + 目标检测的流水线程序。

四路 USB 相机各自采集图像，经由线程安全的消息总线汇集到显示模块，
显示模块调用 [libpanorama](https://github.com/BuerH/pano) 完成实时拼接，再用 YOLOv8 做目标检测并显示。

## 架构

程序由若干 **模块（Module）** 组成，每个模块编译为独立的 `.so`，
由主程序在运行时动态加载（`dlopen`），并各自跑在一个线程里循环调用 `run()`。
模块之间只通过 `MessageBus` 通信，互不直接依赖。

```
  camera1.so ┐
  camera2.so ├─ push ─►  MessageBus  ─ pop ─►  display.so ─► 拼接 + YOLO + 显示
  camera3.so │          (每路最新帧)
  camera4.so ┘
```

- **MessageBus**：每路相机一个槽位，「最新帧覆盖」语义（不排队、低延迟），
  内部加锁保证多线程安全。详见 [include/message_bus.h](include/message_bus.h)。
- **CameraModule**：四路相机共用同一份采集逻辑，仅通过构造参数区分设备路径、
  相机编号、总线槽位和裁剪 ROI。新增相机只需多一行实例化。
  详见 [include/camera_module.h](include/camera_module.h)。
- **DisplayModule**：汇集四路最新帧，仅在有新帧时才拼接，避免对相同画面重复
  计算；拼接后跑 YOLOv8 检测并显示。详见 [modules/display/display.cpp](modules/display/display.cpp)。

## 目录结构

```
panorama_pipeline/
├── include/                # 公共头文件
│   ├── module.h            # 模块基类 Module + create() 工厂约定
│   ├── camera_module.h     # 通用相机采集模块
│   ├── message_bus.h       # 线程安全消息总线（最新帧覆盖）
│   ├── common_def.h        # CameraFrame 等公共定义
│   └── yolov8_detector.h
├── src/
│   ├── main.cpp            # 主程序：加载模块、起线程、监听退出
│   └── yolov8_detector.cpp
├── modules/
│   ├── cameras/            # 四路相机的实例化（camera1..4.cpp）
│   └── display/            # 显示 / 拼接 / 检测模块
├── config/                 # 相机标定参数（CameraParameterList*.yml）
├── models/                 # YOLOv8 模型与类别名（yolov8n.onnx, coco.names）
└── Makefile
```

## 依赖

- OpenCV 4（`pkg-config --cflags --libs opencv4` 可用）
- [libpanorama](https://github.com/BuerH/pano)：拼接库，先安装匹配系统版本的
  `libpanorama1` 和 `libpanorama-dev` 安装包。包内头文件位于 `/usr/include`，
  amd64 库位于 `/usr/lib/x86_64-linux-gnu`，由编译器的系统搜索路径查找
  （`display` 目标链接 `-lpanorama`）。
- 四路 V4L2 相机，默认设备 `/dev/video0,2,4,6`，分辨率 800×600 @ 30fps。

## 构建

```bash
make            # 编译主程序、四个相机模块和显示模块
make clean      # 清理产物
```

产物：可执行文件 `panorama_pipeline`，模块 `.so` 输出到 `lib/`。

默认使用安装包的 `/usr` 前缀。如果从源码安装到 `/usr/local`，使用：

```bash
make clean
make PANORAMA_PREFIX=/usr/local
```

其他安装位置可用 `PANORAMA_PREFIX` 指定；库目录不在该前缀的 `lib/` 下时，
另行指定 `PANORAMA_LIBDIR=/实际/库目录`。自定义库目录还需加入系统动态库
搜索配置，或在运行时通过 `LD_LIBRARY_PATH` 指定。

## 运行

```bash
./panorama_pipeline
```

- 程序会按顺序打开 `lib/camera1.so` … `camera4.so`（对应总线槽位 0–3），
  再加载 `lib/display.so`。
- 运行时按 `q` + 回车安全退出。
- 模型路径默认 `./models/yolov8n.onnx` 与 `./models/coco.names`，
  也可通过环境变量覆盖：`YOLOV8_MODEL_PATH`、`YOLOV8_CLASSES_PATH`、
  `YOLOV8_CONF_THRESHOLD`、`YOLOV8_SCORE_THRESHOLD`、`YOLOV8_NMS_THRESHOLD`、
  `YOLOV8_INPUT_WIDTH`、`YOLOV8_INPUT_HEIGHT`。

## 自定义相机

修改对应 `modules/cameras/cameraN.cpp` 中的实例化参数即可：

```cpp
#include "camera_module.h"
extern "C" Module* create() {
    // 设备路径, 相机编号, 总线槽位, 可选 ROI 裁剪
    return new CameraModule("/dev/video0", 1, 0, cv::Rect(0, 10, 800, 580));
}
```

不传 ROI（或宽高为 0）则使用完整画面。

必需模块加载或初始化失败时，程序释放已加载模块并以非零状态退出。
正常退出时先等待工作线程结束，再销毁模块并卸载动态库，确保相机和录像资源释放。
标准输入关闭（EOF）也会触发退出。

## 回归验证

安装上述 OpenCV 和 C++ 构建依赖以及 Python 3 后执行：

```bash
python3 tests/lifecycle.py
```

测试使用模拟动态模块，无需摄像头和桌面环境，覆盖正常加载、初始化失败、
初始化抛出异常、工厂返回空指针，以及先销毁模块再卸载动态库的顺序。
