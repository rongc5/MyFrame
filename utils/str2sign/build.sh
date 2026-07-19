#/bin/sh

# Lightweight build script for str2sign static library

rm -rf lib include
mkdir -p lib include

mk=core.mk

path=`pwd`
base_file=`basename $path`

has_test_mk=false
if [ -f test/Makefile ] || [ -f test/makefile ] || [ -f test/GNUmakefile ]; then
    has_test_mk=true
fi

if [ $# == 0 ]; then
    make -f $mk clean && make -j8 -f $mk
    cp core/lib*.a lib
    cp core/*.h include
elif [ $# == 1 ];then
    if [ $1 == "clean" ];then
        make -f $mk clean || true
        rm -rf lib include
        if [ "$has_test_mk" = true ]; then
            (cd test && make clean) || true
        fi
    elif [ $1 == "all" ]; then
        make -f $mk clean && make -j8 -f $mk
        cp core/lib*.a lib
        cp core/*.h include
        if [ "$has_test_mk" = true ]; then
            (cd test && make clean && make) || true
        fi
    fi
fi
