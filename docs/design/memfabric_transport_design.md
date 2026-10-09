# MemFabric Transport 对接 Mooncake 设计方案

## 1. 概述

### 1.1 目标

将华为 memfabric（`libmf_smem.so`）的 smem_trans API 接入 Mooncake TENT 框架，作为 Ascend NPU 上的数据传输后端，为 Mooncake Store 提供 KV cache 跨节点/跨设备传输能力。

### 1.2 设计约束

- memfabric 以 `.so` 动态库形式存在，不提供头文件给 Mooncake 编译链路 → 采用 **dlopen/dlsym** 方式
- memfabric 的 `smem_trans_role_t` 角色匹配机制：**两端 role 相同则不记录对端内存映射** → role 需可配置
- TENT Transport 基类要求实现 `install/uninstall/allocateSubBatch/freeSubBatch/submitTransferTasks/getTransferStatus/addMemoryBuffer/removeMemoryBuffer`
- Mooncake Store 的内存分配路径（`protocol="ascend"`）需要独立于 TENT 传输层

### 1.3 架构分层

```
┌─────────────────────────────────────────────┐
│  vLLM-Ascend / Store Client (Python)        │
├─────────────────────────────────────────────┤
│  Mooncake Store (real_client.cpp)           │  ← 内存分配 (aclrtMallocHost / 大页)
│  - protocol="ascend" → AllocateAscendStoreSegment
├─────────────────────────────────────────────┤
│  TENT TransferEngine                        │
│  - transport_loader.cpp: loadTransports()   │  ← MC_TENT_CONF: memfabric.enable=true
│  - transfer_engine_impl.cpp: submit/getStatus│
├─────────────────────────────────────────────┤
│  MemFabricTransport (Transport subclass)     │  ← 本方案核心
│  - install/publish/resolve/submit/status    │
├─────────────────────────────────────────────┤
│  MemFabricUnderApi (all-static, dlopen)     │  ← libmf_smem.so 符号绑定
│  - LoadLibrary/CleanupLibrary/IsLoaded      │
├─────────────────────────────────────────────┤
│  libmf_smem.so (memfabric shared library)   │  ← 外部依赖
│  - smem_trans_init/create/batch_copy/...    │
└─────────────────────────────────────────────┘
```

## 2. 组件设计

### 2.1 `memfabric_types.h` — 类型定义（自包含）

从 memfabric 头文件复制的 C 类型定义，使 Mooncake 编译时不依赖 memfabric 头文件：

| 类型 | 用途 |
|------|------|
| `smem_trans_t` | opaque handle (= `void*`) |
| `smem_trans_role_t` | NONE / SENDER / RECEIVER / BOTH |
| `smem_trans_config_t` | 传输实例配置（role, deviceId, dataOpType, url, ...） |
| `smem_trans_opcode_t` | READ / WRITE |
| `smem_trans_batch_params_t` | 批量传输参数（destUrl, srcList, destList, sizeList, batchSize, opcode, stream, flags） |
| `smem_bm_data_op_type` | 底层数据操作类型（SDMA, DEVICE_RDMA, HOST_RDMA, ...） |
| `smem_trans_peer_down_callback_t` | 对端下线回调 |

### 2.2 `MemFabricUnderApi` — dlopen 封装

**模式**: 参考 TENT 已有 `DlHalApi` 模式，全静态类，无实例、无虚函数。

```
class MemFabricUnderApi {
  static bool LoadLibrary();       // dlopen + dlsym 全部 13 个符号
  static void CleanupLibrary();    // dlclose + 清空函数指针
  static bool IsLoaded();

  // 13 个 static inline 包装函数，内部 null check 后转发
  static inline SmemTransInit(...);
  static inline SmemTransCreate(...);
  static inline SmemTransBatchCopy(...);
  // ...
private:
  static std::mutex gMutex;        // 保护 Load/Cleanup
  static bool gLoaded;
  static void* gLibHandle;
  static smemTransInitFunc pSmemTransInit;  // dlsym 绑定的函数指针
  // ...
};
```

**关键设计点**:

