#ifndef DPLL_CTRL_H
#define DPLL_CTRL_H

#include "xil_types.h"

#define DPLL_BASE_ADDRESS 0x40003000U
#define DPLL_DEVICE_ID 0x44504c31U
#define DPLL_SAMPLE_RATE_HZ 65000000U
#define DPLL_REG_ID 0x00U
#define DPLL_REG_CONTROL 0x04U
#define DPLL_REG_CENTER 0x08U
#define DPLL_REG_HYSTERESIS 0x0cU
#define DPLL_REG_MIN_PERIOD 0x10U
#define DPLL_REG_MAX_PERIOD 0x14U
#define DPLL_REG_KP_SHIFT 0x18U
#define DPLL_REG_KI_SHIFT 0x1cU
#define DPLL_REG_COMMIT 0x20U
#define DPLL_REG_STATUS 0x24U
#define DPLL_REG_SNAPSHOT 0x28U
#define DPLL_REG_GENERATION 0x2cU
#define DPLL_REG_RESULT 0x40U
#define DPLL_STATUS_BUSY (1U << 0)
#define DPLL_STATUS_LIVE (1U << 1)
#define DPLL_STATUS_SNAPSHOT (1U << 2)
#define DPLL_STATUS_LOCKED (1U << 3)
/* Software status flags occupy the upper bits of the SCP1 header flags. */
#define DPLL_STATUS_WRONG_ID (1U << 31)
#define DPLL_STATUS_WAIT_COMMIT (1U << 30)

enum {
    DPLL_RESULT_SEQUENCE = 0, DPLL_RESULT_GENERATION = 1,
    DPLL_RESULT_FLAGS = 2, DPLL_RESULT_FTW = 3,
    DPLL_RESULT_PERIOD_Q16 = 4, DPLL_RESULT_PHASE_ERROR = 5,
    DPLL_RESULT_INTEGRAL_FTW = 6, DPLL_RESULT_LOCK_COUNT = 7,
    DPLL_RESULT_EDGE_COUNT = 8, DPLL_RESULT_CENTER = 9,
    DPLL_RESULT_PHASE = 10, DPLL_RESULT_WORDS = 16
};
#define DPLL_FLAG_ENABLED (1U << 0)
#define DPLL_FLAG_ACTIVE (1U << 1)
#define DPLL_FLAG_LOCKED (1U << 2)
#define DPLL_FLAG_CLIPPED (1U << 3)
#define DPLL_FLAG_TIMEOUT (1U << 4)
#define DPLL_FLAG_BAD_PERIOD (1U << 5)
#define DPLL_FLAG_VALID_GAP (1U << 6)

/* Initializes software only. MMIO starts on the first PLL command, and requires
 * the matching DPL1 bitstream at DPLL_BASE_ADDRESS. Checking an ID cannot make
 * an absent AXI slave safe to read. */
void DpllCtrl_Init(UINTPTR base_address);
/* Parse and queue one complete ASCII line. No MMIO in this function.
 * Return 0 for another command, 1 if accepted, or -1 if malformed. */
int DpllCtrl_Command(const char *line);
/* Keep PI disabled while PLL is requested, committing or applied. */
int DpllCtrl_IsRequestedOrActive(void);
/* Nonblocking main-loop service. Commit waits for the DDS manual queue to
 * drain, so W/A/E settings sent before PLL1 reach the DAC first. */
void DpllCtrl_Poll(int dds_idle);
/* A frozen raw 16-word PL result and the AXI/software status flags. */
int DpllCtrl_TakeReport(u32 words[DPLL_RESULT_WORDS], u32 *status);

#endif
