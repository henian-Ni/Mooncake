#ifndef MEMFABRIC_TYPES_H_
#define MEMFABRIC_TYPES_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMEM_TLS_PATH_SIZE 256

typedef enum {
    SMEMB_DATA_OP_SDMA = 1U << 0,
    SMEMB_DATA_OP_HOST_RDMA = 1U << 1,
    SMEMB_DATA_OP_HOST_TCP = 1U << 2,
    SMEMB_DATA_OP_DEVICE_RDMA = 1U << 3,
    SMEMB_DATA_OP_HOST_URMA = 1U << 4,
    SMEMB_DATA_OP_HOST_SHM = 1U << 5,
    SMEMB_DATA_OP_DEVICE_URMA = 1U << 6,
    SMEMB_DATA_OP_DEVICE_UBOE = 1U << 7,
    SMEMB_DATA_OP_BUTT
} smem_bm_data_op_type;

typedef struct {
    bool tlsEnable;
    char caPath[SMEM_TLS_PATH_SIZE];
    char crlPath[SMEM_TLS_PATH_SIZE];
    char certPath[SMEM_TLS_PATH_SIZE];
    char keyPath[SMEM_TLS_PATH_SIZE];
    char keyPassPath[SMEM_TLS_PATH_SIZE];
    char packagePath[SMEM_TLS_PATH_SIZE];
    char decrypterLibPath[SMEM_TLS_PATH_SIZE];
} smem_tls_config;

#define SMEM_TRANS_BATCH_DONE (0)
#define SMEM_TRANS_BATCH_PENDING (1)

typedef void *smem_trans_t;

typedef enum {
    SMEM_TRANS_NONE = 0,
    SMEM_TRANS_SENDER,
    SMEM_TRANS_RECEIVER,
    SMEM_TRANS_BOTH,
    SMEM_TRANS_BUTT
} smem_trans_role_t;

typedef struct {
    smem_trans_role_t role;
    uint32_t initTimeout;
    uint32_t deviceId;
    uint32_t flags;
    smem_bm_data_op_type dataOpType;
    char nic[64];
    char url[64];
    smem_tls_config hcomTlsConfig;
} smem_trans_config_t;

typedef enum {
    SMEM_TRANS_OP_READ = 0,
    SMEM_TRANS_OP_WRITE = 1,
} smem_trans_opcode_t;

typedef struct {
    const char *destUrl;
    void **srcList;
    void **destList;
    size_t *sizeList;
    uint32_t batchSize;
    smem_trans_opcode_t opcode;
    void *stream;
    uint32_t flags;
} smem_trans_batch_params_t;

typedef void (*smem_trans_peer_down_callback_t)(const char *peerUniqueId, void *userData);

#ifdef __cplusplus
}
#endif

#endif
