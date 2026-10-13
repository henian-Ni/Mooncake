// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <acl/acl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tent/common/config.h"
#include "tent/runtime/control_plane.h"
#include "tent/transport/memfabric/memfabric_transport.h"
#include "tent/transport/memfabric/memfabric_under_api.h"

namespace {

thread_local int g_device_id = 0;
thread_local aclrtContext g_context = reinterpret_cast<aclrtContext>(0x10000);
int g_device_count = 1;

void ResetAcl() {
    g_device_id = 0;
    g_device_count = 1;
    g_context = reinterpret_cast<aclrtContext>(0x10000);
}

}  // namespace

extern "C" {

aclError aclrtGetDeviceCount(uint32_t* count) {
    *count = static_cast<uint32_t>(g_device_count);
    return ACL_ERROR_NONE;
}

aclError aclrtGetDevice(int* deviceId) {
    *deviceId = g_device_id;
    return ACL_ERROR_NONE;
}

aclError aclrtSetDevice(int deviceId) {
    if (deviceId >= g_device_count) return 1;
    g_device_id = deviceId;
    return ACL_ERROR_NONE;
}

aclError aclrtGetCurrentContext(aclrtContext* context) {
    *context = g_context;
    return ACL_ERROR_NONE;
}

aclError aclrtSetCurrentContext(aclrtContext context) {
    g_context = context;
    return ACL_ERROR_NONE;
}

aclError aclrtPointerGetAttributes(const void* ptr,
                                   aclrtPtrAttributes* attributes) {
    (void)ptr;
    attributes->location.id = 0;
    attributes->location.type = ACL_MEM_LOCATION_TYPE_HOST;
    return ACL_ERROR_NONE;
}

aclError aclrtMalloc(void** devPtr, size_t size, aclrtMemMallocPolicy policy) {
    (void)policy;
    *devPtr = std::malloc(size);
    return *devPtr != nullptr ? ACL_ERROR_NONE : 1;
}

aclError aclrtFree(void* devPtr) {
    std::free(devPtr);
    return ACL_ERROR_NONE;
}

aclError aclrtMallocHost(void** hostPtr, size_t size) {
    *hostPtr = std::malloc(size);
    return *hostPtr != nullptr ? ACL_ERROR_NONE : 1;
}

aclError aclrtFreeHost(void* hostPtr) {
    std::free(hostPtr);
    return ACL_ERROR_NONE;
}

}  // extern "C"

namespace mooncake {
namespace tent {

struct MockMemFabricState {
    std::mutex mu;
    int init_calls = 0;
    int uninit_calls = 0;
    int config_init_calls = 0;
    int create_calls = 0;
    int destroy_calls = 0;
    int get_rpc_port_calls = 0;
    int batch_copy_calls = 0;
    std::vector<smem_trans_opcode_t> batch_opcodes;
    int status_query_calls = 0;
    int register_mem_calls = 0;
    int deregister_mem_calls = 0;
    int32_t deregister_ret = 0;
    int set_log_level_calls = 0;
    std::atomic<uint64_t> next_batch_id{1};
    int32_t status_query_result = SMEM_TRANS_BATCH_DONE;
    int32_t batch_copy_ret = 0;
    int32_t get_rpc_port_ret = 0;
    uint16_t rpc_port = 12345;
    std::string create_url;
    std::string create_nic;
    bool create_fail = false;
};

static std::shared_ptr<MockMemFabricState> g_state;

int32_t mockSmemTransInit(uint32_t, uint32_t) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->init_calls++;
    return 0;
}

void mockSmemTransUninit(uint32_t) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->uninit_calls++;
}

int32_t mockSmemTransConfigInit(smem_trans_config_t* config) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->config_init_calls++;
    std::memset(config, 0, sizeof(*config));
    return 0;
}

smem_trans_t mockSmemTransCreate(const smem_trans_config_t* config, uint32_t) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->create_calls++;
    g_state->create_url = config->url;
    g_state->create_nic = config->nic;
    if (g_state->create_fail) return nullptr;
    return reinterpret_cast<smem_trans_t>(0x2000);
}

void mockSmemTransDestroy(smem_trans_t, uint32_t) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->destroy_calls++;
}

int32_t mockSmemTransGetRpcPort(smem_trans_t, uint16_t* port) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->get_rpc_port_calls++;
    *port = g_state->rpc_port;
    return g_state->get_rpc_port_ret;
}

