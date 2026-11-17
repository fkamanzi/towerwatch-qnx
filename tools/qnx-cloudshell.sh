#!/usr/bin/env bash
# Build and run towerwatch on QNX 8.0 from a browser-only Linux shell
# (Google Cloud Shell, Ubuntu 24.04), following QNX's documented headless
# installation: myQNX login with curl, unattended QNX Software Center install,
# qnxsoftwarecenter_clt for the licence and QNX SDP 8.0, mkqnximage and QEMU
# for the target.
#
#   bash tools/qnx-cloudshell.sh setup     QEMU and helpers (no account needed)
#   bash tools/qnx-cloudshell.sh install   asks for your myQNX email and password once,
#                                          installs QNX Software Center and QNX SDP 8.0
#   bash tools/qnx-cloudshell.sh build     builds towerwatch, pwrsim, pwrctl with qcc
#   bash tools/qnx-cloudshell.sh image     QNX 8.0 x86_64 VM image with mkqnximage
#   bash tools/qnx-cloudshell.sh boot      boots it in QEMU (TCG, no KVM or bridge needed), waits for ssh
#   bash tools/qnx-cloudshell.sh run       copies towerwatch to the target and runs the demo
#   bash tools/qnx-cloudshell.sh all       build, image, boot, run
#   bash tools/qnx-cloudshell.sh stop      stops the VM
#   bash tools/qnx-cloudshell.sh status    what is installed and running
#
# The password is read without echo, kept only in this shell process and never
# written to disk. Large files go to /tmp (Cloud Shell keeps $HOME small).
# Cloud Shell has no KVM, so the target CPU is emulated: timing figures from
# this route show the software working, not what QNX does on real hardware.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

QSC_URL="https://www.qnx.com/download/download/79441/qnx-setup-2.0.4-202501021438-linux.run"
WORK=/tmp/qnx
QSC_DIR=$WORK/qnxinstall
SDP_DIR=$WORK/qnx800
VM_DIR=$WORK/vm
RES_DIR=$WORK/results
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR=$REPO_DIR/build/qnx/gcc_ntox86_64
SSH_KEY=$HOME/.ssh/qnx_vm
SSH_PORT=${QNX_SSH_PORT:-10022}   # 2222 is taken in Cloud Shell


say() { printf '\n== %s\n' "$*"; }

sdp_env() {
    [ -f "$SDP_DIR/qnxsdp-env.sh" ] || { echo "QNX SDP 8.0 is not installed yet: run install first"; exit 1; }
    set +u
    # shellcheck disable=SC1091
    source "$SDP_DIR/qnxsdp-env.sh" >/dev/null
    set -u
}

qssh() {
    ssh -i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR -o ConnectTimeout=10 -o BatchMode=yes root@127.0.0.1 "$@"
}

setup() {
    say "Installing QEMU and helpers"
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq qemu-system-x86 qemu-utils mtools dosfstools >/dev/null
    mkdir -p "$WORK"
    qnx_home
    qemu-system-x86_64 --version | head -1
    df -h /tmp | tail -1
}

# QNX Software Center keeps its licences and package cache in ~/.qnx and
# refuses a symlink there ("Cannot create ~/.qnx"). Cloud Shell's home disk is
# small, so bind-mount a directory from the larger /tmp disk onto ~/.qnx.
qnx_home() {
    mkdir -p "$WORK/dot-qnx"
    [ -L "$HOME/.qnx" ] && rm -f "$HOME/.qnx"
    mkdir -p "$HOME/.qnx"
    if ! mountpoint -q "$HOME/.qnx"; then
        cp -a "$HOME/.qnx/." "$WORK/dot-qnx/" 2>/dev/null || true
        sudo mount --bind "$WORK/dot-qnx" "$HOME/.qnx" || echo "note: no bind mount, ~/.qnx stays on the home disk"
    fi
}

# Cloud Shell recycles its VM after a while idle: system packages, mounts and
# /tmp go, $HOME stays. Re-run setup whenever QEMU or the ~/.qnx mount is missing.
ensure_setup() {
    if ! command -v qemu-system-x86_64 >/dev/null || ! mountpoint -q "$HOME/.qnx"; then
        setup
    fi
}

