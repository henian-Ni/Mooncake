# MemFabric Transport

MemFabric Transport is located at `Mooncake/mooncake-transfer-engine/tent/src/transport/memfabric`. It is a data transport backend within the TENT framework that dynamically loads the memfabric shared library (`libmf_smem.so`) via dlopen to provide SDMA/RDMA data transfer capabilities for Ascend NPU.

## Overview

MemFabric is a shared memory transport framework developed by Huawei, implementing cross-node/cross-device batch data copy via the smem_trans API. MemFabric Transport wraps it as a TENT Transport interface with the following characteristics:

- **Dynamic Loading**: Loads `libmf_smem.so` via dlopen/dlsym; no compile-time dependency on memfabric headers
- **Batch Transfer**: Supports `smem_trans_batch_copy` for asynchronous batch transfer, reducing per-call overhead
- **TENT Integration**: Participates in transport selection and failover as a TENT transport type
- **Multi-Protocol**: Supports `device_sdma`, `device_rdma`, `host_rdma`, and other underlying operation types

### Architecture

```
┌─────────────────────────────────────────────┐
│  TENT TransferEngine                        │
│  - transport_selector selects memfabric     │
├─────────────────────────────────────────────┤
│  MemFabricTransport                         │
│  - install: load lib → create smem_trans    │
│  - submitTransferTasks: batch batch_copy    │
│  - getTransferStatus: poll batch status     │
│  - addMemoryBuffer: register local memory   │
├─────────────────────────────────────────────┤
│  MemFabricUnderApi (dlopen wrapper)         │
│  - LoadLibrary / CleanupLibrary             │
│  - 13 smem_trans API function pointers      │
├─────────────────────────────────────────────┤
│  libmf_smem.so (memfabric shared library)   │
│  - smem_trans_init / create / batch_copy    │
└─────────────────────────────────────────────┘
```

### Key Components

| Component | Description |
|-----------|-------------|
| `MemFabricTransport` | Implements TENT Transport interface; manages smem_trans instance lifecycle |
| `MemFabricUnderApi` | All-static class wrapping dlopen/dlsym; provides inline forwarding for 13 APIs |
| `MemFabricSubBatch` | Batch task container with task_list, batch_status_cache, pending_batch_count |

## Prerequisites

### Hardware and Software

- **Hardware**: Atlas A2/A3/A5 series NPU
- **CANN**: >= 9.0.0
- **memfabric**: `libmf_smem.so` installed and available in `LD_LIBRARY_PATH`

### Installing memfabric

Install the memfabric wheel package from PyPI:

```bash
pip install memfabric_hybrid==1.2.1  # See https://pypi.org/project/memfabric-hybrid/#files for the latest version
```

After installation, `libmf_smem.so` is placed in the Python site-packages
directory. Add it to `LD_LIBRARY_PATH`:

```bash
SITE_PKG=$(python3 -c "import site; print(site.getsitepackages()[0])")
export LD_LIBRARY_PATH=$SITE_PKG/memfabric_hybrid/lib:$LD_LIBRARY_PATH
```

Verify:
```bash
ls $SITE_PKG/memfabric_hybrid/lib/libmf_smem.so
```

## Building

### Enabling MemFabric Transport

```bash
cmake -S . -B build -G Ninja \
    -DUSE_TENT=ON \
    **-DUSE_MEMFABRIC=ON** \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel
cmake --install build
```

`USE_MEMFABRIC` automatically enables ACL header/library paths and is decoupled from `USE_ASCEND` / `USE_ASCEND_DIRECT`.

## Environment Variables

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `LD_LIBRARY_PATH` | Yes | — | Directory containing `libmf_smem.so` |
| `MF_PROTOCOL` | Yes | — | Transport protocol: `device_sdma` / `device_rdma` / `host_rdma` / `device_urma` / `device_uboe` |
| `MC_USE_TENT` | Yes | — | Set to `1` to enable TENT mode |
| `MC_TENT_CONF` | Yes | — | Path to TENT config JSON file; must include `transports/memfabric/enable: true` |
| `MC_STORE_USE_HUGEPAGE` | No | — | Set to `1` to enable Store hugepage allocation (recommended when using Store pooling) |
| `MC_STORE_HUGEPAGE_SIZE` | No | `2MB` | Hugepage size: `2MB` / `512MB` / `1GB`; must match the system-allocated hugepage size |

### TENT Config File Example

