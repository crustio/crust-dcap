#!/bin/bash

current_dir=$(cd `dirname $0`;pwd)
sr25519_dir=$(cd $current_dir/..;pwd)
project_root_dir=$(cd $sr25519_dir/..;pwd)

cd $sr25519_dir

mkdir -p build

cd build

cmake .. -DCMAKE_BUILD_TYPE=Release

make install

if [ $? -ne 0 ]; then
    echo "Build Failed!!!"
    exit 1
else
    echo "Build Success!!!"
    cp -f /usr/local/include/sr25519/sr25519.h $project_root_dir/src/include/
    cp -f /usr/local/lib/libsr25519crust.a $project_root_dir/src/lib/
fi