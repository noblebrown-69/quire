#!/bin/bash
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

echo "=== Quire AppImage Builder ==="
echo "Using existing Release binary (will not rebuild)."

BIN="build/Quire"
if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN is missing or not executable. Build it first; this script does not call rebuild.sh."
    exit 1
fi

if [ ! -f quire.png ]; then
    if [ -f icons/monastery.png ]; then
        cp -a icons/monastery.png quire.png
    else
        echo "ERROR: quire.png is missing and icons/monastery.png was not found."
        exit 1
    fi
fi

PACK_DESKTOP="Quire-appimage.desktop"
if [ ! -f "$PACK_DESKTOP" ]; then
    echo "ERROR: $PACK_DESKTOP is missing."
    exit 1
fi

MONASTERY_DIR="$(dirname "$PWD")/Monastery"
LINUXDEPLOY="./linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT="./linuxdeploy-plugin-qt-x86_64.AppImage"
if [ ! -f "$LINUXDEPLOY" ]; then
    LINUXDEPLOY="$MONASTERY_DIR/linuxdeploy-x86_64.AppImage"
fi
if [ ! -f "$LINUXDEPLOY_QT" ]; then
    LINUXDEPLOY_QT="$MONASTERY_DIR/linuxdeploy-plugin-qt-x86_64.AppImage"
fi
if [ ! -f "$LINUXDEPLOY" ]; then
    echo "ERROR: linuxdeploy-x86_64.AppImage is missing."
    exit 1
fi
if [ ! -f "$LINUXDEPLOY_QT" ]; then
    echo "ERROR: linuxdeploy-plugin-qt-x86_64.AppImage is missing."
    exit 1
fi
chmod +x "$LINUXDEPLOY" "$LINUXDEPLOY_QT"

