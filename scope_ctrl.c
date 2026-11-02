#include "scope_ctrl.h"

#include "xparameters.h"
#include "xil_io.h"
#include "sleep.h"

/*
 * 杩欐槸 Vivado 瀵煎嚭 XSA銆乂itis 鐢熸垚 BSP 鍚庤嚜鍔ㄥ緱鍒扮殑鍩哄湴鍧�瀹忋��
 * 鑻ョ紪璇戞椂鎻愮ず鏈畾涔夛紝鎵撳紑 xparameters.h 鎼滅储 SCOPE_AXI_CTRL锛�
 * 骞舵妸涓嬮潰杩欎竴琛屾敼鎴愬疄闄呯敓鎴愮殑瀹忓悕銆�
 */
#define SCOPE_CTRL_BASEADDR  XPAR_SCOPE_AXI_CTRL_0_BASEADDR

/* 淇濆瓨褰撳墠 CONTROL 瀵勫瓨鍣ㄧ殑閰嶇疆鍊笺�� */
static u32 control_value = 0U;

/* 灏� control_value 鍐欏叆 PL 鐨� CONTROL 瀵勫瓨鍣ㄣ�� */
static void ScopeCtrl_WriteControl(void)
{
    Xil_Out32(SCOPE_CTRL_BASEADDR + SCOPE_REG_CONTROL,
              control_value);
}

/* 鍒濆鍖栵細娓呯┖鎺у埗瀵勫瓨鍣紝纭繚 arm=0锛孭L 涓嶉噰闆嗐�� */
void ScopeCtrl_Init(void)
{
    control_value = 0U;
    ScopeCtrl_WriteControl();
}

/*
 * 閰嶇疆鑷姩瑙﹀彂锛�
 * level        锛�12 浣嶉槇鍊硷紝鑼冨洿 0x000 锝� 0xFFF
 * channel_b=0  锛氶�夋嫨 CH_A
 * channel_b=1  锛氶�夋嫨 CH_B
 * falling_edge=0锛氫笂鍗囨部瑙﹀彂
 * falling_edge=1锛氫笅闄嶆部瑙﹀彂
 */
void ScopeCtrl_SetTrigger(u16 level, int channel_b, int falling_edge)
{
    /* 鍏堝啓 TRIGGER_LEVEL 瀵勫瓨鍣紝鍙厑璁镐綆 12 浣嶆湁鏁堛�� */
    Xil_Out32(SCOPE_CTRL_BASEADDR + SCOPE_REG_TRIGGER_LEVEL,
              ((u32)level & 0x0FFFU));

    /* 娓呴櫎鏃х殑瑙﹀彂閰嶇疆 bit锛屼繚鐣� arm 鐘舵�併�� */
    control_value &= ~(SCOPE_CTRL_TRIGGER_ENABLE |
                       SCOPE_CTRL_TRIGGER_CHANNEL |
                       SCOPE_CTRL_TRIGGER_EDGE);

    /* 寮�鍚嚜鍔ㄨЕ鍙戙�� */
    control_value |= SCOPE_CTRL_TRIGGER_ENABLE;

    if (channel_b)
        control_value |= SCOPE_CTRL_TRIGGER_CHANNEL;

    if (falling_edge)
        control_value |= SCOPE_CTRL_TRIGGER_EDGE;

    ScopeCtrl_WriteControl();
}

void ScopeCtrl_SetPostSamples(u32 count)
{
    Xil_Out32(SCOPE_CTRL_BASEADDR + SCOPE_REG_POST_SAMPLES, count);
}

void ScopeCtrl_EnableAutoTrigger(int enable)
{
    if (enable)
        control_value |= SCOPE_CTRL_TRIGGER_ENABLE;
    else
        control_value &= ~SCOPE_CTRL_TRIGGER_ENABLE;
    ScopeCtrl_WriteControl();
}

/* arm=1锛氬紑濮嬮噰闆嗭紱arm=0锛氬仠姝�/澶嶄綅鏈閲囬泦鐘舵�併�� */
void ScopeCtrl_SetArm(int arm)
{
    if (arm)
        control_value |= SCOPE_CTRL_ARM;
    else
        control_value &= ~SCOPE_CTRL_ARM;

    ScopeCtrl_WriteControl();
}

/*
 * 浜х敓涓�娆℃墜鍔ㄨЕ鍙戙��
 * 淇濇寔绾� 1 us锛岃繙澶т簬 65 MHz ADC 鏃堕挓鐨� 3 涓懆鏈燂紝PL 鑳藉彲闈犲悓姝ュ埌銆�
 */
void ScopeCtrl_ManualTrigger(void)
{
    Xil_Out32(SCOPE_CTRL_BASEADDR + SCOPE_REG_CONTROL,
              control_value | SCOPE_CTRL_MANUAL_TRIGGER);

    usleep(1);

    ScopeCtrl_WriteControl();
}

/* 璇诲彇 PL 杩斿洖鐨勯噰闆嗙姸鎬併�� */
u32 ScopeCtrl_ReadStatus(void)
{
    return Xil_In32(SCOPE_CTRL_BASEADDR + SCOPE_REG_STATUS);
}

u32 ScopeCtrl_ReadCompletedFrames(void)
{
    u32 gray = Xil_In32(SCOPE_CTRL_BASEADDR + SCOPE_REG_COMPLETED_FRAMES);
    gray ^= gray >> 16;
    gray ^= gray >> 8;
    gray ^= gray >> 4;
    gray ^= gray >> 2;
    gray ^= gray >> 1;
    return gray;
}