- `DL_LOAD_SYM` 宏：`dlsym` 失败即 `dlclose` + `return false`，不留半加载状态
- `inline` 包装函数：编译期内联，无额外调用开销；`nullptr` check 防止未加载时 crash
- `gMutex` + `gLoaded`：进程级单例加载，多线程安全

### 2.3 `MemFabricTransport` — Transport 实现

继承 TENT `Transport` 基类，实现全部接口：

```cpp
class MemFabricTransport : public Transport {
  // 生命周期
  Status install(...) override;     // 加载库 → init → create → getPort → publish
  Status uninstall() override;      // destroy → uninit → cleanup

  // 批量传输
  Status allocateSubBatch(...) override;   // new MemFabricSubBatch{handle, reserve(max_size)}
  Status freeSubBatch(...) override;       // delete + 清理 pending tasks
  Status submitTransferTasks(...) override; // resolveDestUrl → 按 dest_url 分组 → batch_copy
  Status getTransferStatus(...) override;  // batch_status_query → PENDING/DONE/FAILED

  // 内存注册
  Status addMemoryBuffer(...) override;     // smem_trans_register_mem
  Status removeMemoryBuffer(...) override; // smem_trans_deregister_mem

private:
  Status publishLocalDevices();   // 将 store_url 写入 metadata segment.device_attrs
  Status resolveDestUrl(...);     // 从 metadata 读取对端 store_url

  smem_trans_t handle_;           // smem_trans 实例
  MemFabricConfig config_;         // store_url, device_id, data_op_type
  static std::atomic<int> g_init_ref_;  // smem_trans_init/uninit 引用计数
  static std::mutex g_init_mutex_;
};
```

### 2.4 `MemFabricConfig` — 配置

```cpp
struct MemFabricConfig {
    std::string store_url;                          // smem_trans 端点 (ip:port)
    uint32_t device_id = 0;                         // NPU device ID
    std::optional<smem_bm_data_op_type> data_op_type; // 底层操作类型
    std::string data_op_type_str;                   // 字符串形式（日志用）
};
```

### 2.5 `MemFabricTask` / `MemFabricSubBatch`

```cpp
struct MemFabricTask {
    Request request;
    volatile TransferStatusEnum status_word = PENDING;
    uint64_t batch_id = 0;           // smem_trans 返回的 batch ID
    size_t length = 0;              // 传输长度
    size_t transferred_bytes = 0;   // 已传输字节（DONE 时 = length）
};

struct MemFabricSubBatch : public Transport::SubBatch {
    std::vector<MemFabricTask> task_list;
    smem_trans_t handle;            // 指向 transport 的 handle_（不持有）
    ~MemFabricSubBatch() override;   // 析构时清理 pending batch
};
```

## 3. 数据流

### 3.1 安装流程 (`install`)

```
install(local_segment_name, metadata, ...)
  │
  ├─ MemFabricUnderApi::LoadLibrary()
  │    └─ dlopen("libmf_smem.so", RTLD_NOW|RTLD_GLOBAL) + dlsym 13 个符号
  │
  ├─ 解析配置
  │    ├─ MF_DATA_OP_TYPE 环境变量 → parseDataOpType → config_.data_op_type
  │    ├─ MF_LOG_LEVEL（可选）→ SmemSetLogLevel
  │    └─ aclrtGetDevice() → device_id
  │
  ├─ SmemTransInit(device_id, 0)          [refcount: g_init_ref_++]
  │
  ├─ SmemTransConfigInit(&cfg)
  │    └─ cfg.role = SMEM_TRANS_BOTH
  │    └─ cfg.deviceId = device_id
  │    └─ cfg.dataOpType = config_.data_op_type
  │    └─ cfg.url = "host_ip:0"
  │
  ├─ SmemTransCreate(&cfg, 0) → handle_
  │
  ├─ SmemTransGetRpcPort(handle_, &port)
  │    └─ config_.store_url = "host_ip:actual_port"
  │
  ├─ publishLocalDevices()
  │    └─ metadata.segmentManager().updateLocal(...)
  │         └─ segment.device_attrs["memfabric_unique_id"] = config_.store_url
  │
  └─ caps = {dram_to_dram=true, dram_to_gpu=true, gpu_to_dram=true, gpu_to_gpu=true}
```

### 3.2 传输流程 (`submitTransferTasks`)

