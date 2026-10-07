#!/usr/bin/env bash
set -Eeuo pipefail

# Deploy freshly built Mooncake package into this container.
# Run INSIDE container nhn_0831 after build_memfabric.sh completes on the host.
# Usage (inside container): bash /home/n00693955/code/Mooncake/scripts/deploy_memfabric_container.sh

GREEN="\033[0;32m"; BLUE="\033[0;34m"; YELLOW="\033[0;33m"; NC="\033[0m"
print_section() { echo -e "\n${BLUE}=== $1 ===${NC}"; }
print_success()  { echo -e "${GREEN}✓ $1${NC}"; }
print_warn()     { echo -e "${YELLOW}! $1${NC}"; }

REPO="/home/n00693955/code/Mooncake"
BUILD="$REPO/build"
STAGING="/tmp/mooncake_deploy"

print_section "Staging build artifacts"
rm -rf "$STAGING" && mkdir -p "$STAGING"

# .py files (full set from mooncake-wheel)
cp "$REPO"/mooncake-wheel/mooncake/*.py "$STAGING/"
cp "$REPO"/mooncake-wheel/mooncake/README.md "$STAGING/" 2>/dev/null || true

# .so files (build output)
cp "$BUILD/mooncake-integration/engine.cpython-312-aarch64-linux-gnu.so" "$STAGING/"
cp "$BUILD/mooncake-integration/store.cpython-312-aarch64-linux-gnu.so" "$STAGING/" 2>/dev/null || true
cp "$BUILD/mooncake-common/src/libmooncake_common.so" "$STAGING/"
cp "$BUILD/mooncake-common/libasio.so" "$STAGING/"
cp "$BUILD/mooncake-transfer-engine/src/libtransfer_engine.so" "$STAGING/"
cp "$BUILD/mooncake-transfer-engine/tent/src/libtent_shared.so" "$STAGING/"
cp "$BUILD/mooncake-transfer-engine/tent/src/metrics/libtent_metrics.so" "$STAGING/"
cp "$BUILD/mooncake-transfer-engine/tent/src/python/tent.cpython-312-aarch64-linux-gnu.so" "$STAGING/"
cp "$BUILD/mooncake-store/src/libmooncake_store.so" "$STAGING/" 2>/dev/null || true

print_success "Staged $(ls "$STAGING" | wc -l) files"

print_section "Replacing mooncake packages"

for dest in \
  /usr/local/Ascend/ascend-toolkit/latest/python/site-packages/mooncake \
  /usr/local/Ascend/cann-9.0.1/python/site-packages/mooncake \
  /usr/local/python3.12.13/lib/python3.12/site-packages/mooncake; do

  if [ -d "$dest" ]; then
    echo "Replacing $dest ..."
    rm -rf "${dest}_old"
    mv "$dest" "${dest}_old"
    mkdir -p "$dest"
    cp "$STAGING"/* "$dest/"
  fi
done

rm -rf "$STAGING"
print_success "All mooncake directories replaced (old versions backed up as *_old)"

print_section "Verification"
python3 -c '
import mooncake
print("mooncake:", mooncake.__file__)
import mooncake.engine
print("mooncake.engine: OK")
import tent
print("tent: OK")
' 2>&1 || print_warn "Some imports failed — check error above"

print_section "Done"
echo -e "  ${GREEN}✓${NC} mooncake package replaced"
echo -e "  ${GREEN}✓${NC} old versions backed up as *_old"
echo
echo -e "${YELLOW}Rollback:${NC}"
echo "  for d in /usr/local/Ascend/ascend-toolkit/latest/python/site-packages/mooncake_old \\"
echo "          /usr/local/Ascend/cann-9.0.1/python/site-packages/mooncake_old \\"
echo "          /usr/local/python3.12.13/lib/python3.12/site-packages/mooncake_old; do"
echo "    [ -d \"\$d\" ] && rm -rf \"\${d%_old}\" && mv \"\$d\" \"\${d%_old}\""
echo "  done"