int32_t mockSmemTransRegisterMem(smem_trans_t, void*, size_t, uint32_t) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->register_mem_calls++;
    return 0;
}

int32_t mockSmemTransBatchRegisterMem(smem_trans_t, void*[], size_t[], uint32_t,
                                      uint32_t) {
    return 0;
}

int32_t mockSmemTransDeregisterMem(smem_trans_t, void*) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->deregister_mem_calls++;
    return g_state->deregister_ret;
}

int32_t mockSmemTransBatchCopy(smem_trans_t, smem_trans_batch_params_t* params,
                               uint64_t* batchId) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->batch_copy_calls++;
    if (params) g_state->batch_opcodes.push_back(params->opcode);
    if (g_state->batch_copy_ret != 0) return g_state->batch_copy_ret;
    *batchId = g_state->next_batch_id.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int32_t mockSmemTransBatchStatusQuery(smem_trans_t, uint64_t, uint32_t) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->status_query_calls++;
    return g_state->status_query_result;
}

int32_t mockSmemTransSetPeerDownCallback(smem_trans_t,
                                         smem_trans_peer_down_callback_t,
                                         void*) {
    return 0;
}

int32_t mockSmemSetLogLevel(int) {
    std::lock_guard<std::mutex> lock(g_state->mu);
    g_state->set_log_level_calls++;
    return 0;
}

Request MakeWrite(void* src, uint64_t dest, size_t len) {
    Request request{};
    request.opcode = Request::WRITE;
    request.source = src;
    request.target_id = LOCAL_SEGMENT_ID;
    request.target_offset = dest;
    request.length = len;
    return request;
}

Request MakeRead(void* src, uint64_t dest, size_t len) {
    Request request{};
    request.opcode = Request::READ;
    request.source = src;
    request.target_id = LOCAL_SEGMENT_ID;
    request.target_offset = dest;
    request.length = len;
    return request;
}

class MemFabricTransportTest : public ::testing::Test {
   protected:
    void SetUp() override {
        ResetAcl();
        g_state = std::make_shared<MockMemFabricState>();
        state_ = g_state;
        InstallMockApi();
        metadata_ = std::make_shared<ControlService>("p2p", "", nullptr);
        uint16_t port = 0;
        ASSERT_TRUE(metadata_->start(port).ok());
        name_ = "127.0.0.1:" + std::to_string(static_cast<unsigned>(port));
        ASSERT_TRUE(SetRpcAddress(name_).ok());
        transport_ = std::make_unique<MemFabricTransport>();
    }

    void TearDown() override {
        if (batch_ != nullptr) {
            (void)transport_->freeSubBatch(batch_);
            batch_ = nullptr;
        }
        if (transport_) {
            (void)transport_->uninstall();
        }
        ResetMockApi();
        g_state.reset();
        unsetenv("MF_PROTOCOL");
    }

    Status Install() {
        setenv("MF_PROTOCOL", "device_rdma", 1);
        auto conf = std::make_shared<Config>();
        return transport_->install(name_, metadata_, nullptr, conf);
    }

    Status SetRpcAddress(const std::string& address) {
        return metadata_->segmentManager().updateLocal(
            [&](SegmentDesc& segment) -> Status {
                segment.rpc_server_addr = address;
                return Status::OK();
            });
    }

    std::string PublishedEndpoint() const {
        auto segment = metadata_->segmentManager().getLocal();
        const auto& attrs =
            std::get<MemorySegmentDesc>(segment->detail).device_attrs;
        auto it = attrs.find("memfabric_unique_id");
        return it == attrs.end() ? "" : it->second;
    }

    void InstallMockApi();
    void ResetMockApi();

    std::shared_ptr<MockMemFabricState> state_;
    std::shared_ptr<ControlService> metadata_;
    std::unique_ptr<MemFabricTransport> transport_;
    Transport::SubBatchRef batch_{nullptr};
    std::string name_;
};

