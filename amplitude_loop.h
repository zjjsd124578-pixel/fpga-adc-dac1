#ifndef AMPLITUDE_LOOP_H
#define AMPLITUDE_LOOP_H
#include "feedback_measure.h"
enum {
    LOOP_DISABLED=0, LOOP_WAIT_ADC_CAL=1, LOOP_WAIT_DAC_CAL=2,
    LOOP_WAIT_DC_CONFIRM=3, LOOP_WAIT_CAPTURE=4, LOOP_WAIT_DATA=5,
    LOOP_RUNNING=6, LOOP_LIMIT=7, LOOP_ADC_CLIPPED=8, LOOP_BAD_SIGNAL=9,
    LOOP_BAD_FREQUENCY=10
};
void AmplitudeLoop_Init(void);
int AmplitudeLoop_Command(const char *line);
void AmplitudeLoop_ManualCommand(char code);
const FeedbackCalibration *AmplitudeLoop_Calibration(void);
void AmplitudeLoop_GetChannelCalibration(unsigned channel,FeedbackCalibration *out,int *uses_default);
uint32_t AmplitudeLoop_CalibrationGeneration(void);
int AmplitudeLoop_Ready(int capture_running);
int AmplitudeLoop_NeedsSeed(void);
void AmplitudeLoop_Seed(uint32_t *gain_q16, int32_t *bias_q16);
void AmplitudeLoop_ResetMeasurement(void);
/* After an actuator commit: discard stale validity, keep PI/model history. */
void AmplitudeLoop_InvalidateMeasurement(void);
void AmplitudeLoop_SetState(int state);
int AmplitudeLoop_Update(const FeedbackMeasurement *measurement,
                         uint32_t *gain_q16, int32_t *bias_q16);
int AmplitudeLoop_TakeReport(void);
void AmplitudeLoop_GetStatus(uint32_t words[16]);
void AmplitudeLoop_Diagnostic(const FeedbackMeasurement *measurement);
int AmplitudeLoop_TakeManualBias(int32_t *bias_q16);
/* One-shot open-loop voltage offset, independent of ADC/capture/LOOP0. */
int AmplitudeLoop_TakeOpenLoopOffset(int32_t *bias_q16);
void AmplitudeLoop_SyncOutput(uint32_t gain_q16, int32_t bias_q16);
int AmplitudeLoop_State(void);
#endif
