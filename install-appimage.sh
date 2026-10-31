#!/usr/bin/env bash
# Install a built Quire AppImage into ~/Dropbox/Quire/ safely.
# - Backups go to ~/Dropbox/Development/_backups/appimages/quire/ (never the app folder).
# - The new image is staged OUTSIDE Dropbox, then atomically renamed into place,
#   so Dropbox never syncs a half-written file and a running copy is not truncated.
# Usage: ./install-appimage.sh [path/to/Quire.AppImage] [--force]
set -euo pipefail
SRC="${1:-$(dirname "$0")/Quire.AppImage}"
FORCE="${2:-}"
DEST_DIR="$HOME/Dropbox/Quire"
DEST="$DEST_DIR/Quire.AppImage"
BK="$HOME/Dropbox/Development/_backups/appimages/quire"
STAGE="$HOME/.cache/appimage-staging"
[ -f "$SRC" ] || { echo "no such AppImage: $SRC" >&2; exit 1; }
if [ "$FORCE" != "--force" ] && pgrep -f "mount_Quire|/Quire.AppImage|appimage_extracted_.*/usr/bin/Quire" >/dev/null; then
  echo "Quire is running; close it first (or pass --force)." >&2; exit 2
fi
mkdir -p "$BK" "$STAGE" "$DEST_DIR"
tmp="$STAGE/Quire.AppImage.$$.new"
cp -f "$SRC" "$tmp"; chmod +x "$tmp"; sync -f "$tmp" 2>/dev/null || sync
if [ -f "$DEST" ]; then
  cp -p "$DEST" "$BK/Quire.AppImage.pre-$(date +%Y%m%d-%H%M%S)"
fi
mv -f "$tmp" "$DEST"   # same filesystem -> atomic rename
sha256sum "$DEST"
