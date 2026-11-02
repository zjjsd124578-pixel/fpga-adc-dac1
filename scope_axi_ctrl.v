// 示波�? AXI4-Lite 控制寄存�?
//
// 寄存器地�?（相�? AXI 基地�?）：
// 0x00 CONTROL：bit0=arm，bit1=manual_trigger，bit2=trigger_enable�?
//                 bit3=trigger_channel�?0=CH_A�?1=CH_B），bit4=trigger_edge
//                 �?0=上升沿，1=下降沿）
// 0x04 TRIGGER_LEVEL：bit[11:0] �? ADC 触发阈�??
// 0x08 STATUS：bit0=capture_active，bit1=triggered，bit2=capture_done�?
//              bit3=stream_overflow
//
// 注意：trigger_level、trigger_channel、trigger_edge 应在 arm=0 时配置�??
// 它们是多位配置数据，应保持稳定到�?次采集完成�??
module scope_axi_ctrl #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 6
) (
    // AXI4-Lite 从机接口，连�? PS M_AXI_GP0�?
    input  wire                             s_axi_aclk,     // AXI-Lite 时钟，�?�常�? PS �? FCLK_CLK0�?
    input  wire                             s_axi_aresetn,  // AXI-Lite 低有效复位�??
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]    s_axi_awaddr,   // PS 写寄存器时给出的地址偏移�?
    input  wire                             s_axi_awvalid,  // PS 表示“写地址有效”�??
    output wire                             s_axi_awready,  // Write address ready
    input  wire [C_S_AXI_DATA_WIDTH-1:0]    s_axi_wdata,    // PS 要写入寄存器�? 32 位数据�??
    input  wire [C_S_AXI_DATA_WIDTH/8-1:0]  s_axi_wstrb,    // 写字节使能；每一位对�? wdata 的一个字节�??
    input  wire                             s_axi_wvalid,   // PS 表示“写数据有效”�??
    output wire                             s_axi_wready,   // Write data ready
    output reg  [1:0]                       s_axi_bresp,    // 写响应，2'b00 表示写入成功（OKAY）�??
    output reg                              s_axi_bvalid,   // 本模块表示�?�写响应有效”�??
    input  wire                             s_axi_bready,   // PS 表示“已接收写响应�?��??
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]    s_axi_araddr,   // PS 读寄存器时给出的地址偏移�?
    input  wire                             s_axi_arvalid,  // PS 表示“读地址有效”�??
    output reg                              s_axi_arready,  // 本模块表示�?�已接收读地�?”�??
    output reg  [C_S_AXI_DATA_WIDTH-1:0]    s_axi_rdata,    // 返回�? PS �? 32 位寄存器数据�?
    output reg  [1:0]                       s_axi_rresp,    // 读响应，2'b00 表示读取成功（OKAY）�??
    output reg                              s_axi_rvalid,   // 本模块表示�?�读数据有效”�??
    input  wire                             s_axi_rready,   // PS 表示“已接收读数据�?��??

    // 输出�? ADC 采集、触发模块的控制信号�?
    output wire                             arm_o,             // bit0：启�?/布防�?次采集�??
    output wire                             manual_trigger_o,  // bit1：手动触发电平，送给 trigger_detect�?
    output wire                             trigger_enable_o,  // bit2：是否允许自动阈值触发�??
    output wire                             trigger_channel_o, // bit3：自动触发�?�道�?0=CH_A�?1=CH_B�?
    output wire                             trigger_edge_o,    // bit4：触发边沿，0=上升沿，1=下降沿�??
    output wire [11:0]                      trigger_level_o,   // 自动触发�? 12 位阈值�??
    output wire [31:0]                      post_trigger_samples_o,

    // 来自 ADC 时钟域的状�?�信号�??
    input  wire                             capture_active_i,  // ADC 域：当前是否正在采集�?
    input  wire                             triggered_i,       // ADC 域：本次采集是否已触发�??
    input  wire                             capture_done_i,    // ADC 域：后触发数据是否已输出完毕�?
    input  wire [31:0]                      completed_frames_gray_i,
    input  wire                             stream_overflow_i  // ADC 域：FIFO 反压导致的丢样标志�??
);

    reg [C_S_AXI_ADDR_WIDTH-1:0] axi_awaddr;          // 已锁存的写地�?，用于�?�择内部寄存器�??
    reg [C_S_AXI_ADDR_WIDTH-1:0] axi_araddr;          // 已锁存的读地�?，用于�?�择返回数据�?
    reg [31:0]                   control_reg;         // CONTROL 寄存器，保存 arm、手动触发等位�??
    reg [11:0]                   trigger_level_reg;   // TRIGGER_LEVEL 寄存器，保存 12 位阈值�??
    reg [31:0]                   post_trigger_samples_reg;
    reg                          aw_pending;
    reg                          w_pending;
    reg [C_S_AXI_DATA_WIDTH-1:0] wdata_pending;
    reg [C_S_AXI_DATA_WIDTH/8-1:0] wstrb_pending;

    // 状�?�信号从 ADC 时钟域同步到 AXI/PS 时钟域�??
    (* ASYNC_REG = "TRUE" *) reg capture_active_meta;
    (* ASYNC_REG = "TRUE" *) reg capture_active_sync;
    (* ASYNC_REG = "TRUE" *) reg triggered_meta;
    (* ASYNC_REG = "TRUE" *) reg triggered_sync;
    (* ASYNC_REG = "TRUE" *) reg capture_done_meta;
    (* ASYNC_REG = "TRUE" *) reg capture_done_sync;
    reg capture_done_visible;
    // The source counter is Gray encoded: only one bit changes per frame.
    (* ASYNC_REG = "TRUE" *) reg [31:0] completed_frames_gray_meta;
    (* ASYNC_REG = "TRUE" *) reg [31:0] completed_frames_gray_sync;
    reg [31:0] completed_frames_gray_latched;
    (* ASYNC_REG = "TRUE" *) reg stream_overflow_meta;
    (* ASYNC_REG = "TRUE" *) reg stream_overflow_sync;

    wire write_enable;                               // �?�? AXI 写地�?和写数据握手均完成时�? 1�?

    assign s_axi_awready = s_axi_aresetn && !aw_pending && !s_axi_bvalid;
    assign s_axi_wready  = s_axi_aresetn && !w_pending && !s_axi_bvalid;
    assign write_enable = aw_pending && w_pending && !s_axi_bvalid;

    assign arm_o             = control_reg[0];
    assign manual_trigger_o  = control_reg[1];
    assign trigger_enable_o  = control_reg[2];
    assign trigger_channel_o = control_reg[3];
    assign trigger_edge_o    = control_reg[4];
    assign trigger_level_o   = trigger_level_reg;
    assign post_trigger_samples_o = post_trigger_samples_reg;

    // AXI-Lite write address and data channels must be accepted independently.
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            axi_awaddr    <= {C_S_AXI_ADDR_WIDTH{1'b0}};
            aw_pending    <= 1'b0;
        end else begin
            if (s_axi_awready && s_axi_awvalid) begin
                axi_awaddr    <= s_axi_awaddr;
                aw_pending    <= 1'b1;
            end else if (write_enable)
                aw_pending <= 1'b0;
        end
    end

    // Latch data and byte strobes at the independent W-channel handshake.
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            w_pending     <= 1'b0;
            wdata_pending <= {C_S_AXI_DATA_WIDTH{1'b0}};
            wstrb_pending <= {C_S_AXI_DATA_WIDTH/8{1'b0}};
        end else if (s_axi_wready && s_axi_wvalid) begin
            w_pending     <= 1'b1;
            wdata_pending <= s_axi_wdata;
            wstrb_pending <= s_axi_wstrb;
        end else if (write_enable)
            w_pending <= 1'b0;
    end

    // 控制寄存器写入�??
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            control_reg       <= 32'd0;
            trigger_level_reg <= 12'd0;
            post_trigger_samples_reg <= 32'd2048;
        end else if (write_enable) begin
            case (axi_awaddr[5:2])
                4'h0: begin
                    // CONTROL 的低 8 位位于第 0 个字节�??
                    if (wstrb_pending[0])
                        control_reg[7:0] <= wdata_pending[7:0];
                end

                4'h1: begin
                    // TRIGGER_LEVEL 的低 12 位分布在低两个字节�??
                    if (wstrb_pending[0])
                        trigger_level_reg[7:0]  <= wdata_pending[7:0];
                    if (wstrb_pending[1])
                        trigger_level_reg[11:8] <= wdata_pending[11:8];
                end

                4'h4: begin
                    if (wstrb_pending[0])
                        post_trigger_samples_reg[7:0] <= wdata_pending[7:0];
                    if (wstrb_pending[1])
                        post_trigger_samples_reg[15:8] <= wdata_pending[15:8];
                    if (wstrb_pending[2])
                        post_trigger_samples_reg[23:16] <= wdata_pending[23:16];
                    if (wstrb_pending[3])
                        post_trigger_samples_reg[31:24] <= wdata_pending[31:24];
                end

                default: begin
                end
            endcase
        end
    end

    // AXI 写响应�?�道�?
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_bvalid <= 1'b0;
            s_axi_bresp  <= 2'b00;
        end else begin
            if (write_enable && !s_axi_bvalid) begin
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;  // OKAY
            end else if (s_axi_bvalid && s_axi_bready) begin
                s_axi_bvalid <= 1'b0;
            end
        end
    end

    // AXI 读地�?通道�?
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_arready <= 1'b0;
            axi_araddr    <= {C_S_AXI_ADDR_WIDTH{1'b0}};
        end else begin
            if (!s_axi_arready && s_axi_arvalid && !s_axi_rvalid) begin
                s_axi_arready <= 1'b1;
                axi_araddr    <= s_axi_araddr;
            end else begin
                s_axi_arready <= 1'b0;
            end
        end
    end

    // AXI 读数据和读响应�??
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_rvalid <= 1'b0;
            s_axi_rresp  <= 2'b00;
            s_axi_rdata  <= 32'd0;
        end else begin
            if (s_axi_arready && s_axi_arvalid && !s_axi_rvalid) begin
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b00;  // OKAY

                case (s_axi_araddr[5:2])
                    4'h0: s_axi_rdata <= control_reg;
                    4'h1: s_axi_rdata <= {20'd0, trigger_level_reg};
                    4'h2: s_axi_rdata <= {28'd0,
                                           stream_overflow_sync,
                                           capture_done_visible,
                                           triggered_sync,
                                           capture_active_sync};
                    4'h3: s_axi_rdata <= completed_frames_gray_latched;
                    4'h4: s_axi_rdata <= post_trigger_samples_reg;
                    default: s_axi_rdata <= 32'd0;
                endcase
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

    // 采集状�?�跨时钟域同步；状�?�必须在采集完成前后保持足够长时间�??
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            capture_active_meta <= 1'b0;
            capture_active_sync <= 1'b0;
            triggered_meta      <= 1'b0;
            triggered_sync      <= 1'b0;
            capture_done_meta   <= 1'b0;
            capture_done_sync   <= 1'b0;
            capture_done_visible <= 1'b0;
            completed_frames_gray_meta <= 32'd0;
            completed_frames_gray_sync <= 32'd0;
            completed_frames_gray_latched <= 32'd0;
            stream_overflow_meta <= 1'b0;
            stream_overflow_sync <= 1'b0;
        end else begin
            capture_active_meta <= capture_active_i;
            capture_active_sync <= capture_active_meta;
            triggered_meta      <= triggered_i;
            triggered_sync      <= triggered_meta;
            capture_done_meta   <= capture_done_i;
            capture_done_sync   <= capture_done_meta;
            capture_done_visible <= capture_done_sync;
            completed_frames_gray_meta <= completed_frames_gray_i;
            completed_frames_gray_sync <= completed_frames_gray_meta;
            if (capture_done_sync)
                completed_frames_gray_latched <= completed_frames_gray_sync;
            stream_overflow_meta <= stream_overflow_i;
            stream_overflow_sync <= stream_overflow_meta;
        end
    end

endmodule
