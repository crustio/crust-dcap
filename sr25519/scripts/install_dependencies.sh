#!/bin/bash

curl https://sh.rustup.rs -sSf | sh -s -- -y --default-toolchain 1.76.0
source $HOME/.cargo/env
rustup install 1.76.0
rustup default 1.76.0
cargo install --force cbindgen
