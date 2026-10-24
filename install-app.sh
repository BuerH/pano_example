#!/usr/bin/env bash
set -euo pipefail

target="$PWD/pano-app"
base_url=https://wudu.bitshe.cn/pano
example_ref=master
jobs=2
work=
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
local_source=
die() { printf '错误: %s\n' "$*" >&2; exit 1; }
usage() {
    cat <<'EOF'
用法: bash install-app.sh [选项]

自动安装 PANO 二进制库，拉取并编译 pano_example，生成可运行的应用目录。
支持 Ubuntu 22.04 / 24.04 amd64，以及 Jetson Orin Ubuntu 20.04 arm64。
需要联网和 sudo 权限；Orin 自动安装配套 OpenCV 4.10.0（CPU 版本）。

  --dir PATH          应用目录（默认 当前目录/pano-app）
  --jobs N            编译并行任务数（默认 2）
  --example-ref REF   pano_example 的分支或标签（默认 master）
  --source PATH       使用本地 pano_example 源码；仓库内脚本默认使用所在仓库
  --base-url URL      下载站 HTTPS 根地址（默认 https://wudu.bitshe.cn/pano）
  -h, --help          显示帮助

安装后将设备授权文件放到应用目录的 license.lic，然后运行 ./pano。
相机标定缓存保存在 camera-cache/；重复安装保留许可和已有缓存。
EOF
}
while (($#)); do
    case "$1" in
        --dir|--jobs|--example-ref|--base-url|--source)
            (($# >= 2)) && [[ -n "$2" ]] || die "$1 缺少参数"
            case "$1" in
                --dir) target=$2 ;;
                --jobs) jobs=$2 ;;
                --example-ref) example_ref=$2; local_source=remote ;;
                --source) local_source=$2 ;;
                --base-url) base_url=${2%/} ;;
            esac
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "未知参数: $1" ;;
    esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die '--jobs 必须为正整数'
[[ "$base_url" == https://* ]] || die '下载站必须使用 HTTPS'
[[ -r /etc/os-release ]] || die '无法识别操作系统'
source /etc/os-release
[[ "${ID:-}" == ubuntu ]] || die '仅支持 Ubuntu 22.04/24.04 amd64 和 Ubuntu 20.04 arm64'
arch=$(dpkg --print-architecture)
case "$VERSION_ID/$arch/$(uname -m)" in
    22.04/amd64/x86_64|24.04/amd64/x86_64|20.04/arm64/aarch64) ;;
    *) die '仅支持 Ubuntu 22.04/24.04 amd64 和 Jetson Orin Ubuntu 20.04 arm64' ;;
esac
distro="ubuntu$VERSION_ID"
if [[ -z "$local_source" && -f "$script_dir/modules/display/display.cpp" ]]; then local_source=$script_dir; fi
if [[ -n "$local_source" && "$local_source" != remote ]]; then
    local_source=$(cd -- "$local_source" && pwd -P)
    [[ -f "$local_source/Makefile" && -f "$local_source/src/main.cpp" && -f "$local_source/modules/display/display.cpp" ]] || die '本地源码目录不是 pano_example'
fi
sudo_cmd=()
if ((EUID != 0)); then
    command -v sudo >/dev/null || die '需要 sudo 安装系统依赖'
    sudo_cmd=(sudo)
fi
printf '系统: %s / %s\n安装构建和下载依赖...\n' "$distro" "$arch"
"${sudo_cmd[@]}" apt-get update
"${sudo_cmd[@]}" apt-get install -y --no-install-recommends ca-certificates curl git build-essential pkg-config python3

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
download() { curl --proto '=https' --proto-redir '=https' --fail --location --retry 3 --connect-timeout 20 --output "$2" "$1"; }
download "$base_url/releases.json" "$work/releases.json"
python3 - "$work/releases.json" "$distro" "$arch" > "$work/packages" <<'PY'
import json, re, sys
manifest = json.load(open(sys.argv[1]))
arch = sys.argv[3]
if arch not in manifest.get('architectures', [manifest.get('architecture')]) or manifest.get('license_required') is not True:
    raise SystemExit('下载站没有匹配架构的授权版')
version = manifest['version']
if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
    raise SystemExit('无效版本号')
release = manifest.get('releases', {}).get(sys.argv[2])
if not release:
    raise SystemExit('下载站没有匹配系统的授权版')
packages = [('runtime', f'libpanorama1_{version}-1+{sys.argv[2]}_{arch}.deb'),
            ('dev', f'libpanorama-dev_{version}-1+{sys.argv[2]}_{arch}.deb')]
if arch == 'arm64':
    packages.insert(0, ('opencv', 'libpano-opencv410_4.10.0-1_arm64.deb'))
for kind, filename in packages:
    item = release[kind]
    href = f'downloads/{sys.argv[2]}/{arch}/{filename}'
    if item['filename'] != filename or item['href'] != href or not re.fullmatch(r'[a-f0-9]{64}', item['sha256']):
        raise SystemExit('下载清单的包路径或校验值无效')
    print(filename, item['sha256'], href)
