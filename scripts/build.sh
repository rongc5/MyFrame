#!/bin/bash

set -euo pipefail
shopt -s nullglob

# 确保项目根目录正确
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

echo "Working in project root: $PROJECT_ROOT"

# 需要编译的库（有静态库输出）
build_libs=('utils/str2sign' 'utils/encode')

# 仅头文件的库
header_only_libs=('utils/btree' 'utils/rapidjson' 'utils/ini')

rm -rf lib include
mkdir -p lib include/{core,encode,str2sign,btree,rapidjson,ini}

if [ $# == 0 ]; then
    echo "Building all utility components..."
    
    # 构建需要编译的库
    echo "=== Building static libraries ==="
    i=0
    while [ $i -lt ${#build_libs[@]} ]
    do
        echo "Building ${build_libs[$i]}..."
        cd "${build_libs[$i]}" && ./build.sh all
        
        # 复制静态库
        if [ -d "lib" ]; then
            cp lib/lib*.a ../../lib/
            echo "Copied static library from ${build_libs[$i]}/lib/"
        fi
        
        # 复制头文件到对应的子目录
        project_name=$(basename "$(pwd)")
        echo "Copying headers for ${project_name}..."
        
        if [ -d "core" ]; then
            find core -name "*.h" -exec cp {} "../../include/${project_name}/" \; 2>/dev/null || true
            echo "Copied core headers to include/${project_name}/"
        fi
        
        if [ -d "include" ]; then
            find include -name "*.h" -exec cp {} "../../include/${project_name}/" \; 2>/dev/null || true
            echo "Copied include headers to include/${project_name}/"
        fi
        
        let i+=1
        cd "$PROJECT_ROOT"
    done
    
    # 复制主项目头文件到core子目录
    echo "=== Copying main project headers ==="
    cp core/*.h include/core/
    echo "Copied main project headers to include/core/"
    
    # 复制仅头文件的库
    echo "=== Copying header-only libraries ==="
    
    # btree headers
    echo "Copying btree headers..."
    cp -r utils/btree/*.h include/btree/ 2>/dev/null || true
    cp -r utils/btree/btree* include/btree/ 2>/dev/null || true
    echo "Copied btree headers to include/btree/"
    
    # rapidjson headers (保持目录结构)
    echo "Copying rapidjson headers..."
    cp -r utils/rapidjson/* include/rapidjson/ 2>/dev/null || true
    echo "Copied rapidjson headers to include/rapidjson/"
    
    # ini headers  
    echo "Copying ini headers..."
    cp -r utils/ini/*.h include/ini/ 2>/dev/null || true
    echo "Copied ini headers to include/ini/"
    
    echo ""
    echo "=== Build Summary ==="
    echo "静态库 (lib/):"
    ls -la lib/*.a 2>/dev/null || echo "  (no static libraries found)"
    echo ""
    echo "头文件目录 (include/):"
    ls -d include/*/ 2>/dev/null || echo "  (no include directories found)"
    echo ""
    echo "✅ All utility libraries built successfully!"
    echo "📦 Use 'scripts/build_cmake.sh' for full project build with CMake."
elif [ $# == 1 ];then
    if [ $1 == "clean" ];then
        echo "Cleaning all utility components..."
        rm -rf lib include
        
        # 清理需要编译的库
        i=0
        while [ $i -lt ${#build_libs[@]} ]
        do
            echo "Cleaning ${build_libs[$i]}..."
            cd "${build_libs[$i]}" && ./build.sh clean
            let i+=1
            cd "$PROJECT_ROOT"
        done
        
        echo "✅ Clean completed!"
    fi
fi