```
submitTransferTasks(sub_batch, request_list[128])
  │
  ├─ resolveDestUrl(request_list[i]) × 128
  │    └─ metadata.segmentManager().withCachedSegment(target_id)
  │         └─ dest_url = segment.device_attrs["memfabric_unique_id"]
  │
  ├─ 按 dest_url 分组 (unordered_map<url, indices>)
  │    └─ 同一目标的所有 request 分到一组
  │
  └─ for each group:
       ├─ 填充 src_list / dest_list / size_list
       ├─ params = {destUrl, srcList, destList, sizeList, batchSize, opcode}
       ├─ SmemTransBatchCopy(handle_, &params, &batch_id)
       │    └─ 返回 batch_id > 0 = 成功提交（异步执行）
       └─ task_list[i] = {request, batch_id, PENDING}
```

### 3.3 状态查询 (`getTransferStatus`)

```
getTransferStatus(sub_batch, task_id)
  │
  ├─ task.status_word != PENDING → 直接返回缓存状态
  │
  └─ SmemTransBatchStatusQuery(handle_, task.batch_id, 0)
       ├─ SMEM_TRANS_BATCH_DONE (0)    → COMPLETED, transferred_bytes = length
       ├─ SMEM_TRANS_BATCH_PENDING (1) → PENDING
       └─ 其他                           → FAILED
```

### 3.4 内存注册 (`addMemoryBuffer`)

```
addMemoryBuffer(desc{addr, length})
  └─ SmemTransRegisterMem(handle_, addr, length, 0)
       └─ desc.transports.push_back(MEMFABRIC)
```

### 3.5 卸载 (`uninstall`)

```
uninstall()
  ├─ SmemTransDestroy(handle_, 0)
  ├─ g_init_ref_-- → 0 时:
  │    ├─ SmemTransUninit(0)
  │    └─ CleanupLibrary()    [dlclose + 清空指针]
  └─ metadata_.reset()
```

## 4. CMake 集成

### 4.1 构建选项

| 选项 | 位置 | 作用 |
|------|------|------|
| `USE_MEMFABRIC` | `common.cmake:128` | 编译 `USE_MEMFABRIC` 宏 + 启用 ACL 头/库路径 |
| `tent_xport_memfabric` | `memfabric/CMakeLists.txt` | STATIC 库，链接 `tent_interface dl ascendcl` |
| `tent_transport_all` | `transport/CMakeLists.txt:34` | INTERFACE 聚合，包含 memfabric |
| `tent_link_group` | `tent/src/CMakeLists.txt:164` | 链接入 `libtent_shared.so` |

### 4.2 与 `USE_ASCEND_DIRECT` 的解耦

`USE_MEMFABRIC` 独立于 `USE_ASCEND` / `USE_ASCEND_DIRECT`：

```cmake
# common.cmake — ACL 路径条件包含 USE_MEMFABRIC
if(USE_ASCEND OR USE_ASCEND_DIRECT OR USE_MEMFABRIC)
  # 设置 ASCEND_TOOLKIT_ROOT / ASCEND_LIB_DIR / ASCEND_INCLUDE_DIR
endif()

# common.cmake — 单独的编译宏
if(USE_MEMFABRIC)
  add_compile_definitions(USE_MEMFABRIC)
endif()
```

## 5. Store 内存分配集成

### 5.1 分配路径

`protocol="ascend"` 时 Store 走 `AllocateAscendStoreSegment`：

```
real_client.cpp
  ├─ AllocateAscendStoreSegment(...)        ← #if defined(USE_ASCEND_DIRECT) || defined(USE_MEMFABRIC)
  │    └─ ascend_allocate_memory_best_effort(...)
  │         └─ allocate_buffer_mmap_memory(size, align, defer_hugetlb=false)
  │              └─ mmap(MAP_POPULATE | MAP_HUGETLB | MAP_HUGE_2MB | MAP_ANONYMOUS)
  │
  ├─ AgentModeStoreChunkCap()               ← #ifdef USE_ASCEND_DIRECT only (uses ContextManager)
  └─ RestoreAgentModeDeviceZero()           ← #ifdef USE_ASCEND_DIRECT only
```