PY
packages=()
while read -r filename digest href; do
    download "$base_url/$href" "$work/$filename"
    printf '%s  %s\n' "$digest" "$filename" >> "$work/SHA256SUMS"
    packages+=("$work/$filename")
done < "$work/packages"
(cd "$work" && sha256sum --check SHA256SUMS)
expected=2
[[ "$arch" != arm64 ]] || expected=3
[[ ${#packages[@]} == "$expected" ]] || die '运行包、开发包或配套依赖包缺失'
# apt's unprivileged download helper must be able to read the local packages.
chmod 755 "$work"
chmod 644 "${packages[@]}"
"${sudo_cmd[@]}" apt-get install -y "${packages[@]}"
if [[ "$arch" == arm64 ]]; then
    export OpenCV_DIR=/opt/pano/opencv-4.10.0/lib/cmake/opencv4
    export PKG_CONFIG_PATH=/opt/pano/opencv-4.10.0/lib/pkgconfig
    export LD_LIBRARY_PATH=/opt/pano/opencv-4.10.0/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    [[ "$(pkg-config --modversion opencv4)" == 4.10.0 ]] || die '配套 OpenCV 4.10.0 未正确安装'
fi

if [[ -n "$local_source" && "$local_source" != remote ]]; then
    printf '使用本地源码: %s\n' "$local_source"
    mkdir "$work/source"
    # Copy only build inputs; never reuse host binaries or copy credentials/.git.
    for item in Makefile include src modules models config; do cp -a "$local_source/$item" "$work/source/"; done
    source_commit=$(git -C "$local_source" rev-parse HEAD 2>/dev/null || printf 'local-source')
else
    printf '拉取 pano_example (%s)...\n' "$example_ref"
    git clone --depth 1 --branch "$example_ref" -- https://github.com/BuerH/pano_example.git "$work/source"
    source_commit=$(git -C "$work/source" rev-parse HEAD)
fi
make -C "$work/source" -j"$jobs" PANORAMA_PREFIX=/usr PANORAMA_LIBDIR="/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
"$work/source/panorama_pipeline" --help
ldd "$work/source/lib/display.so" > "$work/linked-libraries.txt"
if grep -q 'not found' "$work/linked-libraries.txt"; then
    cat "$work/linked-libraries.txt" >&2
    die '应用存在未找到的动态库'
fi
mkdir -p -- "$target"
target=$(cd -- "$target" && pwd -P)
mkdir -p "$target/releases" "$target/camera-cache"
release=$(mktemp -d "$target/releases/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
mkdir -p "$release/bin"
install -m 755 "$work/source/panorama_pipeline" "$release/bin/panorama_pipeline"
cp -a "$work/source/lib" "$work/source/models" "$release/"
cp "$work/releases.json" "$work/linked-libraries.txt" "$release/"
printf '%s\n' "$source_commit" > "$release/example-commit.txt"
printf '%s\n' "$arch" > "$release/architecture.txt"
mv "$work/source" "$release/source"
for item in lib models license.lic; do
    case "$item" in
        lib|models) destination="../current/$item" ;;
        license.lic) destination=../license.lic ;;
    esac
    link="$target/camera-cache/$item"
    if [[ -e "$link" || -L "$link" ]]; then
        [[ -L "$link" && "$(readlink "$link")" == "$destination" ]] || die "缓存目录存在冲突文件: $link"
    else
        ln -s "$destination" "$link"
    fi
done
# Each switch is atomic; keep previous releases available for troubleshooting.
ln -s "releases/$(basename "$release")" "$target/.current-$$"
mv -Tf "$target/.current-$$" "$target/current"
cat > "$target/pano" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail
app=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
if [[ "$(cat "$app/current/architecture.txt")" == arm64 ]]; then
    export LD_LIBRARY_PATH=/opt/pano/opencv-4.10.0/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
fi
case "${1:-}" in
    --machine-id) exec panorama-machine-id ;;
    --help|-h) exec "$app/current/bin/panorama_pipeline" --help ;;
esac
if [[ ! -r "$app/license.lic" ]]; then
    printf '请将设备授权文件放到 %s/license.lic\n机器码:\n' "$app" >&2
    panorama-machine-id >&2
    printf '申请地址: https://wudu.bitshe.cn/pano/activate.html\n' >&2
    exit 1
fi
cd "$app/camera-cache"
exec "$app/current/bin/panorama_pipeline" "$@"
LAUNCHER
chmod 755 "$target/pano"
printf '\n安装完成: %s\n启动程序: %s/pano\n授权文件: %s/license.lic\n相机缓存: %s/camera-cache/\n' "$target" "$target" "$target" "$target"
printf '机器码: '
panorama-machine-id
printf '\n需要桌面显示和四路相机 /dev/video0、2、4、6 的读写权限。\n按 q + 回车退出；安装脚本不会自动启动相机。\n'
