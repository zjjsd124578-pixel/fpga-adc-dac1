`timescale 1ns/1ps
// Standalone DDS for Vivado; no UART, vendor PLL, or ROM IP dependency.
// All configuration inputs and cfg_load_i MUST be synchronous to clk_i.
// A future AXI/PS bridge must transfer the complete configuration atomically.
// cfg_load_i applies an atomic configuration. restart_phase_i explicitly restarts.
// Feedback gain/bias updates keep the phase accumulator and ROM pipeline running.
// FTW = round(output_frequency_hz * 2^32 / CLK_HZ).
// dac_data_o is an unsigned 8-bit code; disabled output is midscale (128).
// Generate/forward the physical DAC clock separately and constrain its timing.
module dac_dds_feedback #(
    parameter integer CLK_HZ = 65000000,
    parameter ROM_FILE = "dds_waveforms.mem",
    parameter integer USE_PI = 0
) (
    input  wire        clk_i,
    input  wire        rst_n_i,
    input  wire        cfg_load_i,
    input  wire        restart_phase_i,
    input  wire signed [31:0] offset_q16_i, // signed DAC-code offset, Q16
    input  wire        enable_i,
    input  wire        sweep_enable_i,
    input  wire [2:0]  waveform_sel_i, // 1=sine, 2=cosine, 3=square, 4=triangle, 5=saw
    input  wire [31:0] freq_tuning_word_i,
    input  wire [31:0] phase_offset_i,
    input  wire [16:0] amplitude_q16_i, // 0..65536 = 0..100%, centered on code 128
    input  wire [31:0] sweep_start_ftw_i,
    input  wire [31:0] sweep_stop_ftw_i,
    input  wire [31:0] sweep_step_ftw_i,
    // PLL is synchronous to clk_i; enable selects mode even while acquiring.
    input  wire        pll_enable_i,
    input  wire        pll_active_i,
    input  wire [31:0] pll_phase_i,
    input  wire [31:0] pll_ftw_i,
    input  wire        pi_enable_i,
    input  wire [16:0] pi_gain_i,
    output reg  [7:0]  dac_data_o,
    output wire [31:0] current_ftw_o
);
    // One sweep step per millisecond. CLK_HZ must match the actual input clock.
    localparam integer STEP_CYCLES = (CLK_HZ >= 1000) ? CLK_HZ / 1000 : 1;
    localparam integer COUNT_BITS = (STEP_CYCLES > 1) ? $clog2(STEP_CYCLES) : 1;

    reg enable_r, sweep_enable_r;
    reg [2:0] waveform_index_r;
    reg [31:0] phase_r, phase_offset_r, current_ftw_r;
    reg [31:0] sweep_start_r, sweep_stop_r, sweep_step_r;
    reg [COUNT_BITS-1:0] step_count_r;
    reg rom_valid_r;
    reg [16:0] amplitude_q16_r;
    reg signed [31:0] offset_q16_r;

    (* rom_style = "block" *) reg [7:0] wave_rom [0:5119];
    reg [7:0] rom_data_r;
    wire [31:0] phase_with_offset = (pll_enable_i ? pll_phase_i : phase_r) + phase_offset_r;
    wire [12:0] rom_address = {waveform_index_r, phase_with_offset[31:22]};
    wire [32:0] sweep_sum = {1'b0, current_ftw_r} + {1'b0, sweep_step_r};
    wire [31:0] sweep_remaining = current_ftw_r - sweep_stop_r;

    // Explicit signed widths keep negative half-cycles correct. Round to the
    // nearest DAC code, symmetrically for positive and negative values.
    wire signed [8:0] centered_sample = $signed({1'b0, rom_data_r}) - 9'sd128;
    wire [16:0] applied_gain=(USE_PI && pi_enable_i) ? pi_gain_i : amplitude_q16_r;
    wire signed [26:0] amplitude_product = centered_sample * $signed({1'b0, applied_gain});
    wire signed [32:0] biased_product = $signed(amplitude_product) + $signed(offset_q16_r);
    wire signed [32:0] rounded_sample = (biased_product < 0)
        ? -(((-biased_product) + 33'sd32768) >>> 16)
        : ((biased_product + 33'sd32768) >>> 16);
    wire signed [32:0] biased_sample = rounded_sample + 33'sd128;
    wire [7:0] limited_sample = (biased_sample < 0) ? 8'd0 :
                                (biased_sample > 255) ? 8'd255 : biased_sample[7:0];

    initial $readmemh(ROM_FILE, wave_rom);
    // Synchronous ROM read without a reset on its output to permit BRAM inference.
    always @(posedge clk_i)
        rom_data_r <= wave_rom[rom_address];

    assign current_ftw_o = pll_enable_i ? pll_ftw_i : current_ftw_r;

    always @(posedge clk_i or negedge rst_n_i) begin
        if (!rst_n_i) begin
            amplitude_q16_r <= 17'd65536;
            offset_q16_r <= 0;
            enable_r <= 1'b0;
            sweep_enable_r <= 1'b0;
            waveform_index_r <= 3'd0;
            phase_r <= 32'd0;
            phase_offset_r <= 32'd0;
            current_ftw_r <= 32'd0;
            sweep_start_r <= 32'd0;
            sweep_stop_r <= 32'd0;
            sweep_step_r <= 32'd0;
            step_count_r <= 0;
            rom_valid_r <= 1'b0;
            dac_data_o <= 8'd128;
        end else if (cfg_load_i) begin
            amplitude_q16_r <= (amplitude_q16_i > 17'd65536) ? 17'd65536 : amplitude_q16_i;
            offset_q16_r <= offset_q16_i;
            enable_r <= enable_i;
            sweep_enable_r <= sweep_enable_i;
            waveform_index_r <= ((waveform_sel_i >= 1) && (waveform_sel_i <= 5))
                                ? waveform_sel_i - 3'd1 : 3'd0;
            phase_offset_r <= phase_offset_i;
            current_ftw_r <= sweep_enable_i ? sweep_start_ftw_i : freq_tuning_word_i;
            sweep_start_r <= sweep_start_ftw_i;
            sweep_stop_r <= sweep_stop_ftw_i;
            sweep_step_r <= sweep_step_ftw_i;
            if (restart_phase_i || !enable_r || enable_i != enable_r ||
                sweep_enable_i != sweep_enable_r ||
                waveform_sel_i != waveform_index_r + 3'd1) begin
                phase_r <= 0;
                step_count_r <= 0;
                rom_valid_r <= 0;
                dac_data_o <= 8'd128;
            end else begin
                phase_r <= phase_r + current_ftw_r;
                rom_valid_r <= enable_r && (!pll_enable_i || pll_active_i);
                dac_data_o <= (rom_valid_r && (!pll_enable_i || pll_active_i)) ? limited_sample : 8'd128;
            end
        end else if (enable_r) begin
            phase_r <= phase_r + current_ftw_r;
            rom_valid_r <= !pll_enable_i || pll_active_i;
            dac_data_o <= (rom_valid_r && (!pll_enable_i || pll_active_i)) ? limited_sample : 8'd128;
            if (sweep_enable_r && !pll_enable_i) begin
                if (step_count_r == STEP_CYCLES - 1) begin
                    step_count_r <= 0;
                    if (sweep_start_r <= sweep_stop_r) begin
                        // Use a 33-bit sum so overflow cannot wrap past the endpoint.
                        if (sweep_sum >= {1'b0, sweep_stop_r})
                            current_ftw_r <= sweep_stop_r;
                        else
                            current_ftw_r <= sweep_sum[31:0];
                    end else begin
                        // Compare remaining distance before subtracting to avoid underflow.
                        if ((current_ftw_r <= sweep_stop_r) || (sweep_step_r >= sweep_remaining))
                            current_ftw_r <= sweep_stop_r;
                        else
                            current_ftw_r <= current_ftw_r - sweep_step_r;
                    end
                end else begin
                    step_count_r <= step_count_r + 1'b1;
                end
            end else begin
                step_count_r <= 0;
            end
        end else begin
            step_count_r <= 0;
            rom_valid_r <= 1'b0;
            dac_data_o <= 8'd128;
        end
    end
endmodule

module dac_dds_sweep #(
    parameter integer CLK_HZ=65000000,
    parameter ROM_FILE="dds_waveforms.mem"
) (
    input wire clk_i,rst_n_i,cfg_load_i,restart_phase_i,
    input wire signed [31:0] offset_q16_i,
    input wire enable_i,sweep_enable_i,
    input wire [2:0] waveform_sel_i,
    input wire [31:0] freq_tuning_word_i,phase_offset_i,
    input wire [16:0] amplitude_q16_i,
    input wire [31:0] sweep_start_ftw_i,sweep_stop_ftw_i,sweep_step_ftw_i,
    input wire pll_enable_i,pll_active_i,pi_enable_i,
    input wire [16:0] pi_gain_i,
    input wire [31:0] pll_phase_i,pll_ftw_i,
    output wire [7:0] dac_data_o,
    output wire [31:0] current_ftw_o
);
    dac_dds_feedback #(.CLK_HZ(CLK_HZ),.ROM_FILE(ROM_FILE),.USE_PI(1)) dds_u(
        .clk_i(clk_i),.rst_n_i(rst_n_i),.cfg_load_i(cfg_load_i),.restart_phase_i(restart_phase_i),
        .offset_q16_i(offset_q16_i),.enable_i(enable_i),.sweep_enable_i(sweep_enable_i),
        .waveform_sel_i(waveform_sel_i),.freq_tuning_word_i(freq_tuning_word_i),
        .phase_offset_i(phase_offset_i),.amplitude_q16_i(amplitude_q16_i),
        .sweep_start_ftw_i(sweep_start_ftw_i),.sweep_stop_ftw_i(sweep_stop_ftw_i),
        .sweep_step_ftw_i(sweep_step_ftw_i),.pll_enable_i(pll_enable_i),.pll_active_i(pll_active_i),
        .pll_phase_i(pll_phase_i),.pll_ftw_i(pll_ftw_i),.pi_enable_i(pi_enable_i),.pi_gain_i(pi_gain_i),
        .dac_data_o(dac_data_o),.current_ftw_o(current_ftw_o));
endmodule

// A distinct module reference makes the added PLL ports explicit to Vivado BD.
// The implementation above retains the existing simulation/source interface.
module dac_dds_dpll #(
    parameter integer CLK_HZ=65000000,
    parameter ROM_FILE="dds_waveforms.mem"
) (
    input wire clk_i,rst_n_i,cfg_load_i,restart_phase_i,
    input wire signed [31:0] offset_q16_i,
    input wire enable_i,sweep_enable_i,
    input wire [2:0] waveform_sel_i,
    input wire [31:0] freq_tuning_word_i,phase_offset_i,
    input wire [16:0] amplitude_q16_i,
    input wire [31:0] sweep_start_ftw_i,sweep_stop_ftw_i,sweep_step_ftw_i,
    input wire pll_enable_i,pll_active_i,
    input wire [31:0] pll_phase_i,pll_ftw_i,
    output wire [7:0] dac_data_o,
    output wire [31:0] current_ftw_o
);
    dac_dds_feedback #(.CLK_HZ(CLK_HZ),.ROM_FILE(ROM_FILE)) dds_u(
        .clk_i(clk_i),.rst_n_i(rst_n_i),.cfg_load_i(cfg_load_i),.restart_phase_i(restart_phase_i),
        .offset_q16_i(offset_q16_i),.enable_i(enable_i),.sweep_enable_i(sweep_enable_i),
        .waveform_sel_i(waveform_sel_i),.freq_tuning_word_i(freq_tuning_word_i),
        .phase_offset_i(phase_offset_i),.amplitude_q16_i(amplitude_q16_i),
        .sweep_start_ftw_i(sweep_start_ftw_i),.sweep_stop_ftw_i(sweep_stop_ftw_i),
        .sweep_step_ftw_i(sweep_step_ftw_i),.pll_enable_i(pll_enable_i),.pll_active_i(pll_active_i),
        .pll_phase_i(pll_phase_i),.pll_ftw_i(pll_ftw_i),.dac_data_o(dac_data_o),.current_ftw_o(current_ftw_o));
endmodule
