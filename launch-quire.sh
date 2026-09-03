#!/bin/bash
# Double-click this (or run it from a terminal). The raw build/Quire binary
# needs the extracted Qt WebEngine prefix on this machine.
cd "$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HOME/.local/opt/qt6-webengine/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH}"
export QTWEBENGINEPROCESS_PATH="$HOME/.local/opt/qt6-webengine/usr/lib/qt6/libexec/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="$HOME/.local/opt/qt6-webengine/usr/share/qt6/resources"
export QTWEBENGINE_LOCALES_PATH="$HOME/.local/opt/qt6-webengine/usr/share/qt6/translations/qtwebengine_locales"
export QTWEBENGINE_DISABLE_SANDBOX=1
exec ./build/Quire "$@"