void MemFabricTransportTest::InstallMockApi() {
    MemFabricUnderApi::gLoaded = true;
    MemFabricUnderApi::gLibHandle = nullptr;
    MemFabricUnderApi::pSmemTransInit = &mockSmemTransInit;
    MemFabricUnderApi::pSmemTransUninit = &mockSmemTransUninit;
    MemFabricUnderApi::pSmemTransConfigInit = &mockSmemTransConfigInit;
    MemFabricUnderApi::pSmemTransCreate = &mockSmemTransCreate;
    MemFabricUnderApi::pSmemTransDestroy = &mockSmemTransDestroy;
    MemFabricUnderApi::pSmemTransGetRpcPort = &mockSmemTransGetRpcPort;
    MemFabricUnderApi::pSmemTransRegisterMem = &mockSmemTransRegisterMem;
    MemFabricUnderApi::pSmemTransBatchRegisterMem =
        &mockSmemTransBatchRegisterMem;
    MemFabricUnderApi::pSmemTransDeregisterMem = &mockSmemTransDeregisterMem;
    MemFabricUnderApi::pSmemTransBatchCopy = &mockSmemTransBatchCopy;
    MemFabricUnderApi::pSmemTransBatchStatusQuery =
        &mockSmemTransBatchStatusQuery;
    MemFabricUnderApi::pSmemTransSetPeerDownCallback =
        &mockSmemTransSetPeerDownCallback;
    MemFabricUnderApi::pSmemSetLogLevel = &mockSmemSetLogLevel;
}

void MemFabricTransportTest::ResetMockApi() {
    MemFabricUnderApi::gLoaded = false;
    MemFabricUnderApi::gLibHandle = nullptr;
    MemFabricUnderApi::pSmemTransInit = nullptr;
    MemFabricUnderApi::pSmemTransUninit = nullptr;
    MemFabricUnderApi::pSmemTransConfigInit = nullptr;
    MemFabricUnderApi::pSmemTransCreate = nullptr;
    MemFabricUnderApi::pSmemTransDestroy = nullptr;
    MemFabricUnderApi::pSmemTransGetRpcPort = nullptr;
    MemFabricUnderApi::pSmemTransRegisterMem = nullptr;
    MemFabricUnderApi::pSmemTransBatchRegisterMem = nullptr;
    MemFabricUnderApi::pSmemTransDeregisterMem = nullptr;
    MemFabricUnderApi::pSmemTransBatchCopy = nullptr;
    MemFabricUnderApi::pSmemTransBatchStatusQuery = nullptr;
    MemFabricUnderApi::pSmemTransSetPeerDownCallback = nullptr;
    MemFabricUnderApi::pSmemSetLogLevel = nullptr;
}

TEST_F(MemFabricTransportTest, InstallSucceeds) {
    ASSERT_TRUE(Install().ok());
    EXPECT_EQ(state_->init_calls, 1);
    EXPECT_EQ(state_->create_calls, 1);
    EXPECT_EQ(state_->get_rpc_port_calls, 1);
}

TEST_F(MemFabricTransportTest, InstallFailsWhenCreateReturnsNull) {
    g_state->create_fail = true;
    auto status = Install();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(state_->create_calls, 1);
    EXPECT_TRUE(PublishedEndpoint().empty());
}

TEST_F(MemFabricTransportTest, InstallUsesRpcAddressForOpaqueSegmentName) {
    name_ = "segment_noname_memfabric_test";
    ASSERT_TRUE(SetRpcAddress("192.0.2.10:9000").ok());

    ASSERT_TRUE(Install().ok());

    EXPECT_EQ(state_->create_url, "192.0.2.10:0");
    EXPECT_EQ(state_->create_nic, "tcp://192.0.2.10:9000");
    EXPECT_EQ(PublishedEndpoint(), "192.0.2.10:12345");
    EXPECT_EQ(name_, "segment_noname_memfabric_test");
}

TEST_F(MemFabricTransportTest, InstallIgnoresAddressLikeSegmentName) {
    name_ = "198.51.100.20:8000";
    ASSERT_TRUE(SetRpcAddress("192.0.2.10:9000").ok());

    ASSERT_TRUE(Install().ok());

    EXPECT_EQ(state_->create_url, "192.0.2.10:0");
    EXPECT_EQ(state_->create_nic, "tcp://192.0.2.10:9000");
    EXPECT_EQ(PublishedEndpoint(), "192.0.2.10:12345");
}

