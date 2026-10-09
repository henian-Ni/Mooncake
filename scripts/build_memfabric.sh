#!/usr/bin/env bash
set -Eeuo pipefail

# Build Mooncake TENT + memfabric transport + Store.
# Usage: bash scripts/build_memfabric.sh
# Prerequisites: Mooncake source at target branch, CANN toolkit installed.

GREEN="\033[0;32m"; BLUE="\033[0;34m"; YELLOW="\033[0;33m"; NC="\033[0m"
print_section() { echo -e "\n${BLUE}=== $1 ===${NC}"; }
print_success()  { echo -e "${GREEN}✓ $1${NC}"; }
print_warn()     { echo -e "${YELLOW}! $1${NC}"; }

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="${BUILD_JOBS:-$(nproc)}"
BUILD_DIR="${BUILD_DIR:-build}"

cd "$REPO_ROOT"

# 1. Source CANN
print_section "Sourcing CANN"
if [ -f /usr/local/Ascend/ascend-toolkit/set_env.sh ]; then
    source /usr/local/Ascend/ascend-toolkit/set_env.sh
    print_success "CANN sourced"
elif [ -n "${ASCEND_HOME_PATH:-}" ]; then
    source "${ASCEND_HOME_PATH}/set_env.sh"
    print_success "CANN sourced from ASCEND_HOME_PATH"
else
    print_warn "CANN not found, ensure ACL headers are available"
fi

# 2. Git submodules
print_section "Git submodules"
git submodule update --init --recursive
print_success "Submodules ready"

# 3. Ascend dependencies (with msgpack-c Boost patch)
print_section "Installing Ascend dependencies"
DEP_SCRIPT="scripts/ascend/dependencies_ascend_installation.sh"
if [ -f "$DEP_SCRIPT" ]; then
    if ! grep -q 'disable msgpack Boost lookup' "$DEP_SCRIPT"; then
        python3 - "$DEP_SCRIPT" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
old = '    git checkout cpp-7.0.0\n    rm -rf build\n    mkdir -p build && cd build\n    cmake ..'
new = '    git checkout cpp-7.0.0\n    # disable msgpack Boost lookup\n    sed -i \'s/FIND_PACKAGE *(Boost REQUIRED)/# &/\' CMakeLists.txt\n    rm -rf build\n    mkdir -p build && cd build\n    cmake ..'
if old in s: p.write_text(s.replace(old, new, 1))
PY
    fi
    bash "$DEP_SCRIPT" || print_warn "Ascend deps may have partially failed"
else
    print_warn "Ascend dep script not found, skipping"
fi

# 4. CMake + Build + Install
print_section "Building (TENT + memfabric + Store)"

cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DUSE_TENT=ON \
    -DUSE_MEMFABRIC=ON \
    -DUSE_HTTP=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_UNIT_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD_DIR" -j 255
cmake --install "$BUILD_DIR"
print_success "Build + install complete"

# 5. Python package files
print_section "Installing Python package"
SITE_DIR=$(python3 -c "import site; print(site.getsitepackages()[0])")
PKG_DIR="${SITE_DIR}/mooncake"
mkdir -p "$PKG_DIR"
cp -r python/mooncake/* "$PKG_DIR/"
cp -r mooncake-wheel/mooncake/* "$PKG_DIR/" 2>/dev/null || true

# cmake install already installs engine.so / store.so / asio_shared.so / tent_metrics.so to PKG_DIR.
# Manually copy: libtent_shared.so (only installed to lib/) + tentpy (no install rule).
install -m 755 "${BUILD_DIR}/mooncake-transfer-engine/tent/src/libtent_shared.so" "$PKG_DIR/" 2>/dev/null || true
TENTPY_SO=$(find "${BUILD_DIR}" -name "tent.cpython-*.so" -print -quit)
[ -f "$TENTPY_SO" ] && install -m 755 "$TENTPY_SO" "$PKG_DIR/"

print_success "Python package installed to ${PKG_DIR}"

# 6. Verification
print_section "Verification"
export LD_LIBRARY_PATH="/usr/local/lib:${PKG_DIR}:${LD_LIBRARY_PATH:-}"
python3 -c 'import mooncake; print("mooncake: OK")'
python3 -c 'import mooncake.engine; print("mooncake.engine: OK")' || print_warn "mooncake.engine import failed"
python3 -c 'import tent; print("tent: OK")' || print_warn "tent import failed"
command -v mooncake_master && print_success "mooncake_master found"

echo
print_section "Done"
echo -e "  ${GREEN}✓${NC} libtransfer_engine.so (TENT + memfabric transport)"
echo -e "  ${GREEN}✓${NC} engine.so / store.so (Python modules)"
echo -e "  ${GREEN}✓${NC} tent.cpython-*.so (TENT Python module)"
echo
echo -e "${YELLOW}Runtime setup:${NC}"
echo -e "  export MC_USE_TENT=1"
echo -e "  export MF_DATA_OP_TYPE=device_sdma"
echo -e "  export LD_LIBRARY_PATH=/path/to/libmf_smem.so:\$LD_LIBRARY_PATH"
echo -e "  source /usr/local/Ascend/ascend-toolkit/set_env.sh"
