#!/usr/bin/env bash
# Run the cross-built Kindle binary in an ARM VM with glibc 2.19 (older than the Kindle's 2.20)
# and KOReader's libstdc++/LibreSSL, then do a real pinned-TLS landing against the Rust server.
# Catches startup crashes (like the zig glibc-2.20 segfault) before the device does.
#   tests/kindle-vm.sh        (needs nix for qemu; Kindle mounted or cached KOReader libs)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="${POMO_KNDL_SYSROOT:-$HOME/.cache/pomo-kndl/sysroot}"
VM="$HOME/.cache/pomo-kndl/vm"
M=https://archive.debian.org/debian
KOLIBS="${KOLIBS:-/Volumes/Kindle/koreader/libs}"
mkdir -p "$VM"
QEMU="$(nix build --no-link --print-out-paths nixpkgs#qemu)/bin/qemu-system-arm"

if [ ! -f "$VM/.done" ]; then
  curl -sfL "$M/dists/stretch/main/installer-armhf/current/images/netboot/vmlinuz" -o "$VM/vmlinuz"
  curl -sfL "$M/dists/stretch/main/installer-armhf/current/images/netboot/initrd.gz" -o "$VM/di.gz"
  curl -sfL "$M/dists/jessie/main/binary-armhf/Packages.gz" | gunzip > "$VM/Packages"
  for p in libc6 libgcc1 busybox-static; do
    u=$(awk -v p=$p '$1=="Package:"{x=($2==p)} x&&$1=="Filename:"{print $2;exit}' "$VM/Packages")
    curl -sfL "$M/$u" -o "$VM/$p.deb"
  done
  touch "$VM/.done"
fi

W="$(mktemp -d)"; trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$W"' EXIT
R="$W/root"; mkdir -p "$R"/{app/k,kolibs,mods,proc,dev,bin,etc}
for p in libc6 libgcc1 busybox-static; do
  m=$(ar t "$VM/$p.deb" | grep data.tar)
  (cd "$R" && case $m in *xz) ar p "$VM/$p.deb" "$m" | tar xJ ;; *) ar p "$VM/$p.deb" "$m" | tar xz ;; esac)
done
ln -sf /bin/busybox "$R/bin/sh"
(cd "$W" && gunzip -c "$VM/di.gz" | cpio -id 'lib/modules/*/kernel/drivers/virtio/*' 'lib/modules/*/kernel/drivers/net/virtio_net.ko' 2>/dev/null)
find "$W/lib" -name '*.ko' -exec cp {} "$R/mods/" \;
for l in libssl.so.60 libcrypto.so.57 libstdc++.so.6; do
  src="$KOLIBS/$l"; [ -f "$src" ] || src="$CACHE/kindlelibs/$l"
  cp "$src" "$R/kolibs/"
done
cp "$CACHE/root/lib/arm-linux-gnueabihf/libgcc_s.so.1" "$R/kolibs/" 2>/dev/null || true
cp "$ROOT/kindle/build-kindle/pomo-kndl-dev" "$R/app/"

CLI="$ROOT/cli/target/debug/pomo-kndl"; H="$W/home"; PORT=$((20000 + RANDOM % 20000))
"$CLI" --home "$H" add "VM task" >/dev/null
"$CLI" --home "$H" pair --host 10.0.2.2 --port $PORT --out "$R/app/k/pairing.json" >/dev/null
cat > "$R/init" <<'EOF'
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc; mount -t devtmpfs dev /dev 2>/dev/null
for m in virtio virtio_ring virtio_mmio virtio_net; do insmod /mods/$m.ko 2>/dev/null; done; sleep 1
ip link set eth0 up; ip addr add 10.0.2.15/24 dev eth0; ip route add default via 10.0.2.2
export LD_LIBRARY_PATH=/kolibs; cd /app
echo "RESULT state $(./pomo-kndl-dev k state >/dev/null 2>&1; echo $?)"
echo "RESULT sync1 $(./pomo-kndl-dev k sync >/dev/null 2>&1; echo $?)"
./pomo-kndl-dev k start 1; ./pomo-kndl-dev k end; ./pomo-kndl-dev k done
echo "RESULT sync2 $(./pomo-kndl-dev k sync >/dev/null 2>&1; echo $?)"
poweroff -f
EOF
chmod +x "$R/init"
(cd "$R" && find . | cpio -o -H newc 2>/dev/null | gzip > "$W/initrd.gz")
"$CLI" --home "$H" serve --bind 0.0.0.0:$PORT 2>"$W/serve.log" &
sleep 0.5
OUT=$(timeout 180 "$QEMU" -M virt -cpu cortex-a7 -m 512 -nographic -no-reboot -kernel "$VM/vmlinuz" \
  -initrd "$W/initrd.gz" -append "console=ttyAMA0 rdinit=/init quiet" \
  -netdev user,id=n -device virtio-net-device,netdev=n 2>&1 | grep RESULT || true)
echo "$OUT"
fail=0
for k in state sync1 sync2; do echo "$OUT" | grep -q "RESULT $k 0" || { echo "FAIL $k"; fail=1; }; done
TASKS=$("$CLI" --home "$H" tasks); HIST=$("$CLI" --home "$H" history)
echo "$TASKS" | grep -q '\[x\] .*VM task' || { echo "FAIL: done not landed"; fail=1; }
echo "$HIST" | grep -q interrupted || { echo "FAIL: session not landed"; fail=1; }
[ $fail = 0 ] && echo "kindle-vm: all checks passed"
exit $fail
