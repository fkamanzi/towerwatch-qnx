#!/usr/bin/env bash
# Build and run towerwatch on QNX 8.0 from a browser-only Linux shell
# (Google Cloud Shell, Ubuntu 24.04), following QNX's documented headless
# installation: myQNX login with curl, unattended QNX Software Center install,
# qnxsoftwarecenter_clt for the licence and QNX SDP 8.0, QEMU for the target.
#
#   bash tools/qnx-cloudshell.sh setup     QEMU and helpers (no account needed)
#   bash tools/qnx-cloudshell.sh install   asks for your myQNX email and password once,
#                                          installs QNX Software Center and QNX SDP 8.0
#   bash tools/qnx-cloudshell.sh build     builds towerwatch, pwrsim, pwrctl with qcc
#   bash tools/qnx-cloudshell.sh status    what is installed so far
#
# The password is read without echo, kept only in this shell process and never
# written to disk. Large files go to /tmp (Cloud Shell keeps $HOME small).
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

QSC_URL="https://www.qnx.com/download/download/79441/qnx-setup-2.0.4-202501021438-linux.run"
WORK=/tmp/qnx
QSC_DIR=$WORK/qnxinstall
SDP_DIR=$WORK/qnx800
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"

say() { printf '\n== %s\n' "$*"; }

setup() {
    say "Installing QEMU and helpers"
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq qemu-system-x86 qemu-utils mtools dosfstools >/dev/null
    mkdir -p "$WORK"
    # QNX tools keep their cache and licence data in ~/.qnx; put it on the larger disk.
    if [ ! -L "$HOME/.qnx" ]; then
        mkdir -p "$WORK/dot-qnx"
        [ -d "$HOME/.qnx" ] && cp -a "$HOME/.qnx/." "$WORK/dot-qnx/" && rm -rf "$HOME/.qnx"
        ln -s "$WORK/dot-qnx" "$HOME/.qnx"
    fi
    qemu-system-x86_64 --version | head -1
    df -h /tmp | tail -1
}

install_qnx() {
    mkdir -p "$WORK"
    [ -L "$HOME/.qnx" ] || setup
    read -r -p "myQNX email: " MYQNX_USER
    read -r -s -p "myQNX password (not shown): " MYQNX_PASSWORD
    echo
    local jar="$WORK/myqnxcookies.auth"

    say "Signing in to myQNX"
    curl -s --cookie-jar "$jar" --form "userlogin=$MYQNX_USER" --form "password=$MYQNX_PASSWORD" \
        -X POST https://www.qnx.com/account/login.html -o "$WORK/login_response.html"

    say "Downloading QNX Software Center"
    curl -s -L --cookie "$jar" "$QSC_URL" -o "$WORK/qnx-setup-lin.run"
    rm -f "$jar" "$WORK/login_response.html"
    if ! head -c 2 "$WORK/qnx-setup-lin.run" | grep -q '#!'; then
        echo "The download is not the installer (wrong email or password, or the licence is not deployed yet)."
        exit 1
    fi
    ls -l "$WORK/qnx-setup-lin.run"
    chmod a+x "$WORK/qnx-setup-lin.run"

    say "Installing QNX Software Center (unattended)"
    "$WORK/qnx-setup-lin.run" force-override disable-auto-start agree-to-license-terms "$QSC_DIR"
    local clt="$QSC_DIR/qnxsoftwarecenter/qnxsoftwarecenter_clt"

    say "Activating the licence deployed to your myQNX account"
    "$clt" -syncLicenseKeys -myqnx.user="$MYQNX_USER" -myqnx.password="$MYQNX_PASSWORD" -listLicenseKeys

    say "Installing QNX SDP 8.0 (several GB, takes a while)"
    "$clt" -mirror -myqnx.user="$MYQNX_USER" -myqnx.password="$MYQNX_PASSWORD"
    "$clt" -cleanInstall -destination "$SDP_DIR" -installBaseline com.qnx.qnx800 \
        -myqnx.user="$MYQNX_USER" -myqnx.password="$MYQNX_PASSWORD"
    unset MYQNX_PASSWORD
    ls "$SDP_DIR"
    say "QNX SDP 8.0 installed in $SDP_DIR"
}

build() {
    # shellcheck disable=SC1091
    source "$SDP_DIR/qnxsdp-env.sh"
    cd "$REPO_DIR"
    make qnx
    file build/qnx/gcc_ntox86_64/* || true
}

status() {
    echo "QEMU: $(command -v qemu-system-x86_64 || echo missing)"
    echo "QNX Software Center: $([ -x "$QSC_DIR/qnxsoftwarecenter/qnxsoftwarecenter_clt" ] && echo installed || echo missing)"
    echo "QNX SDP 8.0: $([ -f "$SDP_DIR/qnxsdp-env.sh" ] && echo installed || echo missing)"
    df -h /tmp | tail -1
}

case "${1:-status}" in
    setup) setup ;;
    install) install_qnx ;;
    build) build ;;
    status) status ;;
    *) echo "usage: $0 setup|install|build|status"; exit 2 ;;
esac