TEST_F(MemFabricTransportTest, InstallFormatsIpv6RpcEndpoint) {
    name_ = "segment_noname_ipv6";
    ASSERT_TRUE(SetRpcAddress("[2001:db8::10]:9000").ok());

    ASSERT_TRUE(Install().ok());

    EXPECT_EQ(state_->create_url, "[2001:db8::10]:0");
    EXPECT_EQ(state_->create_nic, "tcp://[2001:db8::10]:9000");
    EXPECT_EQ(PublishedEndpoint(), "[2001:db8::10]:12345");
}

TEST_F(MemFabricTransportTest, InstallRejectsInvalidRpcAddress) {
    for (const auto& address :
         {"", ":9000", "127.0.0.1:bad", "127.0.0.1:0", "0.0.0.0:9000",
          "[::]:9000", "[2001::db8::1]:9000"}) {
        SCOPED_TRACE(address);
        ASSERT_TRUE(SetRpcAddress(address).ok());
        ASSERT_FALSE(Install().ok());
        EXPECT_EQ(state_->init_calls, 0);
        EXPECT_EQ(state_->create_calls, 0);
        EXPECT_TRUE(PublishedEndpoint().empty());
    }
}

TEST_F(MemFabricTransportTest, InstallRejectsOversizedRpcAddress) {
    ASSERT_TRUE(SetRpcAddress(std::string(64, 'a') + ":9000").ok());

    EXPECT_FALSE(Install().ok());
    EXPECT_EQ(state_->init_calls, 0);
    EXPECT_EQ(state_->create_calls, 0);
    EXPECT_TRUE(PublishedEndpoint().empty());
}

TEST_F(MemFabricTransportTest, InstallDoesNotPublishWhenPortQueryFails) {
    state_->get_rpc_port_ret = -1;

    EXPECT_FALSE(Install().ok());
    EXPECT_EQ(state_->get_rpc_port_calls, 1);
    EXPECT_EQ(state_->destroy_calls, 1);
    EXPECT_EQ(state_->uninit_calls, 1);
    EXPECT_TRUE(PublishedEndpoint().empty());
}

TEST_F(MemFabricTransportTest, InstallDoesNotPublishZeroPort) {
    state_->rpc_port = 0;

    EXPECT_FALSE(Install().ok());
    EXPECT_EQ(state_->get_rpc_port_calls, 1);
    EXPECT_EQ(state_->destroy_calls, 1);
    EXPECT_EQ(state_->uninit_calls, 1);
    EXPECT_TRUE(PublishedEndpoint().empty());
}

TEST_F(MemFabricTransportTest, MixedWriteAndReadToSamePeerSubmitsTwoBatches) {
    ASSERT_TRUE(Install().ok());
    ASSERT_TRUE(transport_->allocateSubBatch(batch_, 4).ok());

    char src[32]{};
    std::vector<Request> requests = {MakeWrite(src, 0x1000, 32),
                                     MakeRead(src, 0x2000, 32)};
    ASSERT_TRUE(transport_->submitTransferTasks(batch_, requests).ok());

    EXPECT_EQ(state_->batch_copy_calls, 2);
    ASSERT_EQ(state_->batch_opcodes.size(), 2u);
    EXPECT_EQ(state_->batch_opcodes[0], SMEM_TRANS_OP_WRITE);
    EXPECT_EQ(state_->batch_opcodes[1], SMEM_TRANS_OP_READ);

    auto* mf_batch = dynamic_cast<MemFabricSubBatch*>(batch_);
    ASSERT_NE(mf_batch, nullptr);
    EXPECT_EQ(mf_batch->pending_batch_count, 2u);
}

TEST_F(MemFabricTransportTest, SubmitAndGetStatusCompleted) {
    ASSERT_TRUE(Install().ok());
    ASSERT_TRUE(transport_->allocateSubBatch(batch_, 128).ok());

    char src[64]{};
    std::vector<Request> requests;
    for (int i = 0; i < 4; ++i) {
        requests.push_back(MakeWrite(src, 0x1000 + i * 64, 64));
    }
    ASSERT_TRUE(transport_->submitTransferTasks(batch_, requests).ok());
    EXPECT_EQ(state_->batch_copy_calls, 1);

    auto* mf_batch = dynamic_cast<MemFabricSubBatch*>(batch_);
    ASSERT_NE(mf_batch, nullptr);
    EXPECT_EQ(mf_batch->pending_batch_count, 1u);

    g_state->status_query_result = SMEM_TRANS_BATCH_DONE;

    TransferStatus status0{};
    ASSERT_TRUE(transport_->getTransferStatus(batch_, 0, status0).ok());
    EXPECT_EQ(status0.s, TransferStatusEnum::COMPLETED);
    EXPECT_EQ(status0.transferred_bytes, 64u);
    EXPECT_EQ(state_->status_query_calls, 1);

    TransferStatus status1{};
    ASSERT_TRUE(transport_->getTransferStatus(batch_, 1, status1).ok());
    EXPECT_EQ(status1.s, TransferStatusEnum::COMPLETED);
    EXPECT_EQ(status1.transferred_bytes, 64u);
    EXPECT_EQ(state_->status_query_calls, 1);

    EXPECT_EQ(mf_batch->pending_batch_count, 0u);
}

