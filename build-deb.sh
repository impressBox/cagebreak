set -e

echo "Building Cagebreak .deb package..."

echo "Checking for build dependencies..."
sudo apt-get update
sudo apt-get install -y \
    debhelper \
    meson \
    pkg-config \
    libwayland-dev \
    wayland-protocols \
    libwlroots-dev \
    libxkbcommon-dev \
    libcairo2-dev \
    libpango1.0-dev \
    libfontconfig-dev \
    libinput-dev \
    libevdev-dev \
    libudev-dev \
    scdoc \
    devscripts \
    build-essential

echo "Building Debian package..."
debuild -us -uc -b

echo "Package built successfully!"
echo "The .deb file should be available in the parent directory."
echo "You can install it with: sudo dpkg -i ../cagebreak_2.3.0-1_*.deb"
echo "If there are dependency issues, run: sudo apt-get install -f"
