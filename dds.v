module dds (
    input  clk_125m,
    input  rst_n,

    input  waveform_en,              // 波形设置使能
    input  sweep_en,                 // 扫频使能

    input  [2:0]   waveform_sel,     // 1=Sine, 2=Cos, 3=Square, 4=Triangle, 5=Saw
    input  [31:0]  freq_tuning_word, // 固定输出频率的 FTW
    input  [31:0]  phase_offset,     // 相位偏移

    input  [31:0]  sweep_start_ftw,  // 扫频起始频率 FTW
    input  [31:0]  sweep_stop_ftw,   // 扫频停止频率 FTW
    input  [31:0]  sweep_step_ftw,   // 扫频步进值

    output [9:0]   phase_addr,       // ROM 的 10-bit 相位地址
    output [12:0]  rom_addr          // 13-bit ROM 地址 = {wave_sel, phase_addr}
);

    reg  [31:0] phase_accumulator;   // 相位累加器
    reg  [31:0] current_ftw;         // 当前使用的频率调谐字（FTW）
    reg         sweep_active;        // 扫频状态
    reg  [17:0] cnt_1ms;             // 1ms 计数器（100MHz -> 100000 个周期）

    wire [31:0] phase_with_offset;   // 相位累加器 + 相位偏移
    wire [2:0]  waveform_idx;        // 波形表

    localparam [17:0] CNT_1MS = 18'd99_999; // 100 MHz / 1ms = 100000 - 1

    // 兼容 MIF 中 01..05 顺序排列：索引 1..5 -> 0..4
    assign waveform_idx = (waveform_sel >= 3'd1 && waveform_sel <= 3'd5) ? (waveform_sel - 3'd1) : 3'd0;

    // 相位累加器 + 相位偏移
    assign phase_with_offset = phase_accumulator + phase_offset;
    assign phase_addr = phase_with_offset[31:22];

    // ROM 地址 = 波形选择 + 相位地址
    assign rom_addr = {waveform_idx, phase_addr};

    always @(posedge clk_125m or negedge rst_n) begin
        if (!rst_n) begin
            phase_accumulator <= 32'd0;
            current_ftw       <= 32'd0;
            sweep_active      <= 1'b0;
            cnt_1ms           <= 18'd0;
        end else if (waveform_en) begin
            // 普通波形输出模式：使用固定 FTW
            current_ftw       <= freq_tuning_word;
            phase_accumulator <= phase_accumulator + freq_tuning_word;
            sweep_active      <= 1'b0;
            cnt_1ms           <= 18'd0;
        end else if (sweep_en) begin
            if (cnt_1ms == CNT_1MS) begin
                cnt_1ms <= 18'd0;

                if (!sweep_active) begin
                    // 第一次进入扫频时，从起始频率开始
                    current_ftw  <= sweep_start_ftw;
                    sweep_active <= 1'b1;
                end else begin
                    if (sweep_start_ftw <= sweep_stop_ftw) begin
                        if (current_ftw + sweep_step_ftw >= sweep_stop_ftw)
                            current_ftw <= sweep_stop_ftw;
                        else
                            current_ftw <= current_ftw + sweep_step_ftw;
                    end else begin
                        if (current_ftw <= sweep_stop_ftw)
                            current_ftw <= sweep_stop_ftw;
                        else
                            current_ftw <= current_ftw - sweep_step_ftw;
                    end
                end
            end else begin
                cnt_1ms <= cnt_1ms + 1'b1;
            end

            phase_accumulator <= phase_accumulator + current_ftw;
        end else begin
            // 关闭扫描时保持当前状态
            phase_accumulator <= phase_accumulator;
            current_ftw       <= current_ftw;
            sweep_active      <= 1'b0;
            cnt_1ms           <= 18'd0;
        end
    end
endmodule