TEST_F(MemFabricTransportTest, SubmitAndGetStatusPending) {
    ASSERT_TRUE(Install().ok());
    ASSERT_TRUE(transport_->allocateSubBatch(batch_, 4).ok());

    char src[32]{};
    ASSERT_TRUE(
        transport_->submitTransferTasks(batch_, {MakeWrite(src, 0x1000, 32)})
            .ok());

    g_state->status_query_result = SMEM_TRANS_BATCH_PENDING;

    TransferStatus status{};
    ASSERT_TRUE(transport_->getTransferStatus(batch_, 0, status).ok());
    EXPECT_EQ(status.s, TransferStatusEnum::PENDING);

    auto* mf_batch = dynamic_cast<MemFabricSubBatch*>(batch_);
    EXPECT_EQ(mf_batch->pending_batch_count, 1u);
}

TEST_F(MemFabricTransportTest, InvalidDestinationLeavesEmptyBatchUnchanged) {
    ASSERT_TRUE(Install().ok());
    ASSERT_TRUE(transport_->allocateSubBatch(batch_, 4).ok());

    char src[32]{};
    auto valid = MakeWrite(src, 0x1000, sizeof(src));
    auto invalid = MakeWrite(src, 0x2000, sizeof(src));
    // The fixture has not opened any remote segments.
    invalid.target_id = LOCAL_SEGMENT_ID + 1;

    EXPECT_FALSE(
        transport_->submitTransferTasks(batch_, {valid, invalid}).ok());

    auto* mf_batch = dynamic_cast<MemFabricSubBatch*>(batch_);
    ASSERT_NE(mf_batch, nullptr);
    EXPECT_EQ(mf_batch->size(), 0u);
    EXPECT_EQ(mf_batch->pending_batch_count, 0u);
    EXPECT_TRUE(mf_batch->batch_status_cache.empty());
    EXPECT_EQ(state_->batch_copy_calls, 0);
    EXPECT_EQ(state_->status_query_calls, 0);
}