```json
{
    "transports": {
        "memfabric": {
            "enable": true
        }
    }
}
```

### Basic Configuration

```bash
export MF_PROTOCOL=device_rdma
export MC_USE_TENT=1
export MC_TENT_CONF=/path/to/memfabric_tent.json
SITE_PKG=$(python3 -c "import site; print(site.getsitepackages()[0])")
export LD_LIBRARY_PATH=$SITE_PKG/memfabric_hybrid/lib:$LD_LIBRARY_PATH
source /usr/local/Ascend/ascend-toolkit/set_env.sh
```

## Verification

### Unit Tests

```bash
# Requires USE_MEMFABRIC=ON + BUILD_UNIT_TESTS=ON at build time
ctest --test-dir build -R 'memfabric' --output-on-failure
```

Test coverage:

| Test | Description |
|------|-------------|
| `InstallSucceeds` | install succeeds; smem_trans instance created |
| `InstallFailsWhenCreateReturnsNull` | Error handling when SmemTransCreate returns null |
| `SubmitAndGetStatusCompleted` | Batch submit + status query + cache hit |
| `SubmitAndGetStatusPending` | Transfer in-progress status |
| `FailedBatchPropagatesToAllTasks` | Failure propagation to all tasks in the same batch |
| `PendingBatchDestructorWarning` | Destructor warning for incomplete batches |
| `AddRemoveMemoryBuffer` | Memory registration/deregistration |
| `AddMemoryBufferFailsBeforeInstall` | Error handling when not installed |
| `SubmitFailsWhenBatchCopyFails` | Error handling when batch_copy fails |

Tests use mock ACL functions and mock `MemFabricUnderApi`; no real memfabric hardware required.

## Important Notes

1. **`libmf_smem.so` must be in `LD_LIBRARY_PATH`**: MemFabric Transport loads it via dlopen at runtime. If the library cannot be found, `install()` returns `DeviceNotFound`.

2. **`MF_PROTOCOL` must be set**: If unset, `install()` returns `InvalidArgument`.

3. **Hugepage configuration**: When using Store pooling (`protocol=ascend`), hugepages must be reserved.
   The page size depends on the system configuration; check with `cat /proc/meminfo | grep Hugepagesize`:
   ```bash
   # Check supported hugepage sizes
   ls /sys/kernel/mm/hugepages/
   # e.g.: hugepages-2048kB  hugepages-524288kB

   # Allocate 2MB hugepages (e.g., 200GB → 102400 pages)
   echo 102400 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

   # Or allocate 512MB hugepages (e.g., 200GB → 400 pages)
   echo 400 > /sys/kernel/mm/hugepages/hugepages-524288kB/nr_hugepages

   # Persist across reboots
   echo 'vm.nr_hugepages = 102400' > /etc/sysctl.d/99-hugepages.conf
   sysctl -p /etc/sysctl.d/99-hugepages.conf

   # Enable Store hugepage allocation
   export MC_STORE_USE_HUGEPAGE=1
   export MC_STORE_HUGEPAGE_SIZE=2MB
   ```
   `MC_STORE_HUGEPAGE_SIZE` accepts `2MB` / `512MB` / `1GB`; it must match the system-allocated hugepage size.

4. **Log level**: MemFabric Transport follows TENT's `log_level` configuration; no separate environment variable is needed.

## Troubleshooting

### `DeviceNotFound: libmf_smem.so unavailable`

```
MemFabricUnderApi::LoadLibrary failed, libmf_smem.so unavailable
```

**Solution**: Ensure `libmf_smem.so` is in `LD_LIBRARY_PATH`:
```bash
SITE_PKG=$(python3 -c "import site; print(site.getsitepackages()[0])")
ls $SITE_PKG/memfabric_hybrid/lib/libmf_smem.so
export LD_LIBRARY_PATH=$SITE_PKG/memfabric_hybrid/lib:$LD_LIBRARY_PATH
```

### `InvalidArgument: MF_PROTOCOL must be set`

**Solution**: Set the `MF_PROTOCOL` environment variable:
```bash
export MF_PROTOCOL=device_rdma
```

### `mmap failed, errno=12 (Cannot allocate memory)`

**Cause**: Insufficient hugepages.

**Solution**:
```bash
# Check supported hugepage sizes
ls /sys/kernel/mm/hugepages/
# Allocate pages of the appropriate size
echo <page_count> > /sys/kernel/mm/hugepages/hugepages-<size_in_kB>/nr_hugepages
```
