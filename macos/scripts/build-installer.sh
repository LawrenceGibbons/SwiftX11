#!/bin/bash
#
# build-installer.sh — Build SwiftX11 release .pkg installer
#
# Usage: bash macos/scripts/build-installer.sh
#
# Prerequisites:
#   - Xcode command line tools
#   - X11 fonts at /opt/X11/share/fonts/ (from XQuartz) for bundling
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
INSTALLER_DIR="$SCRIPT_DIR/installer"

# Read version from SwiftX11Version.h
# SWIFTX11_VERSION_BASE always has the quoted base version string.
VERSION=$(grep '#define SWIFTX11_VERSION_BASE' "$PROJECT_DIR/X11LowLevel/include/SwiftX11Version.h" \
    | sed 's/.*"\(.*\)"/\1/')

if [ -z "$VERSION" ]; then
    echo "ERROR: Could not read version from SwiftX11Version.h"
    exit 1
fi

echo "=== Building SwiftX11 v${VERSION} installer ==="
echo ""

# Clean previous build
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# ── Step 1: Build Release ──────────────────────────────────────────────────

echo "── Step 1: Building Release configuration..."
xcodebuild -project "$PROJECT_DIR/SwiftX11.xcodeproj" \
    -scheme SwiftX11 \
    -configuration Release \
    -derivedDataPath "$BUILD_DIR/DerivedData" \
    build 2>&1 | tail -5

APP_PATH="$BUILD_DIR/DerivedData/Build/Products/Release/SwiftX11.app"
if [ ! -d "$APP_PATH" ]; then
    echo "ERROR: Build failed — SwiftX11.app not found at $APP_PATH"
    exit 1
fi
echo "   Built: $APP_PATH"
echo ""

# ── Step 2: Stage payloads ─────────────────────────────────────────────────

echo "── Step 2: Staging payloads..."

# App payload
mkdir -p "$BUILD_DIR/app-payload"
cp -R "$APP_PATH" "$BUILD_DIR/app-payload/"
echo "   App payload: $(du -sh "$BUILD_DIR/app-payload" | cut -f1)"

# Font payload
FONT_SRC="/opt/X11/share/fonts"
if [ ! -d "$FONT_SRC/misc" ]; then
    echo "WARNING: X11 fonts not found at $FONT_SRC"
    echo "   Font component will be skipped. Install XQuartz to bundle fonts."
    INCLUDE_FONTS=false
else
    INCLUDE_FONTS=true
    mkdir -p "$BUILD_DIR/fonts-payload"
    for dir in misc 75dpi 100dpi; do
        if [ -d "$FONT_SRC/$dir" ]; then
            cp -R "$FONT_SRC/$dir" "$BUILD_DIR/fonts-payload/"
        fi
    done
    echo "   Font payload: $(du -sh "$BUILD_DIR/fonts-payload" | cut -f1)"
fi
echo ""

# ── Step 3: Build .pkg components ──────────────────────────────────────────

echo "── Step 3: Building .pkg components..."

# App component
pkgbuild --root "$BUILD_DIR/app-payload" \
    --identifier com.rlan.SwiftX11.app \
    --version "$VERSION" \
    --install-location /Applications \
    "$BUILD_DIR/SwiftX11-app.pkg" >/dev/null 2>&1
echo "   SwiftX11-app.pkg: $(du -sh "$BUILD_DIR/SwiftX11-app.pkg" | cut -f1)"

# Font component (if available)
if [ "$INCLUDE_FONTS" = true ]; then
    pkgbuild --root "$BUILD_DIR/fonts-payload" \
        --identifier com.rlan.SwiftX11.fonts \
        --version "1.0" \
        --install-location /opt/X11/share/fonts \
        "$BUILD_DIR/SwiftX11-fonts.pkg" >/dev/null 2>&1
    echo "   SwiftX11-fonts.pkg: $(du -sh "$BUILD_DIR/SwiftX11-fonts.pkg" | cut -f1)"
fi
echo ""

# ── Step 4: Build distribution .pkg ────────────────────────────────────────

echo "── Step 4: Building distribution installer..."

# Copy resources
mkdir -p "$BUILD_DIR/resources"
cp "$INSTALLER_DIR/welcome.html" "$BUILD_DIR/resources/"

# If no fonts, modify Distribution.xml to remove font choice
if [ "$INCLUDE_FONTS" = true ]; then
    cp "$INSTALLER_DIR/Distribution.xml" "$BUILD_DIR/Distribution.xml"
else
    # Strip font choice from Distribution.xml
    sed '/<line choice="fonts"/d; /<choice id="fonts"/,/<\/choice>/d; /fonts_default/,/<\/script>/d; /SwiftX11-fonts.pkg/d' \
        "$INSTALLER_DIR/Distribution.xml" > "$BUILD_DIR/Distribution.xml"
fi

OUTPUT_PKG="$BUILD_DIR/SwiftX11-${VERSION}.pkg"
productbuild --distribution "$BUILD_DIR/Distribution.xml" \
    --package-path "$BUILD_DIR/" \
    --resources "$BUILD_DIR/resources" \
    "$OUTPUT_PKG"

echo ""
echo "=== Done ==="
echo "Installer: $OUTPUT_PKG"
echo "Size:      $(du -sh "$OUTPUT_PKG" | cut -f1)"
echo ""
echo "To install: double-click the .pkg file"
echo "Note: unsigned — recipients must right-click → Open to bypass Gatekeeper"
