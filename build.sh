#!/bin/bash
set -e

# SqrtalkServer Linux 编译脚本

echo "=== 检查依赖 ==="
DEPS="cmake g++ libboost-all-dev libssl-dev nlohmann-json3-dev"
MISSING=""
for pkg in $DEPS; do
    dpkg -s "$pkg" &>/dev/null || MISSING="$MISSING $pkg"
done

if [ -n "$MISSING" ]; then
    echo "安装缺失依赖:$MISSING"
    sudo apt update
    sudo apt install -y $MISSING
fi

# 检查 websocketpp 头文件
if [ ! -f /usr/local/include/websocketpp/server.hpp ] && \
   [ ! -f /usr/include/websocketpp/server.hpp ]; then
    echo "=== 安装 websocketpp（头文件库） ==="
    TMPDIR=$(mktemp -d)
    git clone --depth=1 https://github.com/zaphoyd/websocketpp.git "$TMPDIR/websocketpp"
    sudo cp -r "$TMPDIR/websocketpp/websocketpp" /usr/local/include/
    rm -rf "$TMPDIR"
fi

echo "=== 编译 ==="
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

echo ""
echo "=== 编译完成 ==="
echo "运行: ./build/sqrtalk_server"
