`timescale 1ns/1ps
// Atomic configuration and frozen measurement snapshots across three clocks.
module lockin_axi #(
    parameter ROM_FILE="lockin_sine.mem",
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
    (* X_INTERFACE_PARAMETER="XIL_INTERFACENAME dds_clk_i, ASSOCIATED_RESET dds_rst_n_i" *)
    (* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 dds_clk_i CLK" *) input wire dds_clk_i,
    (* X_INTERFACE_PARAMETER="XIL_INTERFACENAME dds_rst_n_i, POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO="xilinx.com:signal:reset:1.0 dds_rst_n_i RST" *) input wire dds_rst_n_i,
    input wire [11:0] adc_a_i,adc_b_i,
    input wire sample_valid_i,otr_a_i,otr_b_i,
    input wire [31:0] dds_ftw_i, input wire dds_enable_i
);
    localparam [255:0] DEFAULT_CFG={32'd0,32'd524288,32'd16777216,32'd65000,
                                  32'd65000,32'd32,32'd660764,32'd2};
    wire bridge_arst_n=s_axi_aresetn && adc_rst_n_i && dds_rst_n_i;
    (* ASYNC_REG="TRUE" *) reg [1:0] axi_reset_pipe,adc_reset_pipe,dds_reset_pipe;
    always @(posedge s_axi_aclk or negedge bridge_arst_n)
        if(!bridge_arst_n) axi_reset_pipe<=0; else axi_reset_pipe<={axi_reset_pipe[0],1'b1};
    always @(posedge adc_clk_i or negedge bridge_arst_n)
        if(!bridge_arst_n) adc_reset_pipe<=0; else adc_reset_pipe<={adc_reset_pipe[0],1'b1};
    always @(posedge dds_clk_i or negedge bridge_arst_n)
        if(!bridge_arst_n) dds_reset_pipe<=0; else dds_reset_pipe<={dds_reset_pipe[0],1'b1};
    wire axi_reset_n=axi_reset_pipe[1],adc_reset_n=adc_reset_pipe[1],dds_reset_n=dds_reset_pipe[1];

    reg [31:0] staging[0:6];
    reg [31:0] generation_r;
    // Preserve mailbox identity: merging generation bits into generation_r would
    // move those CDC paths outside the mailbox timing constraints.
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

    reg [32:0] ref_mailbox_r,ref_adc_r;
    reg ref_req_r,ref_ack_r;
    (* ASYNC_REG="TRUE" *) reg ref_req_meta_adc,ref_req_sync_adc,ref_ack_meta_dds,ref_ack_sync_dds;
    always @(posedge dds_clk_i) begin
        if(!dds_reset_n) begin
            ref_ack_meta_dds<=0; ref_ack_sync_dds<=0; ref_req_r<=0; ref_mailbox_r<=0;
        end else begin
            ref_ack_meta_dds<=ref_ack_r; ref_ack_sync_dds<=ref_ack_meta_dds;
            if(ref_req_r==ref_ack_sync_dds && ref_mailbox_r!={dds_enable_i,dds_ftw_i}) begin
                ref_mailbox_r<={dds_enable_i,dds_ftw_i}; ref_req_r<=!ref_req_r;
            end
        end
    end
    always @(posedge adc_clk_i) begin
        if(!adc_reset_n) begin
            ref_req_meta_adc<=0; ref_req_sync_adc<=0; ref_ack_r<=0; ref_adc_r<=0;
        end else begin
            ref_req_meta_adc<=ref_req_r; ref_req_sync_adc<=ref_req_meta_adc;
            if(ref_req_sync_adc!=ref_ack_r) begin ref_adc_r<=ref_mailbox_r; ref_ack_r<=ref_req_sync_adc; end
        end
    end

    wire core_strobe;
    wire [511:0] core_result;
    reg [511:0] result_mailbox_r,result_receive_r,live_r,snapshot_r;
    reg [1:0] result_pending_r;
    reg result_generation_match_r;
    reg result_req_r,result_ack_r,live_present,snapshot_present;
    (* ASYNC_REG="TRUE" *) reg result_req_meta_axi,result_req_sync_axi,result_ack_meta_adc,result_ack_sync_adc;
    lockin_core #(.ROM_FILE(ROM_FILE)) core_u(
        .clk(adc_clk_i),.rst_n(adc_reset_n),.cfg_i(cfg_adc_r),.dds_reference_i(ref_adc_r),
        .adc_a_i(adc_a_i),.adc_b_i(adc_b_i),.valid_i(sample_valid_i),
        .otr_a_i(otr_a_i),.otr_b_i(otr_b_i),.result_strobe_o(core_strobe),.result_o(core_result));
    always @(posedge adc_clk_i) begin
        if(!adc_reset_n) begin
            result_req_r<=0; result_mailbox_r<=0; result_ack_meta_adc<=0; result_ack_sync_adc<=0;
        end else begin
            result_ack_meta_adc<=result_ack_r; result_ack_sync_adc<=result_ack_meta_adc;
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
        input [31:0] old_value,new_value; input [3:0] strobe;
        integer b;
        begin
            merge_bytes=old_value;
            for(b=0;b<4;b=b+1) if(strobe[b]) merge_bytes[b*8+:8]=new_value[b*8+:8];
        end
    endfunction
    wire cfg_legal=(staging[0]<=7 && staging[2]>=1 && staging[2]<=65535 &&
        staging[3]>=64 && staging[3]<=staging[5] && staging[5]<=16777216 &&
        staging[4]<=16777216 && staging[6]>=1 && staging[6]<=32'h0fff0000);
    wire [7:0] read_addr=s_axi_araddr;
    reg [31:0] read_value;
    reg read_legal;
    always @* begin
        read_value=0; read_legal=(read_addr[1:0]==0);
        case(read_addr)
            8'h00: read_value=32'h4c4b4931;
            8'h04: read_value=staging[0]; 8'h08: read_value=staging[1];
            8'h0c: read_value=staging[2]; 8'h10: read_value=staging[3];
            8'h14: read_value=staging[4]; 8'h18: read_value=staging[5];
            8'h1c: read_value=staging[6]; 8'h20: read_value=0;
            8'h24: read_value={28'b0,(snapshot_present && snapshot_r[103]),snapshot_present,live_present,cfg_busy};
            8'h28: read_value=0; 8'h2c: read_value=generation_r;
            default: begin
                if(read_addr>=8'h40 && read_addr<=8'h7c)
                    read_value=snapshot_r[((read_addr-8'h40)>>2)*32+:32];
                else read_legal=0;
            end
        endcase
    end
    integer k;
    always @(posedge s_axi_aclk) begin
        if(!axi_reset_n) begin
            staging[0]<=2; staging[1]<=660764; staging[2]<=32; staging[3]<=65000;
            staging[4]<=65000; staging[5]<=16777216; staging[6]<=524288;
            generation_r<=0; cfg_mailbox_r<=DEFAULT_CFG; cfg_req_r<=0;
            cfg_ack_meta_axi<=0; cfg_ack_sync_axi<=0;
            result_req_meta_axi<=0; result_req_sync_axi<=0; result_ack_r<=0;
            live_r<=0; snapshot_r<=0; live_present<=0; snapshot_present<=0;
            result_receive_r<=0; result_pending_r<=0; result_generation_match_r<=0;
            aw_hold<=0; w_hold<=0; aw_addr<=0; w_data<=0; w_strb<=0;
            s_axi_bvalid<=0; s_axi_bresp<=0; s_axi_rvalid<=0; s_axi_rdata<=0; s_axi_rresp<=0;
        end else begin
            cfg_ack_meta_axi<=cfg_ack_r; cfg_ack_sync_axi<=cfg_ack_meta_axi;
            result_req_meta_axi<=result_req_r; result_req_sync_axi<=result_req_meta_axi;
            // First capture the stable mailbox, then compare its generation in
            // the AXI domain, then publish. No ADC bus drives an AXI status CE.
            case(result_pending_r)
                0: if(result_req_sync_axi!=result_ack_r) begin
                    result_receive_r<=result_mailbox_r; result_pending_r<=1;
                end
                1: begin
                    result_generation_match_r<=(result_receive_r[63:32]==generation_r);
                    result_pending_r<=2;
                end
                2: begin
                    if(result_generation_match_r) begin live_r<=result_receive_r; live_present<=1; end
                    result_ack_r<=result_req_sync_axi; result_pending_r<=0;
                end
                default: result_pending_r<=0;
            endcase
            if(s_axi_awvalid && s_axi_awready) begin aw_addr<=s_axi_awaddr; aw_hold<=1; end
            if(s_axi_wvalid && s_axi_wready) begin w_data<=s_axi_wdata; w_strb<=s_axi_wstrb; w_hold<=1; end
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
                s_axi_rvalid<=1; s_axi_rdata<=read_value; s_axi_rresp<=read_legal ? 0 : 2;
            end
        end
    end
endmodule
