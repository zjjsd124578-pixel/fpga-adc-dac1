#include "dpll_ctrl.h"
#include "xil_io.h"
#include "xil_printf.h"

#include <string.h>
#include <ctype.h>

static UINTPTR base;
static u32 desired[7];
static u32 desired_version, applied_version, inflight_version;
static u32 applied_control, inflight_control;
static int probe_pending, device_ready, bad_id, commit_inflight;
static int report_requested, report_ready;
static u32 report_words[DPLL_RESULT_WORDS], report_status;

static int finished(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ';') ++p;
    return *p == '\0';
}

static int unsigned_integer(const char **cursor, u32 *value)
{
    const char *p = *cursor;
    u32 n = 0U, digit;
    int has_digit = 0;
    while (*p == ' ' || *p == '\t') ++p;
    while (*p >= '0' && *p <= '9') {
        digit = (u32)(*p++ - '0');
        if (n > (0xffffffffU - digit) / 10U) return 0;
        n = n * 10U + digit;
        has_digit = 1;
    }
    if (!has_digit) return 0;
    *cursor = p;
    *value = n;
    return 1;
}

void DpllCtrl_Init(UINTPTR base_address)
{
    base = base_address;
    desired[0] = 0U;       /* Disabled, offset binary ADC A. */
    desired[1] = 2048U;
    desired[2] = 8U;
    desired[3] = 650U;     /* Default 100 kHz upper frequency. */
    desired[4] = 65000U;   /* Default 1 kHz lower frequency. */
    desired[5] = 2U;
    desired[6] = 6U;
    desired_version = applied_version = inflight_version = 0U;
    applied_control = inflight_control = 0U;
    probe_pending = device_ready = bad_id = commit_inflight = 0;
    report_requested = report_ready = 0;
    report_status = 0U;
    memset(report_words, 0, sizeof(report_words));
}

int DpllCtrl_Command(const char *line)
{
    char name[24];
    const char *p = line;
    unsigned int length = 0U;
    u32 value, period, replacement[7];
    if (p == NULL) return -1;
    while (*p == ' ' || *p == '\t') ++p;
    while (isalpha((unsigned char)*p)) {
        if (length + 1U >= sizeof(name)) return 0;
        name[length++] = (char)toupper((unsigned char)*p++);
    }
    name[length] = '\0';
    if (strcmp(name, "PLL") && strcmp(name, "PLLSTATUS") &&
        strcmp(name, "PLLCENTER") && strcmp(name, "PLLHYST") &&
        strcmp(name, "PLLMINHZ") && strcmp(name, "PLLMAXHZ") &&
        strcmp(name, "PLLFORMAT") && strcmp(name, "PLLKP") &&
        strcmp(name, "PLLKI")) return 0;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '=' || *p == ':') ++p;
    if (!strcmp(name, "PLLSTATUS")) {
        if (!finished(p)) return -1;
        report_requested = 1;
        if (!device_ready && !bad_id) probe_pending = 1;
        return 1;
    }
    if (bad_id) return -1;
    if (!unsigned_integer(&p, &value) || !finished(p)) return -1;
    memcpy(replacement, desired, sizeof(replacement));
    if (!strcmp(name, "PLL")) {
        if (value > 1U) return -1;
        replacement[0] = (replacement[0] & 2U) | value;
    } else if (!strcmp(name, "PLLCENTER")) {
        if (value > 4095U) return -1;
        replacement[1] = value;
    } else if (!strcmp(name, "PLLHYST")) {
        if (value < 1U || value > 1023U) return -1;
        replacement[2] = value;
    } else if (!strcmp(name, "PLLMINHZ") || !strcmp(name, "PLLMAXHZ")) {
        /* Bounds are checked after integer rounding too. No silently clamped
         * frequency range or overflow in the round-to-nearest calculation. */
        if (value == 0U || value > DPLL_SAMPLE_RATE_HZ / 256U) return -1;
        period = (u32)(((u64)DPLL_SAMPLE_RATE_HZ + value / 2U) / value);
        if (period < 256U || period > 65534U) return -1;
        if (!strcmp(name, "PLLMINHZ")) replacement[4] = period;
        else replacement[3] = period;
    } else if (!strcmp(name, "PLLFORMAT")) {
        if (value > 1U) return -1;
        replacement[0] = (replacement[0] & 1U) | (value << 1);
    } else if (!strcmp(name, "PLLKP")) {
        if (value < 1U || value > 12U) return -1;
        replacement[5] = value;
    } else {
        if (value > 20U) return -1;
        replacement[6] = value;
    }
    if (replacement[3] > replacement[4] ||
        replacement[6] < replacement[5] + 2U) return -1;
    memcpy(desired, replacement, sizeof(desired));
    ++desired_version;
    /* Avoid the zero sentinel on wrap. This remains a latest-config mailbox. */
    if (desired_version == 0U) ++desired_version;
    if (!device_ready && !bad_id) probe_pending = 1;
    return 1;
}

