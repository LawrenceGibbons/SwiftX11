#!/bin/bash
#
# build-dmg.sh — Build SwiftX11 release .dmg for distribution
#
# Usage: bash macos/scripts/build-dmg.sh
#
# Produces a .dmg with:
#   - SwiftX11.app (drag to Applications)
#   - X11 Fonts folder (optional manual install)
#   - README.txt with quick-start instructions
#
# No code signing required. Recipients: right-click → Open on first launch.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

# Read version from SwiftX11Version.h
VERSION=$(grep '#define SWIFTX11_VERSION_BASE' "$PROJECT_DIR/X11LowLevel/include/SwiftX11Version.h" \
    | sed 's/.*"\(.*\)"/\1/')

if [ -z "$VERSION" ]; then
    echo "ERROR: Could not read version from SwiftX11Version.h"
    exit 1
fi

DMG_NAME="SwiftX11-${VERSION}"
DMG_DIR="$BUILD_DIR/dmg-staging"
OUTPUT_DMG="$BUILD_DIR/${DMG_NAME}.dmg"

echo "=== Building SwiftX11 v${VERSION} DMG ==="
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

# ── Step 2: Stage DMG contents ───────────────────────────────────────────

echo "── Step 2: Staging DMG contents..."
rm -rf "$DMG_DIR"
mkdir -p "$DMG_DIR"

# Copy app and strip quarantine/resource forks
cp -R "$APP_PATH" "$DMG_DIR/"
xattr -cr "$DMG_DIR/SwiftX11.app"
dot_clean -m "$DMG_DIR" 2>/dev/null || true
echo "   SwiftX11.app: $(du -sh "$DMG_DIR/SwiftX11.app" | cut -f1)"

# Applications symlink for drag-install
ln -s /Applications "$DMG_DIR/Applications"

# Bundle X11 fonts if available
FONT_SRC="/opt/X11/share/fonts"
if [ -d "$FONT_SRC/misc" ]; then
    mkdir -p "$DMG_DIR/X11 Fonts"
    for dir in misc 75dpi 100dpi; do
        if [ -d "$FONT_SRC/$dir" ]; then
            cp -R "$FONT_SRC/$dir" "$DMG_DIR/X11 Fonts/"
        fi
    done
    dot_clean -m "$DMG_DIR/X11 Fonts" 2>/dev/null || true
    echo "   X11 Fonts: $(du -sh "$DMG_DIR/X11 Fonts" | cut -f1)"

    # Font install instructions
    cat > "$DMG_DIR/X11 Fonts/INSTALL.txt" << 'FONTEOF'
X11 Fonts — Installation Instructions

These PCF bitmap fonts are needed by legacy X11 apps (xterm, xcalc, xclock).
If you already have XQuartz installed, you don't need these.

To install, run in Terminal:

    sudo mkdir -p /opt/X11/share/fonts
    sudo cp -R misc 75dpi 100dpi /opt/X11/share/fonts/

Then restart SwiftX11.
FONTEOF
fi

# README
cat > "$DMG_DIR/README.txt" << READMEEOF
SwiftX11 v${VERSION} — X11 Server for macOS
============================================

INSTALLATION
  Drag SwiftX11.app to the Applications folder.

FIRST LAUNCH
  Right-click SwiftX11.app → Open (required once for unsigned apps).

QUICK START
  1. Launch SwiftX11 from Applications.
  2. Add to ~/.zprofile:  export DISPLAY=127.0.0.1:1
  3. Run X11 apps:  xterm -sb -rightbar -bc
  4. Docker:  DISPLAY=host.docker.internal:1

X11 FONTS
  If xterm shows blank/missing text, install the fonts from the
  "X11 Fonts" folder (see INSTALL.txt inside). Not needed if
  XQuartz is installed.

For full documentation: Help → SwiftX11 Help (in the app).
READMEEOF

echo ""

# ── Step 3: Create DMG ───────────────────────────────────────────────────

echo "── Step 3: Creating DMG..."

# Create temporary DMG (read-write)
TEMP_DMG="$BUILD_DIR/${DMG_NAME}-temp.dmg"
hdiutil create -volname "$DMG_NAME" \
    -srcfolder "$DMG_DIR" \
    -ov -format UDRW \
    "$TEMP_DMG" >/dev/null 2>&1

# Convert to compressed read-only DMG
rm -f "$OUTPUT_DMG"
hdiutil convert "$TEMP_DMG" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -o "$OUTPUT_DMG" >/dev/null 2>&1
rm -f "$TEMP_DMG"

echo ""
echo "=== Done ==="
echo "DMG:  $OUTPUT_DMG"
echo "Size: $(du -sh "$OUTPUT_DMG" | cut -f1)"
echo ""
echo "To distribute: share the .dmg file."
echo "Recipients: double-click to mount, drag SwiftX11 to Applications,"
echo "            then right-click → Open on first launch."
