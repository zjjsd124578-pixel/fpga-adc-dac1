#include "scope_net.h"
#include "dma_capture.h"
#include "dds_command.h"
#include "dds_ctrl.h"
#include "amplitude_loop.h"
#include "dpll_ctrl.h"
#include "sweep_ctrl.h"

#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "xil_printf.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define WIRE_HEADER_BYTES 40U
#define WIRE_TYPE_PREVIEW 1U
#define WIRE_TYPE_CAPTURE 2U
#define WIRE_TYPE_STATUS  3U
#define WIRE_TYPE_LOOP 4U
#define WIRE_TYPE_PLL 8U
#define TCP_COPY_CHUNK    1460U

static struct tcp_pcb *listener;
static struct tcp_pcb *client;
static volatile u32 commands;
static char command_line[128];
static int command_overflow;
static int loop_status_pending;
static u8 loop_status[64], loop_packet[64];
static int pll_status_pending;
static u32 pll_status_flags;
static u8 pll_status[64], pll_packet[64];
static u32 command_length;
typedef struct {u32 type,flags,count;u8 data[128];} MeasurePacket;
static MeasurePacket measure_queue[16];
static unsigned measure_read,measure_write,measure_count;
static u8 measure_packet[128];
static u32 sequence_number;
static u32 requested_view_us;
static u32 requested_before_us;
static u32 requested_after_us;

static u8 packet_header[WIRE_HEADER_BYTES];
static u32 preview_words[SCOPE_PREVIEW_POINTS];
static u32 packet_type;
static u32 header_offset;
static u32 payload_offset;
static u32 payload_bytes;
static u64 payload_first_sample;

static int status_pending;
static u32 status_flags;
static u64 status_samples;
static u32 status_valid;
static int dump_pending;
static u64 dump_first_sample;
static u32 dump_count;
static u32 dump_flags;

static void put16(u8 *p, u16 value)
{
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
}

static void put32(u8 *p, u32 value)
{
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
    p[2] = (u8)(value >> 16);
    p[3] = (u8)(value >> 24);
}

static void put64(u8 *p, u64 value)
{
    put32(p, (u32)value);
    put32(p + 4, (u32)(value >> 32));
}

static void begin_packet(u32 type, u32 flags, u64 first_sample,
                         u32 stride, u32 count, u32 bytes)
{
    packet_header[0] = 'S';
    packet_header[1] = 'C';
    packet_header[2] = 'P';
    packet_header[3] = '1';
    put16(packet_header + 4, 1U);
    put16(packet_header + 6, (u16)type);
    put32(packet_header + 8, ++sequence_number);
    put32(packet_header + 12, flags);
    put32(packet_header + 16, DMA_SAMPLE_RATE_HZ);
    put64(packet_header + 20, first_sample);
    put32(packet_header + 28, stride);
    put32(packet_header + 32, count);
    put32(packet_header + 36, bytes);
    packet_type = type;
    header_offset = 0U;
    payload_offset = 0U;
    payload_bytes = bytes;
    payload_first_sample = first_sample;
}

static int starts_amplitude_pi(const char *line)
{
    char name[20];
    unsigned int length = 0U;
    while (*line == ' ' || *line == '\t') ++line;
    while (isalpha((unsigned char)*line)) {
        if (length + 1U >= sizeof(name)) return 0;
        name[length++] = (char)toupper((unsigned char)*line++);
    }
    name[length] = '\0';
    if (!strcmp(name, "VPP")) return 1;
    if (strcmp(name, "LOOP")) return 0;
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == '=' || *line == ':') ++line;
    while (*line == ' ' || *line == '\t') ++line;
    return strtol(line, NULL, 10) != 0L;
}

