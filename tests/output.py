"""Test real HTTP MJPEG output and optional YOLO without cameras or a desktop."""
from pathlib import Path
import argparse
import http.client
import os
import socket
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--panorama-include', type=Path, default=(
    root.parent / 'include' if (root.parent / 'include/panorama_api.h').exists() else Path('/usr/include')))
args = parser.parse_args()

def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

with tempfile.TemporaryDirectory(prefix='pano-output-') as tmp:
    work = Path(tmp)
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'opencv4'], text=True).split()
    common = ['g++', '-std=c++17', '-Wall', '-Wextra', '-pthread', '-I'+str(root/'include')]
    subprocess.run(common + ['-I'+str(args.panorama_include), str(root/'tests/output_harness.cpp'),
        str(root/'src/yolov8_detector.cpp'), str(root/'src/mjpeg_server.cpp'),
        '-o', str(work/'output')] + flags, check=True)
    subprocess.run(common + [str(root/'src/main.cpp'), '-ldl', '-o', str(work/'pipeline')] + flags, check=True)
    (work/'player.cpp').write_text('''#include <opencv2/videoio.hpp>
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    cv::VideoCapture capture;
    if (!capture.open(argv[1], cv::CAP_FFMPEG, {
        cv::CAP_PROP_OPEN_TIMEOUT_MSEC, 3000, cv::CAP_PROP_READ_TIMEOUT_MSEC, 3000})) return 2;
    cv::Mat image;
    return capture.read(image) && image.cols == 160 && image.rows == 70 ? 0 : 3;
}
''')
    subprocess.run(common + [str(work/'player.cpp'), '-o', str(work/'player')] + flags, check=True)
    help_text = subprocess.check_output([str(work/'pipeline'), '--help'], text=True)
    assert '--yolo' in help_text and '15678' in help_text and '--output' in help_text
    for option in [['--port', '0'], ['--port', '65536'], ['--port', '-1'],
                   ['--port', '1x'], ['--port', '999999999999999999999999'],
                   ['--output', 'bad'], ['--output'], ['--port']]:
        result = subprocess.run([str(work/'pipeline'), *option], cwd=work, capture_output=True)
        assert result.returncode != 0 and '加载失败' not in result.stderr.decode(), option
    print('CLI validation: passed', flush=True)
    env = dict(os.environ, YOLOV8_MODEL_PATH='/missing/model.onnx')
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    port = free_port()
    process = subprocess.Popen([str(work/'output'), str(port), 'off'], cwd=work,
        env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    connections = []
    try:
        while True:
            line = process.stdout.readline()
            assert line, process.stderr.read()
            if line.strip() == 'READY': break
        def connect(path='/stream'):
            conn = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
            connections.append(conn)
            conn.request('GET', path)
            return conn.getresponse()
        first, second = connect(), connect('/')
        assert first.status == second.status == 200
        assert 'boundary=pano-frame' in first.getheader('Content-Type')
        assert connect('/missing').status == 404
        process.stdin.write('empty\n')
        process.stdin.flush()
        assert process.stdout.readline().strip() == 'PUBLISHED'
        process.stdin.write('frame\n')
        process.stdin.flush()
        assert process.stdout.readline().strip() == 'PUBLISHED'
        def read_frame(response):
            assert response.readline() == b'--pano-frame\r\n'
            assert response.readline() == b'Content-Type: image/jpeg\r\n'
            length = int(response.readline().split(b':')[1])
            assert response.readline() == b'\r\n'
            jpeg = response.read(length)
            assert jpeg[:2] == b'\xff\xd8' and jpeg[-2:] == b'\xff\xd9'
            assert response.read(2) == b'\r\n'
            (work/'frame.jpg').write_bytes(jpeg)
        read_frame(first)
        read_frame(second)
        # A newly connected player immediately receives the latest frame.
        read_frame(connect())
        # A real video reader must recognize and decode the multipart stream.
        player = subprocess.Popen([str(work/'player'), f'http://127.0.0.1:{port}/stream'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            for _ in range(60):
                if player.poll() is not None: break
                process.stdin.write('frame\n')
                process.stdin.flush()
                assert process.stdout.readline().strip() == 'PUBLISHED'
                time.sleep(0.02)
            player_out, player_err = player.communicate(timeout=8)
            assert player.returncode == 0, (player_out, player_err)
        finally:
            if player.poll() is None:
                player.kill()
                player.wait()
        print('Real OpenCV/FFmpeg player decoded the live HTTP stream: passed', flush=True)
        # Leave a socket unread while publishing; frame production must keep moving.
        slow = socket.create_connection(('127.0.0.1', port), timeout=5)
        connections.append(slow)
        slow.sendall(b'GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n')
        started = time.monotonic()
        for _ in range(40):
            process.stdin.write('large\n')
            process.stdin.flush()
            assert process.stdout.readline().strip() == 'PUBLISHED'
        assert time.monotonic() - started < 5
        # Occupied port must fail initialization instead of silently losing output.
        conflict = subprocess.run([str(work/'output'), str(port), 'off'], env=env,
            input='quit\n', capture_output=True, text=True, timeout=10)
        assert conflict.returncode == 2 and 'MJPEG listen' in conflict.stderr
        print('Headless output, multiple players, reconnect, slow reader, port conflict: passed', flush=True)
    finally:
        for connection in connections: connection.close()
        process.stdin.write('quit\n')
        process.stdin.flush()
        out, err = process.communicate(timeout=5)
        assert process.returncode == 0, (out, err)
    enabled = subprocess.run([str(work/'output'), str(free_port()), 'yolo'], cwd=work,
        env=env, input='quit\n', capture_output=True, text=True, timeout=10)
    assert enabled.returncode == 2 and '模型文件不存在' in enabled.stderr
    print('YOLO disabled ignores missing model; explicit enable validates model: passed', flush=True)
    # Decode the received image with real OpenCV, independent of Python cv2 availability.
    (work/'decode.cpp').write_text('''#include <opencv2/imgcodecs.hpp>
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    auto image = cv::imread(argv[1]);
    return image.cols == 160 && image.rows == 70 ? 0 : 1;
}
''')
    subprocess.run(common + [str(work/'decode.cpp'), '-o', str(work/'decode')] + flags, check=True)
    subprocess.run([str(work/'decode'), str(work/'frame.jpg')], check=True)
    print('Received JPEG decodes at the expected cropped resolution: passed', flush=True)
