#pragma once
#include "message_bus.h"

// 流水线模块基类：每个模块被编译为独立的 .so，由主程序动态加载，
// 并在各自的线程里反复调用 run()。模块之间只通过 MessageBus 通信。
class Module {
public:
    MessageBus* bus = nullptr;
    virtual ~Module() = default;
    virtual bool init() = 0;  // 初始化（打开设备、加载模型等），失败返回 false
    virtual void run() = 0;   // 单次处理，会被循环调用
};

// 每个模块的 .so 都需导出该工厂函数。
extern "C" Module* create();