static void parse_command(void)
{
    char *cursor;
    char *end;
    unsigned long values[3];
    int i;
    DdsCommand dds_command;
    int result;
    char upper[128];unsigned j;

    command_line[command_length] = '\0';
    for(j=0;j<=command_length;++j)upper[j]=(char)toupper((unsigned char)command_line[j]);
    result=SweepCtrl_Command(command_line);
    if(result!=0){
        if(result<0)xil_printf("Sweep/PL amplitude command rejected; check mode, parameters and straight-through calibration\r\n");
        command_length=0;return;
    }
    result = DpllCtrl_Command(command_line);
    if (result != 0) {
        if (result < 0) xil_printf("Invalid PLL command\r\n");
        else if (DpllCtrl_IsRequestedOrActive()) {
            if(SweepCtrl_IsScanning()) {DpllCtrl_Command("PLL0");xil_printf("PLL rejected during sweep\r\n");}
            else SweepCtrl_StopForPll();
            AmplitudeLoop_Command("LOOP0");
        }
        command_length = 0U; return;
    }
    if (DpllCtrl_IsRequestedOrActive() && starts_amplitude_pi(command_line)) {
        xil_printf("Amplitude PI command rejected: send PLL0 and wait for it to apply first\r\n");
        command_length = 0U; return;
    }
    if(SweepCtrl_IsScanning() && strcmp(upper,"LOOPSTATUS")!=0 &&
       (starts_amplitude_pi(command_line)||strncmp(upper,"CAL",3)==0||strncmp(upper,"ADCDEFAULT",10)==0||strncmp(upper,"FBCH",4)==0||
        strncmp(upper,"OFFSET",6)==0||strncmp(upper,"BIAS",4)==0)) {
        xil_printf("Output/calibration change rejected during sweep; use SWSTOP\r\n");command_length=0;return;
    }
    result = AmplitudeLoop_Command(command_line);
    if (result != 0) {
        if (result < 0) xil_printf("Invalid feedback command\r\n");
        command_length = 0U; return;
    }
    result = DdsCommand_Parse(command_line, command_length, &dds_command);
    if (result != DDS_COMMAND_OTHER) {
        if (result == DDS_COMMAND_VALID) {
            /* E0 is a global close, not an ordinary scan parameter change. */
            if(dds_command.code=='E' && dds_command.value==0U) {
                result=DdsCtrl_QueueCommand(&dds_command);
                if(result==DDS_CTRL_OK) {
                    SweepCtrl_StopOutput();
                    AmplitudeLoop_ManualCommand('E');
                } else xil_printf("DDS close rejected: %d\r\n",result);
                command_length=0;return;
            }
            if(SweepCtrl_IsScanning() || SweepCtrl_IsStopping()){xil_printf("Manual DDS change rejected during sweep/exit; wait for completion\r\n");command_length=0;return;}
            result = DdsCtrl_QueueCommand(&dds_command);
            if (result != DDS_CTRL_OK)
                xil_printf("DDS command rejected: %d\r\n", result);
            else {
                SweepCtrl_ManualCommand(dds_command.code);
                AmplitudeLoop_ManualCommand(dds_command.code);
            }
        } else {
            xil_printf("Invalid DDS command\r\n");
        }
        command_length = 0U;
        return;
    }
    if (strcmp(command_line, "ARM") == 0)
        commands |= SCOPE_CMD_ARM;
    else if (strcmp(command_line, "TRIGGER") == 0)
        commands |= SCOPE_CMD_TRIGGER;
    else if (strcmp(command_line, "PAUSE") == 0)
        commands |= SCOPE_CMD_PAUSE;
    else if (strcmp(command_line, "DUMP") == 0)
        commands |= SCOPE_CMD_DUMP;
    else if (strncmp(command_line, "WINDOW ", 7U) == 0) {
        cursor = command_line + 7;
        for (i = 0; i < 3; ++i) {
            values[i] = strtoul(cursor, &end, 10);
            if (end == cursor || values[i] > 3400000UL ||
                (i < 2 && *end != ' ') || (i == 2 && *end != '\0'))
                break;
            cursor = end + 1;
        }
        if (i == 3 && values[0] >= 1000UL &&
            values[0] + values[1] + values[2] <= 3400000UL) {
            requested_view_us = (u32)values[0];
            requested_before_us = (u32)values[1];
            requested_after_us = (u32)values[2];
            commands |= SCOPE_CMD_WINDOW;
        }
    }
    command_length = 0U; command_overflow=0;
}

