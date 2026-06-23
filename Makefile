CXX       = g++
INCLUDES  = -Iinclude
OPENCV    = `pkg-config --cflags --libs opencv4`
CXXFLAGS  = -std=c++17 -fPIC -Wall -Wno-write-strings -I/usr/local/include $(INCLUDES)
LIBS      = -L/usr/local/lib -lpanorama

BIN       = panorama_pipeline
LIBDIR    = lib

CAMERAS   = camera1 camera2 camera3 camera4

all: $(BIN) $(CAMERAS) display

# 主程序：动态加载各模块 .so
$(BIN): src/main.cpp
	$(CXX) $^ -o $(BIN) $(CXXFLAGS) $(OPENCV) -ldl -pthread

# 相机模块：四路共用 camera_module.h，仅实例化参数不同
$(CAMERAS): %: modules/cameras/%.cpp
	@mkdir -p $(LIBDIR)
	$(CXX) $< -shared -o $(LIBDIR)/$@.so $(CXXFLAGS) $(OPENCV)

# 显示模块：拼接 + YOLO 检测 + 显示，需链接 libpanorama
display:
	@mkdir -p $(LIBDIR)
	$(CXX) modules/display/display.cpp src/yolov8_detector.cpp -shared -o $(LIBDIR)/display.so $(CXXFLAGS) $(OPENCV) $(LIBS)

clean:
	rm -rf $(BIN) $(LIBDIR)

.PHONY: all clean display $(CAMERAS)