int DpllCtrl_IsRequestedOrActive(void)
{
    if (bad_id) return 0;
    return ((desired[0] | applied_control |
             (commit_inflight ? inflight_control : 0U)) & 1U) != 0U;
}

void DpllCtrl_Poll(int dds_idle)
{
    u32 status, i;
    if (probe_pending) {
        probe_pending = 0;
        if (Xil_In32(base + DPLL_REG_ID) != DPLL_DEVICE_ID) {
            bad_id = 1;
            desired[0] &= ~1U;
            applied_control = inflight_control = 0U;
            applied_version = desired_version;
            xil_printf("DPLL ID mismatch; download the matching DPL1 bitstream\r\n");
        } else {
            device_ready = 1;
            xil_printf("DPL1 ready: ADC A reference, PL digital phase/frequency loop\r\n");
        }
    }
    if (!device_ready) {
        if (report_requested) {
            report_requested = 0;
            report_status = bad_id ? DPLL_STATUS_WRONG_ID : 0U;
            memset(report_words, 0, sizeof(report_words));
            report_ready = 1;
        }
        return;
    }
    status = Xil_In32(base + DPLL_REG_STATUS);
    if (commit_inflight && !(status & DPLL_STATUS_BUSY)) {
        commit_inflight = 0;
        applied_version = inflight_version;
        applied_control = inflight_control;
        xil_printf("PLL configuration applied: enabled=%u\r\n",
                   (unsigned int)(applied_control & 1U));
    }
    if (!commit_inflight && !(status & DPLL_STATUS_BUSY) &&
        desired_version != applied_version && dds_idle) {
        /* AXI staging register offsets 0x04..0x1c match desired[0..6]. */
        for (i = 0U; i < 7U; ++i)
            Xil_Out32(base + DPLL_REG_CONTROL + i * 4U, desired[i]);
        Xil_Out32(base + DPLL_REG_COMMIT, 1U);
        inflight_version = desired_version;
        inflight_control = desired[0];
        commit_inflight = 1;
    }
    if (report_requested) {
        report_requested = 0;
        /* Snapshot freezes one complete PL result, including its generation;
         * a UI must not treat an older generation as the current PLL state.
         * SNAP without live_present returns AXI SLVERR. Do not issue it while
         * committing or before a fresh live result exists: ARM MMIO cannot
         * safely treat an AXI error as an ordinary return value. */
        report_status = Xil_In32(base + DPLL_REG_STATUS);
        if (commit_inflight || desired_version != applied_version) {
            report_status |= DPLL_STATUS_WAIT_COMMIT;
            report_status &= ~(DPLL_STATUS_SNAPSHOT | DPLL_STATUS_LOCKED);
            memset(report_words, 0, sizeof(report_words));
        } else if (report_status & DPLL_STATUS_LIVE) {
            Xil_Out32(base + DPLL_REG_SNAPSHOT, 1U);
            report_status = Xil_In32(base + DPLL_REG_STATUS);
            if (report_status & DPLL_STATUS_SNAPSHOT) {
                for (i = 0U; i < DPLL_RESULT_WORDS; ++i)
                    report_words[i] = Xil_In32(base + DPLL_REG_RESULT + i * 4U);
            } else memset(report_words, 0, sizeof(report_words));
        } else {
            report_status &= ~(DPLL_STATUS_SNAPSHOT | DPLL_STATUS_LOCKED);
            memset(report_words, 0, sizeof(report_words));
        }
        report_ready = 1;
    }
}

int DpllCtrl_TakeReport(u32 words[DPLL_RESULT_WORDS], u32 *status)
{
    if (!report_ready || words == NULL || status == NULL) return 0;
    memcpy(words, report_words, sizeof(report_words));
    *status = report_status;
    report_ready = 0;
    return 1;
}
