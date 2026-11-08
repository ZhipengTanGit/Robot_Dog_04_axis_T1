"""Build/run peripheral unit tests without a serial port or robot connection."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--idf', default=os.environ.get('IDF_PATH'))
    args = parser.parse_args()
    if not args.idf:
        parser.error('Set IDF_PATH or pass --idf pointing to ESP-IDF 5.4.4')
    cjson = Path(args.idf).resolve() / 'components/json/cJSON'
    if not (cjson / 'cJSON.c').is_file():
        parser.error('ESP-IDF cJSON source was not found')
    output = ROOT / 'build/host_tests'
    output.mkdir(parents=True, exist_ok=True)
    board = ROOT / 'main/boards/robot-dog-v024'
    component = ROOT / 'components/robot_peripherals'
    env = os.environ.copy()
    compiler_dirs = [str(Path(p).resolve().parent) for p in (args.cc, args.cxx) if Path(p).is_file()]
    env['PATH'] = os.pathsep.join(compiler_dirs + [env.get('PATH', '')])

    def run(command):
        subprocess.run([str(p) for p in command], cwd=ROOT, env=env, check=True)

    suffix = '.exe' if os.name == 'nt' else ''
    cflags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I', component]
    cppflags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', board]
    completed = []
    for name, sources in [
        ('face', ['FaceLogic.c', 'FaceRender.c']),
        ('display_layout', ['DisplaySyncLogic.c']),
        ('display_sync', ['DisplaySyncLogic.c']),
        ('display_recovery', ['DisplayRecoveryLogic.c']),
    ]:
        executable = output / (name + suffix)
        run([args.cc, *cflags, ROOT / ('tests/test_' + name + '.c'),
             *(component / s for s in sources), '-lm', '-o', executable])
        run([executable])
        completed.append(name)
    executable = output / ('telemetry' + suffix)
    run([args.cxx, *cppflags, ROOT / 'tests/robot_telemetry_test.cc',
         board / 'robot_telemetry.cc', '-o', executable])
    run([executable])
    completed.append('telemetry')
    run([args.cc, '-I', cjson, '-c', cjson / 'cJSON.c', '-o', output / 'cJSON.o'])
    executable = output / ('stm32_link' + suffix)
    run([args.cxx, *cppflags, '-I', ROOT / 'tests/robot_fakes', '-I', cjson,
         ROOT / 'tests/robot_stm32_test.cc', board / 'robot_telemetry.cc',
         board / 'robot_stm32.cc', output / 'cJSON.o', '-o', executable])
    run([executable])
    completed.append('stm32_link')
    (output / 'results.json').write_text(json.dumps({'passed': completed}, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: {len(completed)} suites; no hardware was connected or commanded.')


if __name__ == '__main__':
    main()