static err_t on_receive(void *arg, struct tcp_pcb *pcb,
                        struct pbuf *p, err_t err)
{
    struct pbuf *part;
    u32 i;
    const u8 *bytes;
    (void)arg;
    (void)err;

    if (p == NULL) {
        client = NULL;
        packet_type = 0U;
        command_length = 0U; command_overflow=0; loop_status_pending=0; pll_status_pending=0; measure_read=measure_write=measure_count=0;
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_close(pcb);
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);
    for (part = p; part != NULL; part = part->next) {
        bytes = (const u8 *)part->payload;
        for (i = 0U; i < part->len; ++i) {
            if (bytes[i] == '\n') {
                if (!command_overflow) parse_command();
                else xil_printf("Command too long; discarded\r\n");
                command_length=0; command_overflow=0;
            } else if (bytes[i] != '\r') {
                if (!command_overflow && command_length + 1U < sizeof(command_line))
                    command_line[command_length++] = (char)bytes[i];
                else command_overflow=1;
            }
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

static void on_error(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    client = NULL;
    packet_type = 0U;
    command_length = 0U; command_overflow=0; loop_status_pending=0; pll_status_pending=0; measure_read=measure_write=measure_count=0;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    (void)err;
    if (client != NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    client = pcb;
    packet_type = 0U;
    command_length = 0U; command_overflow=0; loop_status_pending=0; pll_status_pending=0; measure_read=measure_write=measure_count=0;
    tcp_recv(pcb, on_receive);
    tcp_err(pcb, on_error);
    tcp_nagle_disable(pcb);
    status_pending = 1;
    xil_printf("Scope client connected\r\n");
    return ERR_OK;
}

int ScopeNet_Start(void)
{
    err_t err;
    listener = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (listener == NULL)
        return -1;
    err = tcp_bind(listener, IP_ANY_TYPE, SCOPE_NET_PORT);
    if (err != ERR_OK)
        return -2;
    listener = tcp_listen(listener);
    if (listener == NULL)
        return -3;
    tcp_accept(listener, on_accept);
    xil_printf("Scope TCP server listening on %u\r\n", SCOPE_NET_PORT);
    return 0;
}

u32 ScopeNet_TakeCommands(void)
{
    u32 result = commands;
    commands = 0U;
    return result;
}

void ScopeNet_GetWindow(u32 *view_us, u32 *before_us, u32 *after_us)
{
    *view_us = requested_view_us;
    *before_us = requested_before_us;
    *after_us = requested_after_us;
}

int ScopeNet_IsConnected(void)
{
    return client != NULL;
}

int ScopeNet_QueueMeasure(u32 type,u32 flags,const u32 *words,u32 count)
{
    unsigned i;MeasurePacket *p;
    if(!client || !words || count>32 || type<9 || type>12 || measure_count==16)return 0;
    p=&measure_queue[measure_write];p->type=type;p->flags=flags;p->count=count;
    for(i=0;i<count;++i)put32(p->data+4*i,words[i]);
    measure_write=(measure_write+1)%16;++measure_count;return 1;
}

int ScopeNet_IsDumpBusy(void)
{
    return dump_pending || packet_type == WIRE_TYPE_CAPTURE;
}

int ScopeNet_QueuePreview(u64 first_sample, const u32 *words, u32 count,
                          u32 stride, u32 flags)
{
    if (client == NULL || packet_type != 0U || dump_pending ||
        status_pending || pll_status_pending || measure_count || count > SCOPE_PREVIEW_POINTS || stride == 0U)
        return 0;
    memcpy(preview_words, words, count * sizeof(u32));
    begin_packet(WIRE_TYPE_PREVIEW, flags, first_sample,
                 stride, count, count * sizeof(u32));
    return 1;
}

void ScopeNet_ReportStatus(u32 flags, u64 total_samples, u32 valid_samples)
{
    status_flags = flags;
    status_samples = total_samples;
    status_valid = valid_samples;
    status_pending = 1;
}

void ScopeNet_BeginDump(u64 first_sample, u32 count, u32 flags)
{
    dump_first_sample = first_sample;
    dump_count = count;
    dump_flags = flags;
    dump_pending = 1;
}

void ScopeNet_ReportLoop(const u32 words[16])
{
    unsigned int i;
    if(!client) return;
    for(i=0;i<16U;++i) put32(loop_status+i*4U,words[i]);
    loop_status_pending=1;
}

void ScopeNet_ReportPll(const u32 words[16], u32 flags)
{
    unsigned int i;
    if (client == NULL) return;
    for (i = 0U; i < 16U; ++i) put32(pll_status + i * 4U, words[i]);
    pll_status_flags = flags;
    pll_status_pending = 1;
}

void ScopeNet_Poll(void)
{
    const u8 *source;
    u32 remaining;
    u32 available;
    u32 count;
    u32 address;
    u32 end_address;
    int sent = 0;
    int i;
    err_t err;

    if (client == NULL)
        return;

    if (packet_type == 0U) {
        if (status_pending) {
            status_pending = 0;
            begin_packet(WIRE_TYPE_STATUS, status_flags, status_samples,
                         1U, status_valid, 0U);
        } else if (measure_count) {
            MeasurePacket *p=&measure_queue[measure_read];
            memcpy(measure_packet,p->data,p->count*4);
            begin_packet(p->type,p->flags,0,1,p->count,p->count*4);
            measure_read=(measure_read+1)%16;--measure_count;
        } else if (pll_status_pending && !dump_pending) {
            pll_status_pending = 0;
            memcpy(pll_packet, pll_status, sizeof(pll_packet));
            begin_packet(WIRE_TYPE_PLL, pll_status_flags, 0U, 1U, 16U, sizeof(pll_packet));
        } else if (loop_status_pending && !dump_pending) {
            loop_status_pending=0;
            memcpy(loop_packet,loop_status,sizeof(loop_packet));
            begin_packet(WIRE_TYPE_LOOP,0,0,1U,16U,sizeof(loop_packet));
        } else if (dump_pending) {
            dump_pending = 0;
            begin_packet(WIRE_TYPE_CAPTURE, dump_flags, dump_first_sample,
                         1U, dump_count, dump_count * sizeof(u32));
        } else {
            return;
        }
    }

    for (i = 0; i < 8; ++i) {
        available = tcp_sndbuf(client);
        if (available == 0U)
            break;

        if (header_offset < WIRE_HEADER_BYTES) {
            source = packet_header + header_offset;
            remaining = WIRE_HEADER_BYTES - header_offset;
            count = remaining < available ? remaining : available;
            err = tcp_write(client, source, (u16_t)count, TCP_WRITE_FLAG_COPY);
            if (err != ERR_OK)
                break;
            header_offset += count;
        } else if (payload_offset < payload_bytes) {
            remaining = payload_bytes - payload_offset;
            count = remaining < TCP_COPY_CHUNK ? remaining : TCP_COPY_CHUNK;
            if (count > available)
                count = available;
            count &= ~3U;
            if (count == 0U)
                break;
            if (packet_type == WIRE_TYPE_PREVIEW) {
                source = (const u8 *)preview_words + payload_offset;
            } else if (packet_type == WIRE_TYPE_LOOP) {
                source=loop_packet+payload_offset;
            } else if (packet_type == WIRE_TYPE_PLL) {
                source=pll_packet+payload_offset;
            } else if (packet_type>=9 && packet_type<=12) {
                source=measure_packet+payload_offset;
            } else {
                address = DmaCapture_GetSampleAddr(
                    payload_first_sample + payload_offset / sizeof(u32));
                end_address = DMA_RECORD_BASE_ADDR + DmaCapture_GetBufferSize();
                if (count > end_address - address)
                    count = end_address - address;
                DmaCapture_Invalidate(address, count);
                source = (const u8 *)(UINTPTR)address;
            }
            err = tcp_write(client, source, (u16_t)count, TCP_WRITE_FLAG_COPY);
            if (err != ERR_OK)
                break;
            payload_offset += count;
        } else {
            packet_type = 0U;
            break;
        }
        sent = 1;
    }
    if (sent)
        tcp_output(client);
    if (header_offset == WIRE_HEADER_BYTES && payload_offset == payload_bytes)
        packet_type = 0U;
}
