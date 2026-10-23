#define main pipeline_main
#include "../src/main.cpp"
#undef main
int main() {
    MessageBus bus;
    Module* m = load_module("./mock.so", &bus);
    bool ok = m != nullptr;
    loaded_modules.clear();
    return ok ? 0 : 1;
}
