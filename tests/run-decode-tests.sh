#!/bin/sh
# 解码器单元测试：编译 libnwzh.so + tools/test_decode，跑 11 个用例。
set -e
cd "$(dirname "$0")/.."

make libnwzh.so tools/test_decode
./tools/test_decode ./libnwzh.so
