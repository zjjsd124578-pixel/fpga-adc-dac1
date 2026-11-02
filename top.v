// DAC 控制器的主顶层模块。
module top (
    input clk_50m, input rst_n, input uart_rx,
    output uart_tx, output [7:0] dac_out, output clk_125m
);
    localparam integer CMD_BUF_DEPTH = 16;
    localparam [1:0] REPLY_IDLE = 2'd0, REPLY_O = 2'd1, REPLY_K = 2'd2;

    reg [7:0] dac_data_r, tx_data_r;
    reg tx_start_r;
    wire [7:0] uart_data;
    wire uart_valid, tx_busy;

    // 50 MHz 配置邮箱：在收到确认前保持内容不变。
    reg waveform_en_50m;
    reg [2:0] waveform_sel_50m;
    reg [31:0] freq_tuning_word_50m, phase_offset_50m;
    reg sweep_en_50m;
    reg [31:0] sweep_start_ftw_50m, sweep_stop_ftw_50m, sweep_step_ftw_50m;
    reg cfg_req_50m, cfg_ack_meta_50m, cfg_ack_sync_50m, cfg_inflight_50m;
    wire cfg_busy_50m = (cfg_req_50m != cfg_ack_sync_50m);

    // 125 MHz DDS 域使用的配置寄存器。
    reg waveform_en_r;
    reg [2:0] waveform_sel_r;
    reg [31:0] freq_tuning_word_r, phase_offset_r;
    reg sweep_en_r;
    reg [31:0] sweep_start_ftw_r, sweep_stop_ftw_r, sweep_step_ftw_r;
    reg cfg_req_meta_125m, cfg_req_sync_125m, cfg_req_last_125m, cfg_ack_125m;

    reg [7:0] cmd_buf [0:CMD_BUF_DEPTH-1];
    reg [4:0] cmd_len, cmd_parse_len;
    reg parse_pending;
    reg [31:0] parsed_value;
    integer i;

    reg [7:0] ok_count;
    reg [1:0] reply_state;
    wire reply_push = cfg_inflight_50m && !cfg_busy_50m;
    wire reply_pop = (reply_state == REPLY_K) && !tx_busy && !tx_start_r;

    rom_PLL pll_inst (.inclk0(clk_50m), .c0(clk_125m));
    wire rst_50m_n, rst_125m_n;
    reset_sync reset_50m_inst  (.clk(clk_50m),  .arst_n(rst_n), .srst_n(rst_50m_n));
    reset_sync reset_125m_inst (.clk(clk_125m), .arst_n(rst_n), .srst_n(rst_125m_n));
    uart_rx uart_rx_inst (.clk_50m(clk_50m), .rst_n(rst_50m_n), .uart_rx_i(uart_rx), .rx_data(uart_data), .rx_valid(uart_valid));
    uart_tx uart_tx_inst (.clk_50m(clk_50m), .rst_n(rst_50m_n), .tx_start(tx_start_r), .tx_data(tx_data_r), .tx_o(uart_tx), .tx_busy(tx_busy));

    always @(posedge clk_50m or negedge rst_50m_n) begin
        if (!rst_50m_n) begin
            waveform_en_50m <= 1'b1; waveform_sel_50m <= 3'd1;
            freq_tuning_word_50m <= 32'd100000; phase_offset_50m <= 32'd0;
            sweep_en_50m <= 1'b0; sweep_start_ftw_50m <= 32'd0;
            sweep_stop_ftw_50m <= 32'd0; sweep_step_ftw_50m <= 32'd0;
            cfg_req_50m <= 1'b0; cfg_ack_meta_50m <= 1'b0; cfg_ack_sync_50m <= 1'b0; cfg_inflight_50m <= 1'b0;
            cmd_len <= 5'd0; cmd_parse_len <= 5'd0; parse_pending <= 1'b0; parsed_value <= 32'd0;
            for (i = 0; i < CMD_BUF_DEPTH; i = i + 1) cmd_buf[i] <= 8'd0;
            tx_start_r <= 1'b0; tx_data_r <= 8'd0; ok_count <= 8'd0; reply_state <= REPLY_IDLE;
        end else begin
            cfg_ack_meta_50m <= cfg_ack_125m;
            cfg_ack_sync_50m <= cfg_ack_meta_50m;
            tx_start_r <= 1'b0;
            if (cfg_inflight_50m && !cfg_busy_50m) cfg_inflight_50m <= 1'b0;

            // 命令由一个 ASCII 字母、十进制数字和结束分隔符组成。
            if (uart_valid) begin
                if ((uart_data == 8'h3d) || (uart_data == 8'h3a) ||
                    (((uart_data == 8'h20) || (uart_data == 8'h09)) && (cmd_len == 5'd1))) begin
                    // 命令字母后的可选分隔符：'='、':'、空格或制表符。
                end else if ((uart_data == 8'h0d) || (uart_data == 8'h0a) || (uart_data == 8'h20) || (uart_data == 8'h09) || (uart_data == 8'h3b)) begin
                    if ((cmd_len != 5'd0) && !parse_pending) begin
                        cmd_parse_len <= cmd_len;
                        parse_pending <= 1'b1;
                    end
                    cmd_len <= 5'd0;
                end else if (cmd_len == 5'd0) begin
                    if (((uart_data >= "A") && (uart_data <= "Z")) || ((uart_data >= "a") && (uart_data <= "z"))) begin
                        cmd_buf[0] <= uart_data;
                        cmd_len <= 5'd1;
                    end
                end else if ((uart_data >= "0") && (uart_data <= "9")) begin
                    if (cmd_len < CMD_BUF_DEPTH) begin
                        cmd_buf[cmd_len[3:0]] <= uart_data;
                        cmd_len <= cmd_len + 1'b1;
                    end
                end else if (!parse_pending) begin
                    cmd_parse_len <= cmd_len;
                    parse_pending <= 1'b1;
                    cmd_len <= 5'd0;
                end
            end

            // 上一次请求/确认事务未完成时，不修改配置邮箱。
            if (parse_pending && !cfg_busy_50m) begin
                parsed_value = 32'd0;
                for (i = 1; i < CMD_BUF_DEPTH; i = i + 1)
                    if ((i < cmd_parse_len) && (cmd_buf[i] >= "0") && (cmd_buf[i] <= "9"))
                        parsed_value = parsed_value * 32'd10 + (cmd_buf[i] - "0");
                case (cmd_buf[0])
                    "W", "w": waveform_sel_50m     <= parsed_value[2:0];
                    "F", "f": freq_tuning_word_50m <= parsed_value;
                    "P", "p": phase_offset_50m     <= parsed_value;
                    "E", "e": waveform_en_50m      <= parsed_value[0];
                    "S", "s": sweep_en_50m         <= parsed_value[0];
                    "B", "b": sweep_start_ftw_50m  <= parsed_value;
                    "C", "c": sweep_stop_ftw_50m   <= parsed_value;
                    "D", "d": sweep_step_ftw_50m   <= parsed_value;
                    default: begin end
                endcase
                if ((cmd_buf[0] == "W") || (cmd_buf[0] == "w") || (cmd_buf[0] == "F") || (cmd_buf[0] == "f") ||
                    (cmd_buf[0] == "P") || (cmd_buf[0] == "p") || (cmd_buf[0] == "E") || (cmd_buf[0] == "e") ||
                    (cmd_buf[0] == "S") || (cmd_buf[0] == "s") || (cmd_buf[0] == "B") || (cmd_buf[0] == "b") ||
                    (cmd_buf[0] == "C") || (cmd_buf[0] == "c") || (cmd_buf[0] == "D") || (cmd_buf[0] == "d")) begin
                    cfg_req_50m <= ~cfg_req_50m;
                    cfg_inflight_50m <= 1'b1;
                end
                parse_pending <= 1'b0;
            end

            // 仅在 125 MHz 域已接收命令后，才将一条应答加入队列。
            case ({reply_push, reply_pop})
                2'b10: if (ok_count != 8'hff) ok_count <= ok_count + 1'b1;
                2'b01: if (ok_count != 8'd0) ok_count <= ok_count - 1'b1;
                default: ok_count <= ok_count;
            endcase
            case (reply_state)
                REPLY_IDLE: if ((ok_count != 8'd0) && !tx_busy && !tx_start_r) begin
                    tx_data_r <= "O"; tx_start_r <= 1'b1; reply_state <= REPLY_O;
                end
                REPLY_O: if (!tx_busy && !tx_start_r) begin
                    tx_data_r <= "K"; tx_start_r <= 1'b1; reply_state <= REPLY_K;
                end
                REPLY_K: if (!tx_busy && !tx_start_r) reply_state <= REPLY_IDLE;
                default: reply_state <= REPLY_IDLE;
            endcase
        end
    end

    // 使用两级触发器同步事件，然后原子性地捕获配置邮箱。
    always @(posedge clk_125m or negedge rst_125m_n) begin
        if (!rst_125m_n) begin
            cfg_req_meta_125m <= 1'b0; cfg_req_sync_125m <= 1'b0; cfg_req_last_125m <= 1'b0; cfg_ack_125m <= 1'b0;
            waveform_en_r <= 1'b1; waveform_sel_r <= 3'd1; freq_tuning_word_r <= 32'd100000; phase_offset_r <= 32'd0;
            sweep_en_r <= 1'b0; sweep_start_ftw_r <= 32'd0; sweep_stop_ftw_r <= 32'd0; sweep_step_ftw_r <= 32'd0;
        end else begin
            cfg_req_meta_125m <= cfg_req_50m;
            cfg_req_sync_125m <= cfg_req_meta_125m;
            cfg_req_last_125m <= cfg_req_sync_125m;
            if (cfg_req_sync_125m != cfg_req_last_125m) begin
                waveform_en_r <= waveform_en_50m; waveform_sel_r <= waveform_sel_50m;
                freq_tuning_word_r <= freq_tuning_word_50m; phase_offset_r <= phase_offset_50m;
                sweep_en_r <= sweep_en_50m; sweep_start_ftw_r <= sweep_start_ftw_50m;
                sweep_stop_ftw_r <= sweep_stop_ftw_50m; sweep_step_ftw_r <= sweep_step_ftw_50m;
                cfg_ack_125m <= cfg_req_sync_125m;
            end
        end
    end

    wire [12:0] rom_addr;
    wire [9:0] phase_addr;
    wire [7:0] rom_data;
    dds dds_inst (
        .clk_125m(clk_125m), .rst_n(rst_125m_n), .waveform_en(waveform_en_r), .sweep_en(sweep_en_r),
        .waveform_sel(waveform_sel_r), .freq_tuning_word(freq_tuning_word_r), .phase_offset(phase_offset_r),
        .sweep_start_ftw(sweep_start_ftw_r), .sweep_stop_ftw(sweep_stop_ftw_r), .sweep_step_ftw(sweep_step_ftw_r),
        .phase_addr(phase_addr), .rom_addr(rom_addr)
    );
    rom_ip rom_inst (.address(rom_addr), .clock(clk_125m), .q(rom_data));
    always @(posedge clk_125m or negedge rst_125m_n) begin
        if (!rst_125m_n) dac_data_r <= 8'd128;
        // ROM 数据本身就是无符号 8 位 DAC 码，直接寄存以保留完整的 0 至 255 范围。
        else dac_data_r <= rom_data;
    end
    assign dac_out = dac_data_r;
endmodule

module reset_sync (input wire clk, input wire arst_n, output reg srst_n);
    reg [1:0] release_pipe;
    always @(posedge clk or negedge arst_n) begin
        if (!arst_n) begin release_pipe <= 2'b00; srst_n <= 1'b0; end
        else begin release_pipe <= {release_pipe[0], 1'b1}; srst_n <= release_pipe[1]; end
    end
endmodule
