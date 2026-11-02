#ifndef FEEDBACK_DEFAULTS_H
#define FEEDBACK_DEFAULTS_H
/* Host display convention inferred from the supplied screen, not physical
 * calibration: code 2150 displays +0.2490 V, code 1939 displays -0.2661 V.
 * ADC offset binary, 4096 codes over 10000 mV, code 2048 = 0 mV.
 * Vpp = (maximum - minimum) * 10000 / 4096 mV.
 * Raw feedback and sparse preview may have slightly different extrema. */
#define FEEDBACK_ADC_CODE0 0
#define FEEDBACK_ADC_MV0 (-5000)
#define FEEDBACK_ADC_CODE1 2048
#define FEEDBACK_ADC_MV1 0
#define FEEDBACK_ADC_FORMAT 0
/* Initial PI seed and open-loop OFFSET scale only. Historical
 * low-frequency span was 1.85 Vpp; the supplied analog path is inverting.
 * RA1/load changes affect this number. CALDAC overrides it at runtime.
 * The PI can estimate feedback span for its step size without changing this
 * setting, the ADC voltage convention, or claiming physical calibration. */
#define FEEDBACK_DAC_SIGNED_SPAN_MV (-1850)
/* Must match axis_data_fifo_0/FIFO_DEPTH in S2MM.bd (32-bit sample words).
 * A post-commit freshness fence also covers all pending DMA BDs and two
 * frames for the CDC/ADC pipeline; a queued FIFO can still hold old samples. */
#define FEEDBACK_AXIS_FIFO_SAMPLES 32768U
#endif
