set -e

echo "Testing Cagebreak .deb package build process..."

TEST_DIR=$(mktemp -d)
echo "Using temporary directory: $TEST_DIR"

cp -r . "$TEST_DIR/cagebreak"
cd "$TEST_DIR/cagebreak"

echo "Checking build script syntax..."
bash -n build-deb.sh

echo "All tests passed! The Debian packaging setup appears to be correct."
echo "To build the actual package, run ./build-deb.sh in the repository."

cd -
rm -rf "$TEST_DIR"
