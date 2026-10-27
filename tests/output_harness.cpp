// Exercise the actual display module without cameras or a production license.
static bool empty_panorama = false;
#include "../modules/display/display.cpp"

PanoramaAPI::PanoramaAPI() : stitcher_impl(nullptr) {}
PanoramaAPI::~PanoramaAPI() = default;
cv::Mat PanoramaAPI::stitch(const std::vector<cv::Mat>& input) {
    return empty_panorama ? cv::Mat{} : input.front().clone();
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        MessageBus bus;
        DisplayModule display;
        display.bus = &bus;
        display.options.stream = true;
        display.options.port = static_cast<unsigned short>(std::stoi(argv[1]));
        display.options.yolo = std::string(argv[2]) == "yolo";
        if (!display.init()) return 2;
        std::cout << "READY" << std::endl;
        std::string command;
        while (std::getline(std::cin, command) && command != "quit") {
            empty_panorama = command == "empty";
            for (int i = 0; i < 4; ++i) {
                CameraFrame frame{};
                if (command == "large") {
                    frame.image = cv::Mat(600, 800, CV_8UC3);
                    cv::randu(frame.image, cv::Scalar::all(0), cv::Scalar::all(255));
                } else {
                    frame.image = cv::Mat(100, 160, CV_8UC3, cv::Scalar(30, 90, 180));
                }
                bus.push(frame, i);
            }
            display.run();
            std::cout << "PUBLISHED" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 2;
    }
}
