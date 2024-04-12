#!/bin/bash

curl https://sh.rustup.rs -sSf | sh -s -- -y --default-toolchain 1.76.0
source $HOME/.cargo/env
rustup install 1.76.0
rustup default 1.76.0
cargo install --force cbindgen

cd /tmp/
wget https://cmake.org/files/v3.12/cmake-3.12.4.tar.gz
tar -xvf cmake-3.12.4.tar.gz
cd cmake-3.12.4
./bootstrap
make
make install
cd ..
rm -rf cmake-3.12.4*