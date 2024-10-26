#!/bin/bash

# Exit on any error
set -e

echo "=== Finding existing scrcpy files ==="
# Store the found files in an array
mapfile -t FILES < <(sudo find /usr/local -name "*scrcpy*")

# If files were found, ask for confirmation before removal
if [ ${#FILES[@]} -gt 0 ]; then
    echo "The following scrcpy files were found:"
    printf '%s\n' "${FILES[@]}"
    
    read -p "Do you want to remove these files? (y/n): " confirm
    if [[ "$confirm" == [yY] || "$confirm" == [yY][eE][sS] ]]; then
        echo "Removing files..."
        for file in "${FILES[@]}"; do
            sudo rm -rf "$file"
        done
        echo "Existing scrcpy files removed."
    else
        echo "Aborting file removal."
        exit 1
    fi
else
    echo "No existing scrcpy files found."
fi

echo "=== Preparing build environment ==="
# Create build directory if needed
if [ -d "x" ]; then
    echo "Removing existing build directory..."
    rm -rf x
fi

echo "=== Setting JAVA_HOME ==="
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
echo "JAVA_HOME set to: $JAVA_HOME"

echo "=== Setting Android SDK Path ==="
export ANDROID_SDK_ROOT=~/Android/Sdk
echo "ANDROID_SDK_ROOT set to: $ANDROID_SDK_ROOT"

echo "=== Configuring build ==="
meson setup x --buildtype=release --strip -Db_lto=true

echo "=== Building scrcpy ==="
ninja -Cx

echo "=== Installing scrcpy ==="
sudo ninja -Cx install

echo "=== Installation complete ==="
echo "Testing scrcpy installation..."
which scrcpy
echo "You can now run 'scrcpy' to start the application."