TEST_F(MemFabricTransportTest, InvalidDestinationPreservesExistingTasks) {
    ASSERT_TRUE(Install().ok());
    ASSERT_TRUE(transport_->allocateSubBatch(batch_, 8).ok());

    char src[64]{};
    ASSERT_TRUE(
        transport_->submitTransferTasks(batch_, {MakeWrite(src, 0x1000, 32)})
            .ok());
    TransferStatus status{};
    ASSERT_TRUE(transport_->getTransferStatus(batch_, 0, status).ok());
    ASSERT_EQ(status.s, TransferStatusEnum::COMPLETED);

    ASSERT_TRUE(
        transport_->submitTransferTasks(batch_, {MakeWrite(src, 0x2000, 16)})
            .ok());
    state_->status_query_result = SMEM_TRANS_BATCH_PENDING;
    ASSERT_TRUE(transport_->getTransferStatus(batch_, 1, status).ok());
    ASSERT_EQ(status.s, TransferStatusEnum::PENDING);

    auto* mf_batch = dynamic_cast<MemFabricSubBatch*>(batch_);
    ASSERT_NE(mf_batch, nullptr);
    const auto original_tasks = mf_batch->task_list;
    const auto original_cache = mf_batch->batch_status_cache;
    const auto original_pending = mf_batch->pending_batch_count;
    const auto original_copy_calls = state_->batch_copy_calls;
    const auto original_query_calls = state_->status_query_calls;

    auto valid = MakeWrite(src, 0x3000, 8);
    auto invalid = MakeWrite(src, 0x4000, 8);
    invalid.target_id = LOCAL_SEGMENT_ID + 1;
    EXPECT_FALSE(
        transport_->submitTransferTasks(batch_, {valid, invalid}).ok());

    ASSERT_EQ(mf_batch->size(), original_tasks.size());
    EXPECT_EQ(mf_batch->pending_batch_count, original_pending);
    EXPECT_EQ(mf_batch->batch_status_cache, original_cache);
    EXPECT_EQ(state_->batch_copy_calls, original_copy_calls);
    EXPECT_EQ(state_->status_query_calls, original_query_calls);
    for (size_t i = 0; i < original_tasks.size(); ++i) {
        const auto& before = original_tasks[i];
        const auto& after = mf_batch->task_list[i];
        EXPECT_EQ(after.request.opcode, before.request.opcode);
        EXPECT_EQ(after.request.source, before.request.source);
        EXPECT_EQ(after.request.target_id, before.request.target_id);
        EXPECT_EQ(after.request.target_offset, before.request.target_offset);
        EXPECT_EQ(after.request.length, before.request.length);
        EXPECT_EQ(after.batch_id, before.batch_id);
        EXPECT_EQ(after.status_word, before.status_word);
        EXPECT_EQ(after.length, before.length);
        EXPECT_EQ(after.transferred_bytes, before.transferred_bytes);
    }

    // A subsequent valid append must use the next slot without a gap.
    ASSERT_TRUE(transport_->submitTransferTasks(batch_, {valid}).ok());
    ASSERT_EQ(mf_batch->size(), original_tasks.size() + 1);
    EXPECT_EQ(state_->batch_copy_calls, original_copy_calls + 1);
    EXPECT_EQ(mf_batch->pending_batch_count, original_pending + 1);

    state_->status_query_result = SMEM_TRANS_BATCH_DONE;
    const size_t expected_bytes[] = {32, 16, 8};
    for (int task_id = 0; task_id < 3; ++task_id) {
        ASSERT_TRUE(
            transport_->getTransferStatus(batch_, task_id, status).ok());
        EXPECT_EQ(status.s, TransferStatusEnum::COMPLETED);
        EXPECT_EQ(status.transferred_bytes, expected_bytes[task_id]);
    }
    EXPECT_EQ(mf_batch->pending_batch_count, 0u);
}

TEST_F(MemFabricTransportTest, FailedBatchPropagatesToAllTasks) {
    ASSERT_TRUE(Install().ok());
    ASSERT_TRUE(transport_->allocateSubBatch(batch_, 4).ok());

    char src[32]{};
    std::vector<Request> requests = {MakeWrite(src, 0x1000, 32),
                                     MakeWrite(src, 0x2000, 32)};
    ASSERT_TRUE(transport_->submitTransferTasks(batch_, requests).ok());

    g_state->status_query_result = -2000;

    TransferStatus status0{};
    ASSERT_TRUE(transport_->getTransferStatus(batch_, 0, status0).ok());
    EXPECT_EQ(status0.s, TransferStatusEnum::FAILED);

    TransferStatus status1{};
    ASSERT_TRUE(transport_->getTransferStatus(batch_, 1, status1).ok());
    EXPECT_EQ(status1.s, TransferStatusEnum::FAILED);
    EXPECT_EQ(state_->status_query_calls, 1);

    auto* mf_batch = dynamic_cast<MemFabricSubBatch*>(batch_);
    EXPECT_EQ(mf_batch->pending_batch_count, 0u);
}

TEST_F(MemFabricTransportTest, PendingBatchDestructorWarning) {
    ASSERT_TRUE(Install().ok());
    ASSERT_TRUE(transport_->allocateSubBatch(batch_, 4).ok());

    char src[32]{};
    ASSERT_TRUE(
        transport_->submitTransferTasks(batch_, {MakeWrite(src, 0x1000, 32)})
            .ok());

    auto* mf_batch = dynamic_cast<MemFabricSubBatch*>(batch_);
    EXPECT_EQ(mf_batch->pending_batch_count, 1u);

    transport_->freeSubBatch(batch_);
    batch_ = nullptr;
}

