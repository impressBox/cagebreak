# Building Debian Packages for Cagebreak

This document explains how to build Debian (.deb) packages for the Cagebreak Wayland compositor.

## Prerequisites

You need the following packages installed on your Debian/Ubuntu system:

```bash
sudo apt-get install debhelper meson pkg-config libwayland-dev wayland-protocols \
    libwlroots-dev libxkbcommon-dev libcairo2-dev libpango1.0-dev libfontconfig-dev \
    libinput-dev libevdev-dev libudev-dev scdoc devscripts build-essential
```

## Building the Package

1. Clone the repository:
   ```bash
   git clone https://github.com/impressBox/cagebreak.git
   cd cagebreak
   ```

2. Run the build script:
   ```bash
   ./build-deb.sh
   ```

   This script will:
   - Install necessary build dependencies
   - Build the Cagebreak package
   - Create a .deb file in the parent directory

3. Install the package:
   ```bash
   sudo dpkg -i ../cagebreak_2.3.0-1_*.deb
   ```

   If there are dependency issues, run:
   ```bash
   sudo apt-get install -f
   ```

## Manual Package Building

If you prefer to build the package manually:

1. Install build dependencies:
   ```bash
   sudo apt-get install debhelper meson pkg-config libwayland-dev wayland-protocols \
       libwlroots-dev libxkbcommon-dev libcairo2-dev libpango1.0-dev libfontconfig-dev \
       libinput-dev libevdev-dev libudev-dev scdoc devscripts build-essential
   ```

2. Build the package:
   ```bash
   debuild -us -uc -b
   ```

3. Install the package:
   ```bash
   sudo dpkg -i ../cagebreak_2.3.0-1_*.deb
   sudo apt-get install -f  # To resolve any dependencies
   ```

## Package Contents

The Debian package includes:
- The Cagebreak binary
- Configuration files
- Man pages
- License information

## Troubleshooting

### Dependency Issues

If you encounter dependency issues during installation, you can resolve them with:
```bash
sudo apt-get install -f
```

### Version Mismatch

The package is built for wlroots 0.17.x. If your system has a different version, you may need to adjust the dependencies in `debian/control`.

### Build Failures

If the build fails, check the build log for specific errors. Common issues include:
- Missing build dependencies
- Version mismatches in dependencies
- Incompatible compiler versions

## Notes for Packagers

- The package follows Debian packaging guidelines
- The package is configured to build with XWayland support
- Man pages are included in the package
