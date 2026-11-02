// AD9238 采样数据到 AXI4-Stream 的打包模块（触发采集版本）
//
// 连接关系：
// adc9238_if -> trigger_detect -> 本模块 -> AXI4-Stream Data FIFO
//                                           -> AXI DMA S2MM -> DDR
//
// 本模块在 arm_i 有效后持续输出 ADC 数据。收到 trigger_hit_i 后，继续输出
// POST_TRIGGER_SAMPLES 个点（包含触发点），随后在最后一点输出 TLAST 并停止
// Stream 数据。FIFO 排空、DMA 完成该帧后，DDR 数据将保持不再更新。
module adc_axis_packer #(
    parameter integer FRAME_SAMPLES        = 4096,
    parameter integer POST_TRIGGER_SAMPLES = 2048
) (
    input  wire        adc_clk_i,          // ADC 采样时钟；也是 FIFO 写端时钟。
    input  wire        rst_n_i,            // 低有效复位。

    // 采集使能。可来自 PS 的 AXI 寄存器；模块内部会同步到 adc_clk_i 时钟域。
    input  wire        arm_i,

    // 来自 adc9238_if 的双通道采样数据。
    input  wire [23:0] sample_i,           // {CH_B[11:0], CH_A[11:0]}。
    input  wire        sample_valid_i,     // 当前 ADC 采样点有效。

    // 来自 trigger_detect。自动阈值触发和手动触发均已合并到此信号。
    input  wire        trigger_hit_i,
    input  wire [31:0] post_trigger_samples_i, // arm 前配置，包含触发点本身。

    // 来自 AXI4-Stream Data FIFO 的 S_AXIS_TREADY。
    input  wire        m_axis_tready_i,    // 为 1 表示 FIFO 本拍可接收 Stream 数据。

    // 32 位 AXI4-Stream 主机输出，连接 FIFO 的 S_AXIS 接口。
    output reg  [31:0] m_axis_tdata_o,     // {8'd0, CH_B[11:0], CH_A[11:0]}。
    output wire [3:0]  m_axis_tkeep_o,     // 固定为 4'b1111，表示 4 字节均有效。
    output reg         m_axis_tvalid_o,    // 为 1 表示当前 Stream 数据有效。
    output reg         m_axis_tlast_o,     // 帧结束或采集最后一点时置 1。

    // 采集状态，供 PS 读取。
    output reg         capture_active_o,   // PRE/POST 状态为 1。
    output reg         triggered_o,        // 已收到 trigger_hit_i 后为 1。
    output reg         capture_done_o,     // 最后一点已送入 FIFO；需等待 DMA 中断后再读 DDR。
    output reg  [31:0] completed_frames_o, // 已经 FIFO 握手确认的 TLAST 帧数。
    output reg  [31:0] completed_frames_gray_o, // Gray code for ADC-to-AXI CDC.
    output reg         stream_overflow_o   // FIFO 无法接收时置 1，表示出现丢样。
);

    localparam [1:0] STATE_IDLE = 2'd0;
    localparam [1:0] STATE_PRE  = 2'd1;
    localparam [1:0] STATE_POST = 2'd2;
    localparam [1:0] STATE_DONE = 2'd3;

    reg [1:0]  state;
    reg [31:0] frame_count;
    reg [31:0] post_left;
    reg [31:0] post_samples_latched;

    // arm_i 从 PS/AXI 时钟域进入 ADC 时钟域的两级同步器。
    (* ASYNC_REG = "TRUE" *) reg arm_meta;
    (* ASYNC_REG = "TRUE" *) reg arm_sync;
    // Software writes this stable bus before asserting arm. The synchronized
    // copy is sampled only when arm has crossed into the ADC clock domain.
    (* ASYNC_REG = "TRUE" *) reg [31:0] post_samples_meta;
    (* ASYNC_REG = "TRUE" *) reg [31:0] post_samples_sync;

    wire source_can_accept;
    wire periodic_last;

    // 所有 32 位采样字都有效。
    assign m_axis_tkeep_o = 4'b1111;

    // 输出寄存器为空，或当前字会在本拍被 FIFO 取走时，才可装入新样本。
    assign source_can_accept = !m_axis_tvalid_o || m_axis_tready_i;

    // 连续写 DDR 时，普通帧每 FRAME_SAMPLES 点以 TLAST 结束一次。
    assign periodic_last = (FRAME_SAMPLES == 1) ||
                           (frame_count == FRAME_SAMPLES - 1);

    always @(posedge adc_clk_i or negedge rst_n_i) begin
        if (!rst_n_i) begin
            arm_meta <= 1'b0;
            arm_sync <= 1'b0;
            post_samples_meta <= POST_TRIGGER_SAMPLES;
            post_samples_sync <= POST_TRIGGER_SAMPLES;
        end else begin
            arm_meta <= arm_i;
            arm_sync <= arm_meta;
            post_samples_meta <= post_trigger_samples_i;
            post_samples_sync <= post_samples_meta;
        end
    end

    always @(posedge adc_clk_i or negedge rst_n_i) begin
        if (!rst_n_i) begin
            state             <= STATE_IDLE;
            frame_count       <= 32'd0;
            post_left         <= 32'd0;
            post_samples_latched <= POST_TRIGGER_SAMPLES;
            m_axis_tdata_o    <= 32'd0;
            m_axis_tvalid_o   <= 1'b0;
            m_axis_tlast_o    <= 1'b0;
            capture_active_o  <= 1'b0;
            triggered_o       <= 1'b0;
            capture_done_o    <= 1'b0;
            completed_frames_o <= 32'd0;
            completed_frames_gray_o <= 32'd0;
            stream_overflow_o <= 1'b0;
        end else begin
            // AXI-Stream 握手成功后，若本拍没有装入新样本，则撤销 valid。
            if (m_axis_tvalid_o && m_axis_tready_i) begin
                m_axis_tvalid_o <= 1'b0;
                m_axis_tlast_o  <= 1'b0;
                if (m_axis_tlast_o) begin
                    completed_frames_o <= completed_frames_o + 1'b1;
                    completed_frames_gray_o <=
                        ((completed_frames_o + 1'b1) >> 1) ^
                         (completed_frames_o + 1'b1);
                end
            end

            case (state)
                STATE_IDLE: begin
                    capture_active_o <= 1'b0;
                    triggered_o      <= 1'b0;
                    capture_done_o   <= 1'b0;

                    if (arm_sync) begin
                        // 新一次采集：先持续写 DDR，等待触发。
                        state             <= STATE_PRE;
                        frame_count       <= 32'd0;
                        post_left         <= 32'd0;
                        post_samples_latched <= (post_samples_sync == 0) ?
                                                POST_TRIGGER_SAMPLES : post_samples_sync;
                        completed_frames_o <= 32'd0;
                        completed_frames_gray_o <= 32'd0;
                        stream_overflow_o <= 1'b0;
                        capture_active_o  <= 1'b1;
                    end
                end

                STATE_PRE: begin
                    if (!arm_sync) begin
                        // 软件撤销 arm，停止输出新数据。
                        state            <= STATE_IDLE;
                        capture_active_o <= 1'b0;
                        m_axis_tvalid_o  <= 1'b0;
                        m_axis_tlast_o   <= 1'b0;
                    end else if (sample_valid_i) begin
                        if (source_can_accept) begin
                            m_axis_tdata_o  <= {8'd0, sample_i};
                            m_axis_tvalid_o <= 1'b1;

                            if (trigger_hit_i) begin
                                triggered_o <= 1'b1;

                                if (post_samples_latched <= 1) begin
                                    // 当前触发点即为最后一个保存点。
                                    m_axis_tlast_o   <= 1'b1;
                                    frame_count      <= 32'd0;
                                    state            <= STATE_DONE;
                                    capture_active_o <= 1'b0;
                                end else begin
                                    // 当前触发点已经送出；后续还需保存 N-1 个点。
                                    state     <= STATE_POST;
                                    post_left <= post_samples_latched - 1;

                                    if (periodic_last) begin
                                        m_axis_tlast_o <= 1'b1;
                                        frame_count    <= 32'd0;
                                    end else begin
                                        m_axis_tlast_o <= 1'b0;
                                        frame_count    <= frame_count + 1'b1;
                                    end
                                end
                            end else if (periodic_last) begin
                                m_axis_tlast_o <= 1'b1;
                                frame_count    <= 32'd0;
                            end else begin
                                m_axis_tlast_o <= 1'b0;
                                frame_count    <= frame_count + 1'b1;
                            end
                        end else begin
                            // ADC 不能停采，FIFO 反压时当前点会丢失。
                            stream_overflow_o <= 1'b1;
                        end
                    end
                end

                STATE_POST: begin
                    if (!arm_sync) begin
                        state            <= STATE_IDLE;
                        capture_active_o <= 1'b0;
                        m_axis_tvalid_o  <= 1'b0;
                        m_axis_tlast_o   <= 1'b0;
                    end else if (sample_valid_i) begin
                        if (source_can_accept) begin
                            m_axis_tdata_o  <= {8'd0, sample_i};
                            m_axis_tvalid_o <= 1'b1;

                            if (post_left == 1) begin
                                // 最后一个触发后点：强制 TLAST，随后停止数据流。
                                m_axis_tlast_o   <= 1'b1;
                                frame_count      <= 32'd0;
                                post_left        <= 32'd0;
                                state            <= STATE_DONE;
                                capture_active_o <= 1'b0;
                            end else begin
                                post_left <= post_left - 1'b1;

                                if (periodic_last) begin
                                    m_axis_tlast_o <= 1'b1;
                                    frame_count    <= 32'd0;
                                end else begin
                                    m_axis_tlast_o <= 1'b0;
                                    frame_count    <= frame_count + 1'b1;
                                end
                            end
                        end else begin
                            stream_overflow_o <= 1'b1;
                        end
                    end
                end

                STATE_DONE: begin
                    // 已停止向 FIFO 输入新数据。等待 PS 处理 DMA 完成中断后清 arm。
                    if (!arm_sync) begin
                        state <= STATE_IDLE;
                        m_axis_tvalid_o <= 1'b0;
                        m_axis_tlast_o  <= 1'b0;
                    end else if (m_axis_tvalid_o && m_axis_tready_i && m_axis_tlast_o) begin
                        // 最后一个样本已被 FIFO 接收，随后才能报告采集完成。
                        capture_done_o <= 1'b1;
                    end
                end

                default: state <= STATE_IDLE;
            endcase
        end
    end

endmodule
