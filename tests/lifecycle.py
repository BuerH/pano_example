from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'opencv4'], text=True).split()
    subprocess.run(['g++', '-std=c++17', '-fPIC', '-shared', '-fno-gnu-unique', '-I'+str(root/'include'), str(root/'tests/lifecycle_mock.cpp'), '-o', str(work/'mock.so'), '-pthread'] + flags, check=True)
    subprocess.run(['g++', '-std=c++17', '-I'+str(root/'include'), str(root/'tests/lifecycle_harness.cpp'), '-o', str(work/'check'), '-ldl', '-pthread'] + flags, check=True)
    for mode in ['success', 'fail', 'throw', 'null']:
        env = dict(os.environ, MOCK_MODE=mode)
        env.pop('MOCK_NULL', None)
        if mode == 'null': env['MOCK_NULL'] = '1'
        result = subprocess.run([str(work/'check')], cwd=work, env=env, timeout=10)
        assert result.returncode == (0 if mode == 'success' else 1), mode
        events = (work/'events').read_text().splitlines()
        assert events == (['unload'] if mode == 'null' else ['destroy', 'unload']), (mode, events)
        (work/'events').unlink()
        print(mode + ': passed', flush=True)
