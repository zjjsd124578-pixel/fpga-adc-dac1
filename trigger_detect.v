// 示波器触发检测模块（第一版）
//
// 当选定通道的数据跨越 trigger_level_i 时，产生一个 adc_clk_i 周期宽度的
// trigger_hit_o 脉冲。该模块只负责判断触发条件；触发后停止或继续存储数据
// 的控制由后级 adc_axis_packer 模块完成。
module trigger_detect (
    input  wire        adc_clk_i,
    input  wire        rst_n_i,

    // 来自 adc9238_if 的双通道采样数据及其有效标志。
    input  wire [11:0] ch_a_i,
    input  wire [11:0] ch_b_i,
    input  wire        sample_valid_i,

    // 触发配置。
    input  wire        trigger_enable_i,   // 1：允许触发
    input  wire        trigger_channel_i,  // 0：CH_A，1：CH_B
    input  wire        trigger_edge_i,     // 0：上升沿，1：下降沿
    input  wire [11:0] trigger_level_i,    // 12 位触发阈值

    // 手动触发请求。该信号可来自 PS 的 AXI 控制寄存器或外部 GPIO。
    // 模块内部会同步到 adc_clk_i 时钟域，并只在上升沿产生一次触发。
    // 若来自 PS，请保持高电平至少 3 个 adc_clk_i 周期后再清零。
    input  wire        manual_trigger_i,

    // 自动阈值触发或手动触发任一命中时，拉高一个 adc_clk_i 周期。
    output wire        trigger_hit_o
);

    reg  [11:0] previous_sample;
    reg         previous_valid;
    // Configuration is written while capture is disarmed and held stable while
    // enabled. Cross each bit through two flops before it feeds ADC logic.
    (* ASYNC_REG = "TRUE" *) reg trigger_enable_meta;
    (* ASYNC_REG = "TRUE" *) reg trigger_enable_sync;
    (* ASYNC_REG = "TRUE" *) reg trigger_channel_meta;
    (* ASYNC_REG = "TRUE" *) reg trigger_channel_sync;
    (* ASYNC_REG = "TRUE" *) reg trigger_edge_meta;
    (* ASYNC_REG = "TRUE" *) reg trigger_edge_sync;
    (* ASYNC_REG = "TRUE" *) reg [11:0] trigger_level_meta;
    (* ASYNC_REG = "TRUE" *) reg [11:0] trigger_level_sync;
    (* ASYNC_REG = "TRUE" *) reg manual_trigger_meta;
    (* ASYNC_REG = "TRUE" *) reg manual_trigger_sync;
    reg                       manual_trigger_sync_d;
    wire [11:0] selected_sample;
    wire        manual_trigger_pulse;
    wire        threshold_hit;

    // 根据配置选择需要触发的 ADC 通道。
    assign selected_sample = trigger_channel_sync ? ch_b_i : ch_a_i;

    // 手动触发信号跨入 ADC 时钟域，并转换为单拍脉冲。
    assign manual_trigger_pulse = manual_trigger_sync && !manual_trigger_sync_d;

    // 与 sample_i 同一拍判断阈值跨越。后级打包模块在本拍时钟沿同时读取
    // sample_i 和 trigger_hit_o，避免把触发点标记到下一组采样数据。
    assign threshold_hit = trigger_enable_sync && sample_valid_i && previous_valid &&
                           ((!trigger_edge_sync &&
                             (previous_sample < trigger_level_sync) &&
                             (selected_sample >= trigger_level_sync)) ||
                            (trigger_edge_sync &&
                             (previous_sample > trigger_level_sync) &&
                             (selected_sample <= trigger_level_sync)));
    assign trigger_hit_o = rst_n_i && (manual_trigger_pulse || threshold_hit);

    always @(posedge adc_clk_i or negedge rst_n_i) begin
        if (!rst_n_i) begin
            trigger_enable_meta <= 1'b0;
            trigger_enable_sync <= 1'b0;
            trigger_channel_meta <= 1'b0;
            trigger_channel_sync <= 1'b0;
            trigger_edge_meta <= 1'b0;
            trigger_edge_sync <= 1'b0;
            trigger_level_meta <= 12'd0;
            trigger_level_sync <= 12'd0;
            manual_trigger_meta   <= 1'b0;
            manual_trigger_sync   <= 1'b0;
            manual_trigger_sync_d <= 1'b0;
        end else begin
            trigger_enable_meta <= trigger_enable_i;
            trigger_enable_sync <= trigger_enable_meta;
            trigger_channel_meta <= trigger_channel_i;
            trigger_channel_sync <= trigger_channel_meta;
            trigger_edge_meta <= trigger_edge_i;
            trigger_edge_sync <= trigger_edge_meta;
            trigger_level_meta <= trigger_level_i;
            trigger_level_sync <= trigger_level_meta;
            manual_trigger_meta   <= manual_trigger_i;
            manual_trigger_sync   <= manual_trigger_meta;
            manual_trigger_sync_d <= manual_trigger_sync;
        end
    end

    always @(posedge adc_clk_i or negedge rst_n_i) begin
        if (!rst_n_i) begin
            previous_sample <= 12'd0;
            previous_valid  <= 1'b0;
        end else begin
            if (!trigger_enable_sync) begin
                // 关闭触发后丢弃历史点，重新开启时不会误触发。
                previous_valid <= 1'b0;
            end else if (sample_valid_i) begin
                // 第一组有效数据只保存；从下一组数据开始比较阈值。
                previous_sample <= selected_sample;
                previous_valid  <= 1'b1;
            end
        end
    end

endmodule
