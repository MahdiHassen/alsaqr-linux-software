#ifndef __APMU_ABI_H__
#define __APMU_ABI_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

// Host <-> apmu-os interface. A copy lives in alsaqr-software/linux/apmu; keep them identical.
// Offsets are relative to the ISPM/DSPM base. All fields are 32-bit words.

#define ABI_ISPM_PHYS           0x10427000u
#define ABI_ISPM_SIZE           0x2000u
#define ABI_DSPM_PHYS           0x10429000u

// DSPM header, written by the PE at boot.
#define ABI_STATUS_OFF          0x1000u     // ABI_STATUS_READY once the queues are up
#define ABI_STATUS_READY        0xA9E05003u
#define ABI_EXPORTS_OFF         0x1004u     // address of the abi_export_t table
#define ABI_NEXPORTS_OFF        0x1008u
#define ABI_DYN_ISPM_OFF        0x100Cu     // ISPM offset where the dynamic region starts
// Set by the host; the PE clears it on a cold boot, so the host can tell that
// apmu-os restarted (and its components are gone) without being asked to.
#define ABI_SESSION_OFF         0x1010u
// The last trap: mcause, mepc, mtval. A trap restarts apmu-os cold (see
// crt0.s); the host reads and clears these.
#define ABI_TRAP_OFF            0x1018u

#define ABI_MAX_COMPONENTS      10          // id 0 is the base component

// Queue: word 0 head, word 1 tail (byte offsets into the data area), then data.
// Object: word 0 payload size in bytes, word 1 component id, then the payload.
#define ABI_REQ_Q_OFF           0x1100u
#define ABI_RSP_Q_OFF           0x1400u
#define ABI_Q_SIZE              0x300u
#define ABI_Q_DATA_SIZE         (ABI_Q_SIZE - 8)
#define ABI_Q_WRAP              0xFFFFFFFFu
#define ABI_QUEUE_REQ           0
#define ABI_QUEUE_RSP           1

#define ABI_PRINT_OFF           0x1800u
#define ABI_DYN_DSPM_OFF        0x2000u
#define ABI_DYN_DSPM_SIZE       0xE000u

// Writing 0x80000000 to this counter sets its pending bit and wakes the base component.
#define ABI_DOORBELL_COUNTER    0

// Requests carry the target component id. Components answer on the response
// queue with their own id. Id 0 is the base component: payload word 0 is the op.
#define ABI_BASE_ID             0
#define ABI_OP_INSTALL          1   // op, id, generation, bitmask, event, request, init, exit
#define ABI_OP_UNINSTALL        2   // op, id, generation
// Base response: op, status, component id.
#define ABI_OK                  0
#define ABI_ERR_INVAL           1
#define ABI_ERR_BUSY            2
#define ABI_ERR_NOENT           3

typedef struct {
    char name[24];
    uint32_t addr;
} abi_export_t;

#endif