# Runs one QNX Software Center step from its install directory, as QNX's
# instructions say. If the step fails, the credentials stay in this process
# and the script waits: fix the cause from another terminal, then press Enter
# to retry (or type s and Enter to skip), without typing the password again.
qsc_step() {
    local label=$1 reply
    shift
    say "$label"
    until (cd "$QSC_DIR/qnxsoftwarecenter" && "$@"); do
        echo "!! '$label' failed. Fix the cause, then press Enter to retry, or type s and Enter to skip."
        read -r reply
        [ "$reply" = s ] && return 0
    done
}

install_qnx() {
    mkdir -p "$WORK"
    ensure_setup
    read -r -p "myQNX email: " MYQNX_USER
    read -r -s -p "myQNX password (not shown): " MYQNX_PASSWORD
    echo
    local clt="$QSC_DIR/qnxsoftwarecenter/qnxsoftwarecenter_clt"

    if [ ! -x "$clt" ]; then
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
    else
        say "QNX Software Center already installed in $QSC_DIR"
    fi

    local cred=(-myqnx.user="$MYQNX_USER" -myqnx.password="$MYQNX_PASSWORD")
    # One licence is deployed to the account, so -activateAll activates it
    # without typing the key (QNX's unattended-installation instructions).
    qsc_step "Activating the licence deployed to your myQNX account" \
        "$clt" -syncLicenseKeys "${cred[@]}" -activateAll -listLicenseKeys
    qsc_step "Mirroring QNX package metadata" "$clt" -mirror "${cred[@]}"
    qsc_step "Installing QNX SDP 8.0 (several GB, takes a while)" \
        "$clt" -cleanInstall -destination "$SDP_DIR" -installBaseline com.qnx.qnx800 "${cred[@]}"
    unset MYQNX_PASSWORD cred
    ls "$SDP_DIR"
    du -sh "$SDP_DIR" "$WORK/dot-qnx" 2>/dev/null || true
    df -h /tmp | tail -1
    say "QNX SDP 8.0 installed in $SDP_DIR"
}

