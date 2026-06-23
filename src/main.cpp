#include "module.h"
#include "message_bus.h"
#include <dlfcn.h>
#include <atomic>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;

// 全局退出标志：运行时为 false，按 q 回车后变为 true
atomic<bool> need_exit(false);

// 已加载的 .so 句柄，程序结束时统一释放
vector<void*> dl_handles;

// 动态加载一个模块 .so，调用其 create() 并完成 init()
Module* load_module(const char* path, MessageBus* bus) {
    void* handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
        cerr << "加载失败: " << dlerror() << endl;
        return nullptr;
    }

    auto create = reinterpret_cast<Module* (*)()>(dlsym(handle, "create"));
    if (!create) {
        cerr << "找不到 create(): " << dlerror() << endl;
        dlclose(handle);
        return nullptr;
    }

    Module* module = create();
    module->bus = bus;

    if (!module->init()) {
        cerr << "⚠️  初始化失败：" << path << endl;
    } else {
        cout << "✅ 初始化成功：" << path << endl;
    }

    dl_handles.push_back(handle);
    return module;
}

// 监听键盘：按 q 退出
void input_thread() {
    char c;
    while (true) {
        c = getchar();
        if (c == 'q' || c == 'Q') {
            cout << "\n🚪 收到退出指令，正在关闭程序..." << endl;
            need_exit = true;
            break;
        }
    }
}

int main() {
    MessageBus bus;

    // 相机加载顺序对应总线槽位 0→1→2→3（与拼接逻辑顺序一致）
    const vector<const char*> camera_libs = {
        "./lib/camera1.so",
        "./lib/camera2.so",
        "./lib/camera3.so",
        "./lib/camera4.so",
    };

    cout << "=== 按顺序打开相机 ===" << endl;
    cout << "=== 按 q + 回车 安全退出程序 ===" << endl;

    vector<Module*> cameras;
    for (const char* lib : camera_libs) {
        cameras.push_back(load_module(lib, &bus));
        sleep(1); // 逐个打开，避免 V4L2 设备同时初始化冲突
    }

    cout << "=== 启动显示模块 ===" << endl;
    Module* display = load_module("./lib/display.so", &bus);

    // 每个模块跑在各自线程里，循环调用 run()
    vector<thread> workers;
    for (Module* cam : cameras) {
        if (cam) workers.emplace_back([cam]() { while (!need_exit) cam->run(); });
    }
    if (display) workers.emplace_back([display]() { while (!need_exit) display->run(); });

    thread t_input(input_thread);

    for (auto& t : workers) t.join();
    t_input.join();

    for (void* h : dl_handles) dlclose(h);

    cout << "✅ 程序已安全退出！" << endl;
    return 0;
}
