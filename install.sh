#!/bin/bash

# Configure
meson setup build --buildtype release --strip -Db_lto=true

# Build
ninja -C build

# Install (use sudo only for this step)
sudo ninja -C build install