export QMAKE=/usr/bin/qmake6
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$HOME/.local/opt/qt6-webengine/usr/lib/qt6/libexec:$(dirname "$LINUXDEPLOY"):$PWD:$PATH"
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu/qt6${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Encourage linuxdeploy-plugin-qt to pull extra WebEngine-related plugins
export EXTRA_QT_PLUGINS="${EXTRA_QT_PLUGINS:-webview;qmltooling;position}"

HUNSPELL_LIB="/usr/lib/x86_64-linux-gnu/libhunspell-1.7.so.0"
if [ ! -e "$HUNSPELL_LIB" ]; then
    echo "ERROR: $HUNSPELL_LIB not found."
    exit 1
fi

if [ ! -f /usr/share/hunspell/en_US.aff ] || [ ! -f /usr/share/hunspell/en_US.dic ]; then
    echo "Hunspell en_US dictionaries missing; installing hunspell-en-us..."
    sudo -n apt-get update -qq
    sudo -n apt-get install -y --no-install-recommends hunspell-en-us
fi

echo "Cleaning previous AppDir..."
rm -rf AppDir

run_linuxdeploy() {
    local -a cmd
    cmd=("$LINUXDEPLOY")
    if [ "${1:-}" = "extract" ]; then
        cmd+=(--appimage-extract-and-run)
    fi
    cmd+=(--appdir AppDir
          --plugin qt
          --executable "$BIN"
          --icon-file quire.png
          --desktop-file "$PACK_DESKTOP"
          --library "$HUNSPELL_LIB"
          --output appimage)
    echo "Running: ${cmd[*]}"
    "${cmd[@]}"
}

echo "Bundling Qt application + Hunspell into AppImage..."
if ! run_linuxdeploy; then
    echo "linuxdeploy failed; retrying with --appimage-extract-and-run..."
    run_linuxdeploy extract
fi

copy_hunspell() {
    mkdir -p AppDir/usr/share/hunspell
    if [ -f /usr/share/hunspell/en_US.aff ]; then
        cp -a /usr/share/hunspell/en_US.aff AppDir/usr/share/hunspell/
    fi
    if [ -f /usr/share/hunspell/en_US.dic ]; then
        cp -a /usr/share/hunspell/en_US.dic AppDir/usr/share/hunspell/
    fi
}

copy_webengine() {
    echo "Copying Qt WebEngine process, resources, locales, and libraries into AppDir..."
    local we_prefix
    if [ -x "${HOME}/.local/opt/qt6-webengine/usr/lib/qt6/libexec/QtWebEngineProcess" ]; then
        we_prefix="${HOME}/.local/opt/qt6-webengine/usr"
    else
        we_prefix="/usr"
    fi
    echo "WE_PREFIX=${we_prefix}"

    mkdir -p AppDir/usr/lib/qt6/libexec
    mkdir -p AppDir/usr/libexec
    mkdir -p AppDir/usr/bin
    mkdir -p AppDir/usr/share/qt6/resources
    mkdir -p AppDir/usr/share/qt6/translations/qtwebengine_locales
    mkdir -p AppDir/usr/lib
    mkdir -p AppDir/usr/lib/x86_64-linux-gnu
    mkdir -p AppDir/usr/lib/qt6/resources
    mkdir -p AppDir/usr/resources
    mkdir -p AppDir/usr/translations/qtwebengine_locales

    local we_proc="${we_prefix}/lib/qt6/libexec/QtWebEngineProcess"
    if [ -x "${we_proc}" ]; then
        cp -a "${we_proc}" AppDir/usr/lib/qt6/libexec/
        cp -a "${we_proc}" AppDir/usr/libexec/
        cp -a "${we_proc}" AppDir/usr/bin/
    else
        echo "WARNING: QtWebEngineProcess not found at ${we_proc}"
    fi

    local we_res="${we_prefix}/share/qt6/resources"
    if [ -d "${we_res}" ]; then
        cp -a "${we_res}"/. AppDir/usr/share/qt6/resources/
        cp -a "${we_res}"/. AppDir/usr/lib/qt6/resources/
        cp -a "${we_res}"/. AppDir/usr/resources/
    else
        echo "WARNING: WebEngine resources not found at ${we_res}"
    fi

    local we_loc="${we_prefix}/share/qt6/translations/qtwebengine_locales"
    if [ -d "${we_loc}" ]; then
        cp -a "${we_loc}"/. AppDir/usr/share/qt6/translations/qtwebengine_locales/
        cp -a "${we_loc}"/. AppDir/usr/translations/qtwebengine_locales/
    fi

    local libdir lib
    local -a we_libs=(
        libQt6WebEngineWidgets.so.6
        libQt6WebEngineCore.so.6
        libQt6QuickWidgets.so.6
        libQt6Quick.so.6
        libQt6WebChannel.so.6
        libQt6Qml.so.6
        libQt6QmlModels.so.6
        libQt6Positioning.so.6
        libQt6PrintSupport.so.6
        libQt6OpenGL.so.6
        libQt6Network.so.6
    )
    for libdir in "${we_prefix}/lib/x86_64-linux-gnu" "/usr/lib/x86_64-linux-gnu"; do
        [ -d "${libdir}" ] || continue
        for lib in "${we_libs[@]}"; do
            if [ -e "${libdir}/${lib}" ]; then
                cp -a "${libdir}/${lib}"* AppDir/usr/lib/ 2>/dev/null || true
                if [ -d AppDir/usr/lib/x86_64-linux-gnu ]; then
                    cp -a "${libdir}/${lib}"* AppDir/usr/lib/x86_64-linux-gnu/ 2>/dev/null || true
                fi
            fi
        done
    done
}

has_webengine() {
    local proc pak
    proc="$(find AppDir -name QtWebEngineProcess -type f 2>/dev/null | head -n 1 || true)"
    pak="$(find AppDir -name qtwebengine_resources.pak -type f 2>/dev/null | head -n 1 || true)"
    [ -n "$proc" ] && [ -n "$pak" ]
}

copy_hunspell

if ! has_webengine; then
    echo "WARNING: QtWebEngineProcess or qtwebengine_resources.pak missing from AppDir."
    copy_webengine
    echo "Re-running linuxdeploy so the AppImage includes WebEngine files..."
    if ! run_linuxdeploy; then
        echo "linuxdeploy failed; retrying with --appimage-extract-and-run..."
        run_linuxdeploy extract
    fi
    copy_hunspell
    if ! has_webengine; then
        echo "WebEngine files still missing after re-run; copying again and rebuilding AppImage..."
        copy_webengine
        copy_hunspell
        if ! run_linuxdeploy; then
            run_linuxdeploy extract
        fi
        copy_hunspell
    fi
fi

# After linuxdeploy (which creates AppRun as a symlink to the binary), replace
# it with a real script that sets QTWEBENGINE_* from $APPDIR, then re-squash
# with appimagetool so linuxdeploy cannot recreate the symlink.
install_qtwebengine_apprun() {
    local bin_name="$1"
    rm -f AppDir/AppRun
    cat > AppDir/AppRun <<APPRUN_EOF
#!/bin/bash
HERE="\$(dirname "\$(readlink -f "\$0")")"
export APPDIR="\$HERE"
export LD_LIBRARY_PATH="\$HERE/usr/lib:\$HERE/usr/lib/x86_64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="\$HERE/usr/plugins\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}"
if [ -x "\$HERE/usr/lib/qt6/libexec/QtWebEngineProcess" ]; then
    export QTWEBENGINEPROCESS_PATH="\$HERE/usr/lib/qt6/libexec/QtWebEngineProcess"
elif [ -x "\$HERE/usr/bin/QtWebEngineProcess" ]; then
    export QTWEBENGINEPROCESS_PATH="\$HERE/usr/bin/QtWebEngineProcess"
fi
if [ -d "\$HERE/usr/share/qt6/resources" ]; then
    export QTWEBENGINE_RESOURCES_PATH="\$HERE/usr/share/qt6/resources"
elif [ -d "\$HERE/usr/lib/qt6/resources" ]; then
    export QTWEBENGINE_RESOURCES_PATH="\$HERE/usr/lib/qt6/resources"
elif [ -d "\$HERE/usr/resources" ]; then
    export QTWEBENGINE_RESOURCES_PATH="\$HERE/usr/resources"
fi
export QTWEBENGINE_LOCALES_PATH="\$HERE/usr/share/qt6/translations/qtwebengine_locales"
export QTWEBENGINE_DISABLE_SANDBOX=1
exec "\$HERE/usr/bin/${bin_name}" "\$@"
APPRUN_EOF
    chmod +x AppDir/AppRun
}