### 5.2 `ascend_allocator.cpp` 守卫

```cpp
// USE_MEMFABRIC without USE_ASCEND_DIRECT 时不引用 ContextManager
#include "context_manager.h"    // ← #ifdef USE_ASCEND_DIRECT
BindNextAgentDevice()          // ← #ifdef USE_ASCEND_DIRECT body
CommitAgentDeviceSlot()        // ← #ifdef USE_ASCEND_DIRECT body
```

### 5.3 `client_buffer_allocation.cpp` 守卫

```cpp
#if defined(USE_ASCEND_DIRECT) || defined(USE_MEMFABRIC)
  if (protocol == "ascend")
    return ascend_allocate_memory(total_size, protocol);
#endif
```

## 6. 运行时配置

### 6.1 环境变量

| 变量 | 必需 | 默认 | 说明 |
|------|------|------|------|
| `MC_USE_TENT` | 是 | - | `=1` 启用 TENT |
| `MC_TENT_CONF` | 是 | - | JSON 配置，含 `transports/memfabric/enable: true` |
| `MF_DATA_OP_TYPE` | 是 | - | `device_sdma` / `device_rdma` / `host_rdma` / ... |
| `MF_LOG_LEVEL` | 否 | - | memfabric 日志级别（int） |
| `LD_LIBRARY_PATH` | 是 | - | 包含 `libmf_smem.so` 所在目录 |
| `MC_STORE_USE_HUGEPAGE` | 否 | - | 启用大页分配 |
| `MC_STORE_HUGEPAGE_SIZE` | 否 | 2MB | 大页大小 |

### 6.2 TENT 配置 JSON

```json
{
  "transports": {
    "memfabric": {
      "enable": true
    }
  }
}
```

### 6.3 完整启动示例

```bash
export MC_USE_TENT=1
export MC_TENT_CONF='{"transports":{"memfabric":{"enable":true}}}'
export MF_DATA_OP_TYPE=device_sdma
export LD_LIBRARY_PATH=/path/to/libmf_smem.so:$LD_LIBRARY_PATH
source /usr/local/Ascend/ascend-toolkit/set_env.sh
```

## 7. 文件清单

| 文件 | 角色 |
|------|------|
| `tent/include/tent/transport/memfabric/memfabric_types.h` | C 类型定义（自包含） |
| `tent/include/tent/transport/memfabric/memfabric_under_api.h` | dlopen 封装类声明 |
| `tent/src/transport/memfabric/memfabric_under_api.cpp` | dlopen 实现 + 符号绑定 |
| `tent/include/tent/transport/memfabric/memfabric_transport.h` | Transport 类 + Config + Task + SubBatch |
| `tent/src/transport/memfabric/memfabric_transport.cpp` | Transport 全接口实现 |
| `tent/src/transport/memfabric/CMakeLists.txt` | STATIC 库构建 |
| `tent/src/transport/CMakeLists.txt` | `add_subdirectory` + `tent_transport_all` |
| `tent/src/CMakeLists.txt` | `tent_link_group` 包含 memfabric |
| `tent/include/tent/common/types.h` | `TransportType::MEMFABRIC` 枚举 |
| `tent/src/runtime/transport_loader.cpp` | `USE_MEMFABRIC` → `loadTransports` |
| `tent/src/runtime/transfer_engine_impl.cpp` | `getSupportedTransports` + MEMFABRIC |
| `mooncake-common/common.cmake` | `USE_MEMFABRIC` 选项 + ACL 路径 + 编译宏 |
| `mooncake-store/src/real_client.cpp` | 4 处 `#ifdef` 守卫 |
| `mooncake-store/src/common/client_buffer_allocation.cpp` | 2 处 `#if` 守卫 |
| `mooncake-transfer-engine/src/transport/ascend_transport/ascend_allocator.cpp` | 3 处 `ContextManager` 守卫 |
| `mooncake-transfer-engine/src/CMakeLists.txt` | `USE_MEMFABRIC` 块: `ascend_allocator.cpp` + `ascendcl` |
| `scripts/build_memfabric.sh` | 一键构建脚本 |
| `scripts/deploy_memfabric_container.sh` | 容器内部部署脚本 |
