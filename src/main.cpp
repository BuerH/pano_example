#include "module.h"
#include "message_bus.h"
#include <dlfcn.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;

// 全局退出标志：运行时为 false，按 q 回车后变为 true
atomic<bool> need_exit(false);

// 已加载的 .so 句柄，程序结束时统一释放
struct LoadedModule {
    void* handle;
    unique_ptr<Module> module;
    explicit LoadedModule(void* h) : handle(h) {}
    ~LoadedModule() {
        module.reset();
        dlclose(handle);
    }
};
vector<unique_ptr<LoadedModule>> loaded_modules;

// 动态加载一个模块 .so，调用其 create() 并完成 init()
Module* load_module(const char* path, MessageBus* bus, const ModuleOptions& options = {}) {
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

    auto loaded = make_unique<LoadedModule>(handle);
    try {
        loaded->module.reset(create());
        if (!loaded->module) throw runtime_error("create() returned null");
        loaded->module->bus = bus;
        loaded->module->options = options;
        if (!loaded->module->init()) {
            cerr << "初始化失败：" << path << endl;
            return nullptr;
        }
        Module* module = loaded->module.get();
        loaded_modules.push_back(std::move(loaded));
        return module;
    } catch (const exception& e) {
        cerr << "加载模块失败：" << path << ": " << e.what() << endl;
    } catch (...) {
        cerr << "加载模块失败：" << path << endl;
    }
    return nullptr;
}

// 监听键盘：按 q 退出
void input_thread() {
    int c;
    while (true) {
        c = getchar();
        if (c == EOF || c == 'q' || c == 'Q') {
            cout << "\n🚪 收到退出指令，正在关闭程序..." << endl;
            need_exit = true;
            break;
        }
    }
}

void print_usage(const char* prog) {
    cout << "用法: " << prog << " [--mode multi|single] [--yolo] [--output display|stream] [--port 15678]\n"
         << "  --yolo   : 开启 YOLOv8 检测（默认关闭）\n"
         << "  --output : display 本地显示（默认）；stream 输出 HTTP MJPEG，不创建窗口\n"
         << "  --port   : 网络输出端口，默认 15678，播放地址 http://设备IP:15678/stream\n"
         << "  multi  : 默认。每个相机模块和显示模块各自独立线程（并行采集）\n"
         << "  single : 单线程顺序执行，依次取 4 路帧后拼接显示\n"
         << "           （取帧方式类似 libpanorama/examples/param_tuner）" << endl;
}

int main(int argc, char** argv) {
    string mode = "multi";
    ModuleOptions options;
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if ((arg == "--mode" || arg == "-m") && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--yolo") {
            options.yolo = true;
        } else if (arg == "--output" && i + 1 < argc) {
            string output = argv[++i];
            if (output != "display" && output != "stream") {
                cerr << "无效输出方式: " << output << endl;
                return 1;
            }
            options.stream = output == "stream";
        } else if (arg == "--port" && i + 1 < argc) {
            string port = argv[++i];
            try {
                if (port.empty() || port.find_first_not_of("0123456789") != string::npos)
                    throw invalid_argument("port");
                unsigned long value = stoul(port);
                if (value == 0 || value > 65535) throw out_of_range("port");
                options.port = static_cast<unsigned short>(value);
            } catch (const exception&) {
                cerr << "无效端口（应为 1–65535）: " << port << endl;
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            cerr << "未知参数: " << arg << endl;
            print_usage(argv[0]);
            return -1;
        }
    }
    if (mode != "multi" && mode != "single") {
        cerr << "无效模式: " << mode << "（只支持 multi / single）" << endl;
        print_usage(argv[0]);
        return -1;
    }

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
        Module* camera = load_module(lib, &bus);
        if (!camera) {
            loaded_modules.clear();
            return 1;
        }
        cameras.push_back(camera);
        sleep(1); // 逐个打开，避免 V4L2 设备同时初始化冲突
    }

    cout << "=== 启动显示模块 ===" << endl;
    Module* display = load_module("./lib/display.so", &bus, options);
    if (!display) {
        loaded_modules.clear();
        return 1;
    }

    vector<thread> workers;
    if (mode == "multi") {
        // 每个模块跑在各自线程里，循环调用 run()
        cout << "=== 运行模式: multi（多线程并行采集） ===" << endl;
        for (Module* cam : cameras) {
            if (cam) workers.emplace_back([cam]() { while (!need_exit) cam->run(); });
        }
        if (display) workers.emplace_back([display]() { while (!need_exit) display->run(); });
    } else {
        // single：同一线程内依次取 4 路帧再拼接显示（同 param_tuner 的顺序取帧），
        // 每轮各路帧的采集时刻更接近同一次循环，便于排查多线程时序问题。
        cout << "=== 运行模式: single（单线程顺序采集） ===" << endl;
        workers.emplace_back([&cameras, display]() {
            while (!need_exit) {
                for (Module* cam : cameras) {
                    if (cam) cam->run();
                }
                if (display) display->run();
            }
        });
    }

    thread t_input(input_thread);

    for (auto& t : workers) t.join();
    t_input.join();

    loaded_modules.clear();

    cout << "✅ 程序已安全退出！" << endl;
    return 0;
}
