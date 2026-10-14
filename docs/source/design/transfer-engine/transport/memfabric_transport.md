# MemFabric Transport

MemFabric Transport is located at `Mooncake/mooncake-transfer-engine/tent/src/transport/memfabric`. It is a data transport backend within the TENT framework that dynamically loads the memfabric shared library (`libmf_smem.so`) via dlopen to provide SDMA/RDMA data transfer capabilities for Ascend NPU.

## Overview

MemFabric is a shared memory transport framework developed by Huawei, implementing cross-node/cross-device batch data copy via the smem_trans API. MemFabric Transport wraps it as a TENT Transport interface with the following characteristics:

- **Dynamic Loading**: Loads `libmf_smem.so` via dlopen/dlsym; no compile-time dependency on memfabric headers
- **Batch Transfer**: Supports `smem_trans_batch_copy` for asynchronous batch transfer, reducing per-call overhead
- **TENT Integration**: Participates in transport selection and failover as a TENT transport type
- **Multi-Protocol**: Supports `device_rdma`、`device_urma`、`device_sdma`, and other underlying operation types

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

Set `MEMFABRIC_WHEEL` to the wheel built from a compatible MemFabric revision:

```bash
pip install "$MEMFABRIC_WHEEL"  # Wheel built with the compatible query ABI below
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
| `MF_TRANSFER_TIMEOUT` | No | `10000` | Quiesce polling timeout in milliseconds |
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
export MF_TRANSFER_TIMEOUT=10000
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
| `QuiesceDrainsInflightBatches` | Quiesce polls completion without an external polling thread |
| `QuiesceTimeoutWithPendingBatches` | Timeout preserves pending state and keeps new submissions blocked |
| `QuiesceSerializesWithAcceptedSubmission` | An accepted submission is tracked before quiesce polls it |
| `QuiesceAndExternalPollConsumeCompletionOnce` | Concurrent polling consumes each terminal result once |
| `QueryErrorPreservesPending` | Query errors do not become execution results |
| `ExecutionRetryCodeIsTerminalFailure` | Query return codes and execution results have distinct meanings |
| `MixedGroupsKeepIndependentResultsAndBytes` | Execution failures do not change other groups or count successful bytes |
| `AddRemoveMemoryBuffer` | Memory registration/deregistration |
| `AddMemoryBufferFailsBeforeInstall` | Error handling when not installed |
| `SubmitFailsWhenBatchCopyFails` | Error handling when batch_copy fails |

Tests use mock ACL functions and mock `MemFabricUnderApi`; no real memfabric hardware required.

### Completion contract

Use a MemFabric build with the four-argument
`smem_trans_batch_status_query(handle, batch_id, &result, flags)` ABI, such as
`trans@8d0dbbf9`, or a compatible build. The old three-argument API uses the same
symbol name; the package version `1.3.0` alone does not distinguish these builds.
Deploy matching libraries and headers; this adapter does not support the old ABI.

`SM_NEED_RETRY` means pending. `SM_OK` means the execution result is available
and **all memory accesses by that batch have stopped**, even if `result` reports
failure. This is a required backend contract, not something the adapter can
infer from an error code. Only a successful execution counts transferred bytes.
A failed batch can have partially modified memory; zero confirmed bytes does
not imply rollback.

Queries consume the backend result once. The adapter caches each terminal result
for all tasks in that vendor batch. A query error is returned to the caller and
leaves tasks pending without decrementing the pending count.

`quiesce()` stops new submissions and polls outstanding tasks until their results
are available or `MF_TRANSFER_TIMEOUT` expires. It reuses normal status handling
to update task states and the in-flight count. Polling and submission are
serialized; the polling wait releases the lock between iterations. Submission
remains disabled after quiesce returns, including after a timeout.

A timeout returns an error without completing pending tasks or clearing their
in-flight count. A later query or quiesce call can still observe completion.
The timeout does not cancel backend operations or preempt a blocked backend
call. The engine currently logs quiesce errors and continues teardown, so a
timeout is not proof that buffers and batches are safe to reclaim.

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