build() {
    sdp_env
    cd "$REPO_DIR"
    say "Building towerwatch, pwrsim and pwrctl with qcc for x86_64 (QNX_HOST=$QNX_HOST)"
    make qnx
    file "$BUILD_DIR"/* || true
}

image() {
    sdp_env
    [ -f "$SSH_KEY" ] || ssh-keygen -q -t ed25519 -N '' -f "$SSH_KEY"
    mkdir -p "$VM_DIR"
    cd "$VM_DIR"
    say "Building a QNX 8.0 x86_64 image for QEMU with mkqnximage"
    local opts=(--type=qemu --arch=x86_64 --hostname=towerwatch)
    mkqnximage "${opts[@]}" --ssh-ident="$SSH_KEY" --build
    # The ssh identity must end up as root's authorized key; this finds out
    # whether this mkqnximage wanted the private key path or the .pub file.
    local key; key=$(cut -d' ' -f2 "$SSH_KEY.pub")
    if ! grep -rqa -- "$key" output 2>/dev/null; then
        say "Rebuilding with the public key file"
        mkqnximage "${opts[@]}" --ssh-ident="$SSH_KEY.pub" --build
        grep -rqa -- "$key" output 2>/dev/null || echo "note: ssh key not found in the image"
    fi
    ls -l output
}

# mkqnximage --run needs a host network bridge (brctl, br0 or virbr0) and uses
# KVM when it can; Cloud Shell has neither. So start the same QEMU command that
# mkqnximage's runimage builds for an x86_64, non-UEFI image (IDE disk,
# virtio-net, virtio-rng, -kernel ifs.bin, serial console to a file), with TCG
# emulation and user-mode networking: the target's ssh port on 127.0.0.1:2222.
opt() { sed -n "s/^OPT_$1='\(.*\)'$/\1/p" "$VM_DIR/output/options" | head -1; }

boot() {
    ensure_setup
    cd "$VM_DIR"
    if qssh true 2>/dev/null; then say "QNX is already running"; return 0; fi
    [ -f output/qemu.pid ] && kill "$(cat output/qemu.pid)" 2>/dev/null && sleep 1
    local cpus ram mac
    cpus=$(opt CPU); ram=$(opt RAM); mac=$(opt MACADDR)
    local cmd=(qemu-system-x86_64 -smp "${cpus:-2}" -cpu "${QNX_QEMU_CPU:-Nehalem}" -m "${ram:-1G}"
        -drive file=output/disk-qemu.vmdk,if=ide,id=drv0
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22"
        -device "virtio-net-pci,netdev=net0${mac:+,mac=$mac}"
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0
        -kernel output/ifs.bin -serial file:output/qemu.out
        -display none -monitor none -pidfile output/qemu.pid -daemonize)
    printf '%q ' "${cmd[@]}" > output/qemu.cmd
    say "Booting QNX 8.0 in QEMU (headless, emulated CPU)"
    cat output/qemu.cmd; echo
    "${cmd[@]}"
    local i
    for i in $(seq 1 60); do
        sleep 5
        if qssh true 2>/dev/null; then
            say "QNX is up after about $((i * 5)) s"
            qssh 'uname -a'
            return 0
        fi
        [ $((i % 6)) -eq 0 ] && echo "  still booting; console: $(tail -c 300 output/qemu.out | tr -s '\r\n' ' ' | tail -c 120)"
    done
    echo "No ssh yet. Console:"
    tail -40 output/qemu.out || true
    return 1
}

run_demo() {
    mkdir -p "$RES_DIR"
    say "Copying towerwatch to the QNX target"
    local f
    for f in towerwatch pwrsim pwrctl; do
        qssh "cat > /tmp/$f && chmod 755 /tmp/$f" < "$BUILD_DIR/$f"
    done
    qssh 'cat > /tmp/outage.txt' < "$REPO_DIR/scenarios/outage.txt"
    qssh 'cat > /tmp/quick.txt' < "$REPO_DIR/scenarios/quick.txt"

    say "Target"
    qssh 'uname -a; pidin info 2>/dev/null | head -4' | tee "$RES_DIR/target.txt"

    say "1. Mains outage, generator fails to start, protection on (1 kHz timer pulses, SCHED_FIFO 40)"
    qssh '/tmp/towerwatch -m -r 1000 -d 110 -s /tmp/outage.txt -c /tmp/protect.csv' | tee "$RES_DIR/run_protect.txt"

    say "2. Same outage, observe only (alarms, no commands)"
    qssh '/tmp/towerwatch -m -n -r 1000 -d 110 -s /tmp/outage.txt -c /tmp/observe.csv -q' | tee "$RES_DIR/run_observe.txt"

    say "3. The plant as a resource manager: pwrsim at /dev/pwrsim, towerwatch -R over message passing"
    qssh '/tmp/pwrsim -m > /tmp/pwrsim.log 2>&1 & sleep 2; ls -l /dev/pwrsim; /tmp/pwrctl status;
          (sleep 4; /tmp/pwrctl status; sleep 6; /tmp/pwrctl status) &
          /tmp/towerwatch -R -m -r 1000 -d 14 -s /tmp/quick.txt; /tmp/pwrctl status;
          pidin -p pwrsim 2>/dev/null | head -8; slay -f pwrsim 2>/dev/null' \
        | tee "$RES_DIR/run_resmgr.txt"

    say "4. slog2 records from towerwatch"
    qssh 'slog2info -b towerwatch 2>/dev/null | tail -15' | tee "$RES_DIR/slog2.txt"

    qssh 'cat /tmp/protect.csv' > "$RES_DIR/protect.csv"
    qssh 'cat /tmp/observe.csv' > "$RES_DIR/observe.csv"
    say "Results in $RES_DIR"
    ls -l "$RES_DIR"
}

stop_vm() {
    [ -f "$VM_DIR/output/qemu.pid" ] && kill "$(cat "$VM_DIR/output/qemu.pid")" 2>/dev/null && echo "QEMU stopped" || echo "QEMU not running"
}

status() {
    echo "QEMU: $(command -v qemu-system-x86_64 || echo missing)"
    echo "QNX Software Center: $([ -x "$QSC_DIR/qnxsoftwarecenter/qnxsoftwarecenter_clt" ] && echo installed || echo missing)"
    echo "QNX SDP 8.0: $([ -f "$SDP_DIR/qnxsdp-env.sh" ] && echo installed || echo missing)"
    echo "towerwatch for QNX: $([ -x "$BUILD_DIR/towerwatch" ] && echo built || echo not built)"
    echo "VM image: $([ -d "$VM_DIR/output" ] && echo built || echo not built)"
    echo "QNX target: $(qssh true 2>/dev/null && echo running || echo not running)"
    df -h /tmp | tail -1
}

case "${1:-status}" in
    setup) setup ;;
    install) install_qnx ;;
    build) build ;;
    image) image ;;
    boot) boot ;;
    run) run_demo ;;
    all) build && image && boot && run_demo ;;
    stop) stop_vm ;;
    status) status ;;
    *) echo "usage: $0 setup|install|build|image|boot|run|all|stop|status"; exit 2 ;;
esac
