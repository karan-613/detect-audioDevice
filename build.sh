#!/bin/bash

# 检查version.txt文件是否存在
if [ ! -f "version.txt" ]; then
    echo "1.0.0.0" > version.txt
fi

# 读取当前版本号
VERSION=$(cat version.txt)
IFS='.' read -ra VERSION_PARTS <<< "$VERSION"

# 增加最后一位版本号
VERSION_PARTS[3]=$((VERSION_PARTS[3] + 1))

# 组合新版本号
NEW_VERSION="${VERSION_PARTS[0]}.${VERSION_PARTS[1]}.${VERSION_PARTS[2]}.${VERSION_PARTS[3]}"

# 更新版本号文件
echo "$NEW_VERSION" > version.txt

# 创建并进入build目录
mkdir -p build
cd build

# 配置CMake并传入版本号
cmake -DPROJECT_VERSION="$NEW_VERSION" ..

# 获取系统架构
ARCH=$(uname -m)

# 编译项目
make -j$(nproc)

# 定义新的可执行文件名
EXEC_NAME="detect-AudioDevice-${NEW_VERSION}-${ARCH}"

# 重命名编译后的文件
mv send "${EXEC_NAME}"

cd ..

# 显示新版本号和输出信息
echo "构建完成，当前版本: $NEW_VERSION"
echo "系统架构: $ARCH"
echo "可执行文件已重命名为: ${EXEC_NAME}"