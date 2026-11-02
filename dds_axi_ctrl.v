`timescale 1ns/1ps
// AXI4-Lite register bank + atomic request/ack configuration bridge.
// Keep the complete mailbox stable until the DDS domain acknowledges it.
// Register map is shared with dds_firmware/dds_ctrl.h.
module dds_axi_ctrl_feedback #(
    parameter integer C_S_AXI_ADDR_WIDTH = 6
) (
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF S_AXI, ASSOCIATED_RESET s_axi_aresetn" *)
    input wire s_axi_aclk,
    input wire s_axi_aresetn,
    input wire [C_S_AXI_ADDR_WIDTH-1:0] s_axi_awaddr,
    input wire s_axi_awvalid,
    output wire s_axi_awready,
    input wire [31:0] s_axi_wdata,
    input wire [3:0] s_axi_wstrb,
    input wire s_axi_wvalid,
    output wire s_axi_wready,
    output reg [1:0] s_axi_bresp,
    output reg s_axi_bvalid,
    input wire s_axi_bready,
    input wire [C_S_AXI_ADDR_WIDTH-1:0] s_axi_araddr,
    input wire s_axi_arvalid,
    output wire s_axi_arready,
    output reg [31:0] s_axi_rdata,
    output reg [1:0] s_axi_rresp,
    output reg s_axi_rvalid,
    input wire s_axi_rready,
    input wire dds_clk_i,
    input wire dds_rst_n_i,
    output wire dds_reset_n_o,
    output reg cfg_load_o,
    output wire restart_phase_o,
    output wire signed [31:0] offset_q16_o,
    output wire enable_o,
    output wire sweep_enable_o,
    output wire [2:0] waveform_sel_o,
    output wire [31:0] freq_tuning_word_o,
    output wire [31:0] phase_offset_o,
    output wire [31:0] sweep_start_ftw_o,
    output wire [31:0] sweep_stop_ftw_o,
    output wire [16:0] amplitude_q16_o,
    output wire [31:0] sweep_step_ftw_o
);
    // Reset either side resets the entire handshake. Release is synchronized
    // independently in each clock domain; neither domain may reset alone.
    wire bridge_arst_n = s_axi_aresetn && dds_rst_n_i;
    (* ASYNC_REG = "TRUE" *) reg [1:0] axi_reset_pipe, dds_reset_pipe;
    always @(posedge s_axi_aclk or negedge bridge_arst_n)
        if (!bridge_arst_n) axi_reset_pipe <= 0;
        else axi_reset_pipe <= {axi_reset_pipe[0], 1'b1};
    always @(posedge dds_clk_i or negedge bridge_arst_n)
        if (!bridge_arst_n) dds_reset_pipe <= 0;
        else dds_reset_pipe <= {dds_reset_pipe[0], 1'b1};
    wire axi_reset_n = axi_reset_pipe[1];
    wire dds_reset_n = dds_reset_pipe[1];
    assign dds_reset_n_o = dds_reset_n;

    reg [31:0] control_r, wave_r, ftw_r, phase_r, start_r, stop_r, step_r;
    reg [31:0] amplitude_r;
    reg signed [31:0] offset_r;
    wire signed [31:0] offset_write = merge_bytes(offset_r, wdata_r, wstrb_r);
    wire [31:0] amplitude_write = merge_bytes(amplitude_r, wdata_r, wstrb_r);
    reg [214:0] mailbox_r;
    reg request_r;
    reg acknowledge_r;
    (* ASYNC_REG = "TRUE" *) reg ack_meta_r, ack_sync_r;
    (* ASYNC_REG = "TRUE" *) reg req_meta_r, req_sync_r;
    reg [214:0] cfg_dds_r;
    wire busy = request_r != ack_sync_r;

    // Preserve legacy top.v behavior: E takes precedence over S; E0/S1 sweeps.
    assign restart_phase_o = cfg_dds_r[214];
    assign offset_q16_o = cfg_dds_r[213:182];
    assign amplitude_q16_o = cfg_dds_r[181:165];
    assign enable_o = cfg_dds_r[163] || cfg_dds_r[164];
    assign sweep_enable_o = cfg_dds_r[164] && !cfg_dds_r[163];
    assign waveform_sel_o = cfg_dds_r[162:160];
    assign freq_tuning_word_o = cfg_dds_r[159:128];
    assign phase_offset_o = cfg_dds_r[127:96];
    assign sweep_start_ftw_o = cfg_dds_r[95:64];
    assign sweep_stop_ftw_o = cfg_dds_r[63:32];
    assign sweep_step_ftw_o = cfg_dds_r[31:0];

    always @(posedge dds_clk_i or negedge dds_reset_n) begin
        if (!dds_reset_n) begin
            req_meta_r <= 0; req_sync_r <= 0;
            acknowledge_r <= 0; cfg_dds_r <= 0; cfg_load_o <= 0;
        end else begin
            req_meta_r <= request_r;
            req_sync_r <= req_meta_r;
            cfg_load_o <= 0;
            if (cfg_load_o) begin
                // dac_dds_amplitude consumes the previous cycle's cfg_load_o now.
                acknowledge_r <= req_sync_r;
            end else if (req_sync_r != acknowledge_r) begin
                cfg_dds_r <= mailbox_r;
                cfg_load_o <= 1;
            end
        end
    end
    always @(posedge s_axi_aclk or negedge axi_reset_n) begin
        if (!axi_reset_n) begin ack_meta_r <= 0; ack_sync_r <= 0; end
        else begin ack_meta_r <= acknowledge_r; ack_sync_r <= ack_meta_r; end
    end

    reg aw_pending_r, w_pending_r;
    reg [C_S_AXI_ADDR_WIDTH-1:0] awaddr_r;
    reg [31:0] wdata_r;
    reg [3:0] wstrb_r;
    assign s_axi_awready = axi_reset_n && !aw_pending_r && !s_axi_bvalid;
    assign s_axi_wready = axi_reset_n && !w_pending_r && !s_axi_bvalid;
    assign s_axi_arready = axi_reset_n && !s_axi_rvalid;
    function [31:0] merge_bytes;
        input [31:0] old_value, new_value;
        input [3:0] strobes;
        integer j;
        begin
            merge_bytes = old_value;
            for (j=0; j<4; j=j+1)
                if (strobes[j]) merge_bytes[j*8 +: 8] = new_value[j*8 +: 8];
        end
    endfunction
    always @(posedge s_axi_aclk or negedge axi_reset_n) begin
        if (!axi_reset_n) begin
            control_r <= 0; wave_r <= 1; ftw_r <= 100000;
            phase_r <= 0; start_r <= 0; stop_r <= 0; step_r <= 0;
            amplitude_r <= 32'd65536; offset_r <= 0;
            mailbox_r <= 0; request_r <= 0;
            aw_pending_r <= 0; w_pending_r <= 0;
            awaddr_r <= 0; wdata_r <= 0; wstrb_r <= 0;
            s_axi_bvalid <= 0; s_axi_bresp <= 0;
        end else begin
            if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 0;
            if (s_axi_awvalid && s_axi_awready) begin
                awaddr_r <= s_axi_awaddr; aw_pending_r <= 1;
            end
            if (s_axi_wvalid && s_axi_wready) begin
                wdata_r <= s_axi_wdata; wstrb_r <= s_axi_wstrb; w_pending_r <= 1;
            end
            if (aw_pending_r && w_pending_r && !s_axi_bvalid) begin
                aw_pending_r <= 0; w_pending_r <= 0;
                s_axi_bvalid <= 1; s_axi_bresp <= 0;
                if (awaddr_r[1:0] != 0) s_axi_bresp <= 2'b10;
                else case (awaddr_r)
                    'h04: if (wstrb_r[0] && wdata_r[0]) begin
                        if (busy) s_axi_bresp <= 2'b10;
                        else begin
                            mailbox_r <= {wdata_r[1], offset_r, amplitude_r[16:0], control_r[1:0], wave_r[2:0], ftw_r,
                                          phase_r, start_r, stop_r, step_r};
                            request_r <= ~request_r;
                        end
                    end
                    'h08: control_r <= merge_bytes(control_r, wdata_r, wstrb_r);
                    'h0c: wave_r <= merge_bytes(wave_r, wdata_r, wstrb_r);
                    'h10: ftw_r <= merge_bytes(ftw_r, wdata_r, wstrb_r);
                    'h14: phase_r <= merge_bytes(phase_r, wdata_r, wstrb_r);
                    'h18: start_r <= merge_bytes(start_r, wdata_r, wstrb_r);
                    'h1c: stop_r <= merge_bytes(stop_r, wdata_r, wstrb_r);
                    'h20: step_r <= merge_bytes(step_r, wdata_r, wstrb_r);
                    'h28: if (amplitude_write <= 65536) amplitude_r <= amplitude_write;
                          else s_axi_bresp <= 2'b10;
                    'h2c: if (offset_write >= -32'sd8388608 && offset_write <= 32'sd8323072)
                              offset_r <= offset_write;
                          else s_axi_bresp <= 2'b10;
                    default: s_axi_bresp <= 2'b10;
                endcase
            end
        end
    end
    always @(posedge s_axi_aclk or negedge axi_reset_n) begin
        if (!axi_reset_n) begin
            s_axi_rvalid <= 0; s_axi_rdata <= 0; s_axi_rresp <= 0;
        end else begin
            if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 0;
            if (s_axi_arvalid && s_axi_arready) begin
                s_axi_rvalid <= 1; s_axi_rresp <= 0; s_axi_rdata <= 0;
                if (s_axi_araddr[1:0] != 0) s_axi_rresp <= 2'b10;
                else case (s_axi_araddr)
                    'h00: s_axi_rdata <= {31'd0, busy};
                    'h04: s_axi_rdata <= 0;
                    'h08: s_axi_rdata <= control_r;
                    'h0c: s_axi_rdata <= wave_r;
                    'h10: s_axi_rdata <= ftw_r;
                    'h14: s_axi_rdata <= phase_r;
                    'h18: s_axi_rdata <= start_r;
                    'h1c: s_axi_rdata <= stop_r;
                    'h20: s_axi_rdata <= step_r;
                    'h24: s_axi_rdata <= 32'h44445333; // DDS3: continuous feedback updates and signed bias
                    'h28: s_axi_rdata <= amplitude_r;
                    'h2c: s_axi_rdata <= offset_r;
                    default: s_axi_rresp <= 2'b10;
                endcase
            end
        end
    end
endmodule
