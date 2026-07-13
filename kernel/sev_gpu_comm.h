/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_comm.h — RPC framing: NV escape/NVOS offsets, per-control forwarding
 * policy, nested-buffer layout. Shared by the client RM interceptor
 * (sev_gpu_client_rm.c) and the manager RPC service (sev_gpu_main.c). Function
 * bodies currently remain in sev_gpu_main.c; this header exposes the ABI so the
 * client-RM file can build. (Full comm .c split is a later step.)
 */
#ifndef SEV_GPU_COMM_H
#define SEV_GPU_COMM_H

#include <linux/types.h>
#include <linux/mutex.h>
#include "sev_gpu_rpc.h"

enum rpc_ctrl_disp {
	RPC_CTRL_FLAT = 0,	/* no embedded pointers: flat/FINN path        */
	RPC_CTRL_LEVEL2,	/* pParams embeds pointers: explicit level-2   */
	RPC_CTRL_LOCAL,		/* answered locally by the client module (hook)*/
};


#define RPC_NV_ESC_RM_CONTROL	0x2Au
#define RPC_NV_ESC_RM_ALLOC	0x2Bu

#define RPC_NVOS54_PARAMS_OFF		16u	/* NVOS54_PARAMETERS.params      */
#define RPC_NVOS54_PARAMSSIZE_OFF	24u	/* NVOS54_PARAMETERS.paramsSize  */
#define RPC_NVOS54_SIZE			32u
#define RPC_NVOS54_FLAGS_OFF		12u	/* NVOS54_PARAMETERS.flags       */
#define RPC_NVOS54_CMD_OFF		 8u	/* NVOS54_PARAMETERS.cmd         */
#define RPC_NVOS54_FLAGS_FINN_SERIALIZED 0x4u	/* params is a FINN blob         */

/*
 * NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION (0x101): the pParams struct embeds
 * three output string pointers. Offsets within the params struct (LP64, 8-byte
 * aligned NvP64 fields):
 *   [0]  sizeOfStrings          NvU32   length of each string buffer
 *   [8]  pDriverVersionBuffer   NvP64   output: driver version string
 *   [16] pVersionBuffer         NvP64   output: version string
 *   [24] pTitleBuffer           NvP64   output: title string
 */
#define NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION 0x101u
#define BVPAR_SOS_OFF    0u   /* sizeOfStrings field */
#define BVPAR_PDRVVER_OFF 8u  /* pDriverVersionBuffer field */
#define BVPAR_PVER_OFF   16u  /* pVersionBuffer field */
#define BVPAR_PTITLE_OFF 24u  /* pTitleBuffer field */

#define RPC_NVOS21_PALLOC_OFF		16u	/* NVOS21/64.pAllocParms (same)  */
#define RPC_NVOS21_PARAMSSIZE_OFF	24u	/* NVOS21_PARAMETERS.paramsSize  */
#define RPC_NVOS21_SIZE			32u

#define RPC_NVOS64_PRIGHTS_OFF		24u	/* NVOS64_PARAMETERS.pRightsRequested */
#define RPC_NVOS64_PARAMSSIZE_OFF	32u	/* NVOS64_PARAMETERS.paramsSize  */
#define RPC_NVOS64_SIZE			48u
#define RPC_RS_ACCESS_MASK_SIZE		4u	/* sizeof(RS_ACCESS_MASK)        */

#define RPC_SIZE_FIXED 0xffffffffu
struct rpc_embedded_field {
	u32 pptr_off;
	u32 size_off;
	u32 fixed_size;
	u32 dir;
	u32 elem_size;
};

struct rpc_ctrl_policy {
	u32 ctrl_cmd;
	u8  disp;
	u8  n_fields;
	const struct rpc_embedded_field *fields;
};

#define RPC_STATE_OFF	offsetof(sev_gpu_rpc_slot_t, state)
#define RPC_TIMEOUT_MS	5000		/* client: max wait for a reply           */

const struct rpc_ctrl_policy *rpc_ctrl_policy(u32 ctrl_cmd);

/* client: one in-flight RM call at a time (defined in sev_gpu_main.c). */
extern struct mutex rpc_client_lock;
extern u32 rpc_client_seq;

#endif /* SEV_GPU_COMM_H */