TEST_F(MemFabricTransportTest, QuiesceDrainsInflightBatches) {
    ASSERT_TRUE(Install().ok());
    ASSERT_TRUE(transport_->allocateSubBatch(batch_, 4).ok());

    char src[32]{};
    ASSERT_TRUE(
        transport_->submitTransferTasks(batch_, {MakeWrite(src, 0x1000, 32)})
            .ok());
    EXPECT_EQ(transport_->inflight_.load(), 1);

    g_state->status_query_result = SMEM_TRANS_BATCH_DONE;
    TransferStatus status{};
    ASSERT_TRUE(transport_->getTransferStatus(batch_, 0, status).ok());
    EXPECT_EQ(status.s, TransferStatusEnum::COMPLETED);
    EXPECT_EQ(transport_->inflight_.load(), 0);

    ASSERT_TRUE(transport_->quiesce().ok());
}

TEST_F(MemFabricTransportTest, QuiesceTimeoutWithPendingBatches) {
    ASSERT_TRUE(Install().ok());
    transport_->transfer_timeout_ms_ = 100;
    ASSERT_TRUE(transport_->allocateSubBatch(batch_, 4).ok());

    char src[32]{};
    ASSERT_TRUE(
        transport_->submitTransferTasks(batch_, {MakeWrite(src, 0x1000, 32)})
            .ok());
    EXPECT_EQ(transport_->inflight_.load(), 1);

    g_state->status_query_result = SMEM_TRANS_BATCH_PENDING;
    TransferStatus status{};
    ASSERT_TRUE(transport_->getTransferStatus(batch_, 0, status).ok());
    EXPECT_EQ(status.s, TransferStatusEnum::PENDING);

    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(transport_->quiesce().ok());
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    EXPECT_GE(elapsed, 90);
    EXPECT_EQ(transport_->inflight_.load(), 1);
}

TEST_F(MemFabricTransportTest, AddRemoveMemoryBuffer) {
    ASSERT_TRUE(Install().ok());

    BufferDesc desc{};
    desc.addr = 0x10000;
    desc.length = 4096;
    MemoryOptions options;
    ASSERT_TRUE(transport_->addMemoryBuffer(desc, options).ok());
    EXPECT_EQ(state_->register_mem_calls, 1);

    bool found = false;
    for (auto t : desc.transports) {
        if (t == TransportType::MEMFABRIC) found = true;
    }
    EXPECT_TRUE(found);

    ASSERT_TRUE(transport_->removeMemoryBuffer(desc).ok());
    EXPECT_EQ(state_->deregister_mem_calls, 1);
}

TEST_F(MemFabricTransportTest, DeregisterFailurePropagatesError) {
    ASSERT_TRUE(Install().ok());

    BufferDesc desc{};
    desc.addr = 0x10000;
    desc.length = 4096;
    MemoryOptions options;
    ASSERT_TRUE(transport_->addMemoryBuffer(desc, options).ok());

    g_state->deregister_ret = -9;
    auto status = transport_->removeMemoryBuffer(desc);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(state_->deregister_mem_calls, 1);

    g_state->deregister_ret = 0;
    ASSERT_TRUE(transport_->removeMemoryBuffer(desc).ok());
    EXPECT_EQ(state_->deregister_mem_calls, 2);
}

TEST_F(MemFabricTransportTest, AddMemoryBufferFailsBeforeInstall) {
    BufferDesc desc{};
    desc.addr = 0x10000;
    desc.length = 4096;
    MemoryOptions options;
    auto status = transport_->addMemoryBuffer(desc, options);
    EXPECT_FALSE(status.ok());
}

TEST_F(MemFabricTransportTest, SubmitFailsWhenBatchCopyFails) {
    ASSERT_TRUE(Install().ok());
    ASSERT_TRUE(transport_->allocateSubBatch(batch_, 4).ok());

    g_state->batch_copy_ret = -1;

    char src[32]{};
    ASSERT_TRUE(
        transport_->submitTransferTasks(batch_, {MakeWrite(src, 0x1000, 32)})
            .ok());

    auto* mf_batch = dynamic_cast<MemFabricSubBatch*>(batch_);
    ASSERT_EQ(mf_batch->task_list.size(), 1u);
    EXPECT_EQ(mf_batch->task_list[0].status_word, TransferStatusEnum::FAILED);
    EXPECT_EQ(mf_batch->pending_batch_count, 0u);
}

}  // namespace tent
}  // namespace mooncake