find_appimagetool() {
    local candidate
    local -a candidates=(
        "/tmp/appimage_extracted_c13a8b6f65a3ddf1cad26656f509034a/plugins/linuxdeploy-plugin-appimage/appimagetool-prefix/usr/bin/appimagetool"
    )
    shopt -s nullglob
    candidates+=(/tmp/appimage_extracted_*/plugins/linuxdeploy-plugin-appimage/appimagetool-prefix/usr/bin/appimagetool)
    shopt -u nullglob
    for candidate in "${candidates[@]}"; do
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

resquash_appimage() {
    local out="$1"
    local bin_name="$2"
    local tool=""
    local extract_dir=""

    install_qtwebengine_apprun "$bin_name"

    if ! tool="$(find_appimagetool)"; then
        echo "appimagetool not found; extracting linuxdeploy to locate it..."
        extract_dir="$(mktemp -d /tmp/linuxdeploy-extract-XXXXXX)"
        (
            cd "$extract_dir"
            APPIMAGE_EXTRACT_AND_RUN=1 "$LINUXDEPLOY" --appimage-extract >/dev/null
        ) || true
        tool="$(find "$extract_dir" -name appimagetool -type f 2>/dev/null | head -n 1 || true)"
        if [ -z "$tool" ] || [ ! -x "$tool" ]; then
            echo "appimagetool still missing; running linuxdeploy --output appimage after AppRun replace..."
            if ! run_linuxdeploy; then
                run_linuxdeploy extract
            fi
            copy_hunspell
            if [ -L AppDir/AppRun ]; then
                echo "linuxdeploy recreated AppRun symlink; restoring script..."
                install_qtwebengine_apprun "$bin_name"
            fi
            tool="$(find_appimagetool || true)"
            if [ -z "$tool" ] || [ ! -x "$tool" ]; then
                echo "ERROR: could not locate appimagetool after linuxdeploy."
                exit 1
            fi
        fi
    fi

    echo "Rebuilding AppImage with appimagetool: $tool"
    ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream AppDir "$out"

    if [ -L AppDir/AppRun ]; then
        echo "WARNING: AppRun became a symlink during squash; restoring and retrying..."
        install_qtwebengine_apprun "$bin_name"
        ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream AppDir "$out"
    fi

    if [ -L AppDir/AppRun ]; then
        echo "ERROR: AppDir/AppRun is still a symlink."
        exit 1
    fi
    if ! grep -q QTWEBENGINEPROCESS_PATH AppDir/AppRun; then
        echo "ERROR: AppDir/AppRun missing QTWEBENGINEPROCESS_PATH."
        exit 1
    fi
}

# Always copy WebEngine bits into AppDir before the final resquash so the
# shipped image is not the incomplete linuxdeploy first-pass (~102-118MB).
copy_webengine
copy_hunspell

# Rename linuxdeploy output (desktop Name=Quire)
if [ -f Quire-x86_64.AppImage ]; then
    mv -f Quire-x86_64.AppImage Quire.AppImage
elif ls Quire-*.AppImage >/dev/null 2>&1; then
    shopt -s nullglob
    for f in Quire-*.AppImage; do
        case "$f" in
            linuxdeploy*) continue ;;
        esac
        mv -f "$f" Quire.AppImage
        break
    done
    shopt -u nullglob
fi

install_qtwebengine_apprun Quire
resquash_appimage Quire.AppImage Quire

if [ ! -f Quire.AppImage ]; then
    echo "ERROR: Quire.AppImage was not produced."
    ls -lh ./*.AppImage 2>/dev/null || true
    exit 1
fi

# Do not leave the incomplete linuxdeploy first-pass name around.
rm -f Quire-x86_64.AppImage

echo ""
echo "=== AppDir sanity ==="
echo -n "Quire binary: "
find AppDir -type f \( -path 'AppDir/usr/bin/Quire' -o -name Quire \) ! -name '*.desktop' | head -n 5
echo -n "libQt6WebEngineWidgets: "
find AppDir -name 'libQt6WebEngineWidgets.so*' | head -n 5
echo -n "QtWebEngineProcess: "
find AppDir -name QtWebEngineProcess
echo -n "qtwebengine_resources.pak: "
find AppDir -name qtwebengine_resources.pak
echo -n "AppRun: "
ls -l AppDir/AppRun
if [ -L AppDir/AppRun ]; then
    echo "ERROR: AppDir/AppRun is a symlink"
    exit 1
fi
if ! grep -q QTWEBENGINEPROCESS_PATH AppDir/AppRun; then
    echo "ERROR: AppDir/AppRun missing QTWEBENGINEPROCESS_PATH"
    exit 1
fi

echo ""
echo "✅ Quire.AppImage ready"
ls -lh Quire.AppImage
