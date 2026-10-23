#include "module.h"
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>
static void event(const char* text) { std::ofstream("events", std::ios::app) << text << std::endl; }
class Mock : public Module {
public:
    ~Mock() override { event("destroy"); }
    bool init() override {
        const char* mode = std::getenv("MOCK_MODE");
        if (mode && std::string(mode) == "throw") throw std::runtime_error("test exception");
        return !mode || std::string(mode) != "fail";
    }
    void run() override { std::this_thread::yield(); }
};
extern "C" Module* create() { return std::getenv("MOCK_NULL") ? nullptr : new Mock; }
__attribute__((destructor)) static void unloaded() { event("unload"); }
