#!/bin/sh
# 编译并安装 libnwzh.so 到 ~/.local/lib/nwn-zh/
set -e
cd "$(dirname "$0")/.."

PREFIX="${PREFIX:-$HOME/.local/lib/nwn-zh}"

make
make install PREFIX="$PREFIX"

cat <<EOF

已安装到 $PREFIX/libnwzh.so

Steam 的启动选项填：
  env NWNZH_TOUTF8=1 LD_PRELOAD=\$HOME/.local/lib/nwn-zh/libnwzh.so %command%

命令行启动见 README「用法」一节。
EOF
