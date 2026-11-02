`timescale 1ns/1ps
// Atomic configuration and frozen DPLL snapshots across AXI and ADC clocks.
// All PLL outputs are synchronous to adc_clk_i; DDS must use that same clock.
module dpll_axi #(
    parameter integer C_S_AXI_ADDR_WIDTH=8
)(
    (* X_INTERFACE_PARAMETER="XIL_INTERFACENAME s_axi_aclk, ASSOCIATED_BUSIF s_axi, ASSOCIATED_RESET s_axi_aresetn" *)
    (* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 s_axi_aclk CLK" *) input wire s_axi_aclk,
    (* X_INTERFACE_PARAMETER="XIL_INTERFACENAME s_axi_aresetn, POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO="xilinx.com:signal:reset:1.0 s_axi_aresetn RST" *) input wire s_axi_aresetn,
    input wire [C_S_AXI_ADDR_WIDTH-1:0] s_axi_awaddr,
    input wire [2:0] s_axi_awprot,
    input wire s_axi_awvalid, output wire s_axi_awready,
    input wire [31:0] s_axi_wdata, input wire [3:0] s_axi_wstrb,
    input wire s_axi_wvalid, output wire s_axi_wready,
    output reg [1:0] s_axi_bresp, output reg s_axi_bvalid, input wire s_axi_bready,
    input wire [C_S_AXI_ADDR_WIDTH-1:0] s_axi_araddr,
    input wire [2:0] s_axi_arprot,
    input wire s_axi_arvalid, output wire s_axi_arready,
    output reg [31:0] s_axi_rdata, output reg [1:0] s_axi_rresp,
    output reg s_axi_rvalid, input wire s_axi_rready,
    (* X_INTERFACE_PARAMETER="XIL_INTERFACENAME adc_clk_i, ASSOCIATED_RESET adc_rst_n_i" *)
    (* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 adc_clk_i CLK" *) input wire adc_clk_i,
    (* X_INTERFACE_PARAMETER="XIL_INTERFACENAME adc_rst_n_i, POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO="xilinx.com:signal:reset:1.0 adc_rst_n_i RST" *) input wire adc_rst_n_i,
    input wire [11:0] adc_a_i,
    input wire sample_valid_i, otr_a_i,
    output wire pll_enable_o, pll_active_o,
    output wire [31:0] pll_phase_o, pll_ftw_o
);
    localparam [255:0] DEFAULT_CFG={32'd0,32'd6,32'd2,32'd65000,
                                  32'd650,32'd8,32'd2048,32'd0};
    wire bridge_arst_n=s_axi_aresetn && adc_rst_n_i;
    (* ASYNC_REG="TRUE" *) reg [1:0] axi_reset_pipe,adc_reset_pipe;
    always @(posedge s_axi_aclk or negedge bridge_arst_n)
        if(!bridge_arst_n) axi_reset_pipe<=0;
        else axi_reset_pipe<={axi_reset_pipe[0],1'b1};
    always @(posedge adc_clk_i or negedge bridge_arst_n)
        if(!bridge_arst_n) adc_reset_pipe<=0;
        else adc_reset_pipe<={adc_reset_pipe[0],1'b1};
    wire axi_reset_n=axi_reset_pipe[1],adc_reset_n=adc_reset_pipe[1];

    reg [31:0] staging[0:6];
    reg [31:0] generation_r;
    // Keep CDC mailbox registers distinct from the staging/generation registers.
    (* DONT_TOUCH="TRUE" *) reg [255:0] cfg_mailbox_r;
    reg [255:0] cfg_adc_r;
    reg cfg_req_r,cfg_ack_r;
    (* ASYNC_REG="TRUE" *) reg cfg_req_meta_adc,cfg_req_sync_adc,cfg_ack_meta_axi,cfg_ack_sync_axi;
    wire cfg_busy=(cfg_req_r!=cfg_ack_sync_axi);
    always @(posedge adc_clk_i) begin
        if(!adc_reset_n) begin
            cfg_req_meta_adc<=0; cfg_req_sync_adc<=0; cfg_ack_r<=0; cfg_adc_r<=DEFAULT_CFG;
        end else begin
            cfg_req_meta_adc<=cfg_req_r; cfg_req_sync_adc<=cfg_req_meta_adc;
            if(cfg_req_sync_adc!=cfg_ack_r) begin
                cfg_adc_r<=cfg_mailbox_r; cfg_ack_r<=cfg_req_sync_adc;
            end
        end
    end
    assign pll_enable_o=adc_reset_n && cfg_adc_r[0];

    wire core_strobe;
    wire [511:0] core_result;
    reg [511:0] result_mailbox_r,result_receive_r,live_r,snapshot_r;
    reg [1:0] result_pending_r;
    reg result_generation_match_r;
    reg result_req_r,result_ack_r,live_present,snapshot_present;
    (* ASYNC_REG="TRUE" *) reg result_req_meta_axi,result_req_sync_axi,result_ack_meta_adc,result_ack_sync_adc;
    dpll_core core_u(
        .clk(adc_clk_i),.rst_n(adc_reset_n),.cfg_i(cfg_adc_r),
        .adc_a_i(adc_a_i),.valid_i(sample_valid_i),.otr_i(otr_a_i),
        .active_o(pll_active_o),.phase_o(pll_phase_o),.ftw_o(pll_ftw_o),
        .result_strobe_o(core_strobe),.result_o(core_result));
    always @(posedge adc_clk_i) begin
        if(!adc_reset_n) begin
            result_req_r<=0; result_mailbox_r<=0;
            result_ack_meta_adc<=0; result_ack_sync_adc<=0;
        end else begin
            result_ack_meta_adc<=result_ack_r; result_ack_sync_adc<=result_ack_meta_adc;
            // If AXI is busy, skip this telemetry sample; never block the loop.
            if(core_strobe && result_req_r==result_ack_sync_adc) begin
                result_mailbox_r<=core_result; result_req_r<=!result_req_r;
            end
        end
    end

    reg aw_hold,w_hold;
    reg [7:0] aw_addr;
    reg [31:0] w_data;
    reg [3:0] w_strb;
    assign s_axi_awready=axi_reset_n && !aw_hold && !s_axi_bvalid;
    assign s_axi_wready=axi_reset_n && !w_hold && !s_axi_bvalid;
    assign s_axi_arready=axi_reset_n && !s_axi_rvalid;
    function [31:0] merge_bytes;
        input [31:0] old_value,new_value;
        input [3:0] strobe;
        integer b;
        begin
            merge_bytes=old_value;
            for(b=0;b<4;b=b+1)
                if(strobe[b]) merge_bytes[b*8+:8]=new_value[b*8+:8];
        end
    endfunction
    wire cfg_legal=(staging[0]<=3 && staging[1]<=4095 &&
        staging[2]>=1 && staging[2]<=1023 && staging[3]>=256 &&
        staging[3]<=staging[4] && staging[4]<=65534 &&
        staging[5]>=1 && staging[5]<=12 &&
        staging[6]>=staging[5]+32'd2 && staging[6]<=20);
    wire [7:0] read_addr=s_axi_araddr;
    reg [31:0] read_value;
    reg read_legal;
    always @* begin
        read_value=0; read_legal=(read_addr[1:0]==0);
        case(read_addr)
            8'h00: read_value=32'h44504c31; // DPL1
            8'h04: read_value=staging[0]; 8'h08: read_value=staging[1];
            8'h0c: read_value=staging[2]; 8'h10: read_value=staging[3];
            8'h14: read_value=staging[4]; 8'h18: read_value=staging[5];
            8'h1c: read_value=staging[6]; 8'h20: read_value=0;
            8'h24: read_value={28'b0,(snapshot_present && snapshot_r[66]),
                                    snapshot_present,live_present,cfg_busy};
            8'h28: read_value=0; 8'h2c: read_value=generation_r;
            default: begin
                if(read_addr>=8'h40 && read_addr<=8'h7c)
                    read_value=snapshot_r[((read_addr-8'h40)>>2)*32+:32];
                else read_legal=0;
            end
        endcase
    end
    always @(posedge s_axi_aclk) begin
        if(!axi_reset_n) begin
            staging[0]<=0; staging[1]<=2048; staging[2]<=8; staging[3]<=650;
            staging[4]<=65000; staging[5]<=2; staging[6]<=6;
            generation_r<=0; cfg_mailbox_r<=DEFAULT_CFG; cfg_req_r<=0;
            cfg_ack_meta_axi<=0; cfg_ack_sync_axi<=0;
            result_req_meta_axi<=0; result_req_sync_axi<=0; result_ack_r<=0;
            live_r<=0; snapshot_r<=0; live_present<=0; snapshot_present<=0;
            result_receive_r<=0; result_pending_r<=0; result_generation_match_r<=0;
            aw_hold<=0; w_hold<=0; aw_addr<=0; w_data<=0; w_strb<=0;
            s_axi_bvalid<=0; s_axi_bresp<=0;
            s_axi_rvalid<=0; s_axi_rdata<=0; s_axi_rresp<=0;
        end else begin
            cfg_ack_meta_axi<=cfg_ack_r; cfg_ack_sync_axi<=cfg_ack_meta_axi;
            result_req_meta_axi<=result_req_r; result_req_sync_axi<=result_req_meta_axi;
            // Capture CDC data first, compare generation locally, then publish.
            // Never drive an AXI status enable directly from the ADC mailbox.
            case(result_pending_r)
                0: if(result_req_sync_axi!=result_ack_r) begin
                    result_receive_r<=result_mailbox_r; result_pending_r<=1;
                end
                1: begin
                    result_generation_match_r<=(result_receive_r[63:32]==generation_r);
                    result_pending_r<=2;
                end
                2: begin
                    if(result_generation_match_r) begin
                        live_r<=result_receive_r; live_present<=1;
                    end
                    result_ack_r<=result_req_sync_axi; result_pending_r<=0;
                end
                default: result_pending_r<=0;
            endcase
            if(s_axi_awvalid && s_axi_awready) begin
                aw_addr<=s_axi_awaddr; aw_hold<=1;
            end
            if(s_axi_wvalid && s_axi_wready) begin
                w_data<=s_axi_wdata; w_strb<=s_axi_wstrb; w_hold<=1;
            end
            if(s_axi_bvalid && s_axi_bready) s_axi_bvalid<=0;
            if(aw_hold && w_hold && !s_axi_bvalid) begin
                aw_hold<=0; w_hold<=0; s_axi_bvalid<=1; s_axi_bresp<=0;
                if(aw_addr[1:0]!=0) s_axi_bresp<=2;
                else case(aw_addr)
                    8'h04,8'h08,8'h0c,8'h10,8'h14,8'h18,8'h1c:
                        staging[(aw_addr>>2)-1]<=merge_bytes(staging[(aw_addr>>2)-1],w_data,w_strb);
                    8'h20: if(w_strb[0] && w_data[0]) begin
                        if(cfg_busy || !cfg_legal) s_axi_bresp<=2;
                        else begin
                            cfg_mailbox_r<={generation_r+32'd1,staging[6],staging[5],staging[4],
                                staging[3],staging[2],staging[1],staging[0]};
                            cfg_req_r<=!cfg_req_r; generation_r<=generation_r+1;
                            live_present<=0; snapshot_present<=0; live_r<=0; snapshot_r<=0;
                            result_pending_r<=0;
                        end
                    end
                    8'h28: if(w_strb[0] && w_data[0]) begin
                        if(!live_present) s_axi_bresp<=2;
                        else begin snapshot_r<=live_r; snapshot_present<=1; end
                    end
                    default: s_axi_bresp<=2;
                endcase
            end
            if(s_axi_rvalid && s_axi_rready) s_axi_rvalid<=0;
            if(s_axi_arvalid && s_axi_arready) begin
                s_axi_rvalid<=1; s_axi_rdata<=read_value;
                s_axi_rresp<=read_legal ? 0 : 2;
            end
        end
    end
endmodule
