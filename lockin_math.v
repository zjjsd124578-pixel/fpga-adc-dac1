`timescale 1ns/1ps
// Shared iterative arithmetic. IQ is the mean ADC code times a Q15 reference.
// Phase uses signed binary turns: 0x40000000 = +90 degrees.
module lockin_udiv64 (
    input wire clk, rst_n, start,
    input wire [63:0] numerator,
    input wire [31:0] denominator,
    output reg busy, done,
    output reg [63:0] quotient
);
    reg [63:0] bits_r, q_r;
    reg [32:0] rem_r;
    reg [31:0] den_r;
    reg [6:0] count_r;
    wire [32:0] trial = {rem_r[31:0], bits_r[63]};
    wire take = trial >= {1'b0, den_r};
    wire [63:0] next_q = {q_r[62:0], take};
    always @(posedge clk) begin
        if (!rst_n) begin
            busy <= 0; done <= 0; quotient <= 0;
            bits_r <= 0; q_r <= 0; rem_r <= 0; den_r <= 1; count_r <= 0;
        end else begin
            done <= 0;
            if (start && !busy) begin
                bits_r <= numerator; den_r <= denominator; q_r <= 0;
                rem_r <= 0; count_r <= 0; busy <= 1;
            end else if (busy) begin
                bits_r <= bits_r << 1;
                rem_r <= take ? trial - {1'b0, den_r} : trial;
                q_r <= next_q;
                if (count_r == 63) begin
                    quotient <= next_q; busy <= 0; done <= 1;
                end else count_r <= count_r + 1'b1;
            end
        end
    end
endmodule

module lockin_cordic (
    input wire clk, rst_n, start,
    input wire signed [31:0] i_value, q_value,
    output reg done,
    output reg [31:0] vpp_q16,
    output reg signed [31:0] phase_turns
);
    reg [2:0] state;
    reg [4:0] iteration;
    reg signed [35:0] x, y;
    reg signed [31:0] z;
    reg signed [54:0] magnitude_product;
    reg zero_vector;
    wire signed [35:0] i_ext = {{4{i_value[31]}}, i_value};
    wire signed [35:0] q_ext = {{4{q_value[31]}}, q_value};
    wire signed [35:0] xs = x >>> iteration;
    wire signed [35:0] ys = y >>> iteration;
    function [31:0] atan_turns;
        input [4:0] k;
        begin
            case (k)
                0: atan_turns=32'h20000000; 1: atan_turns=32'h12e4051e;
                2: atan_turns=32'h09fb385b; 3: atan_turns=32'h051111d4;
                4: atan_turns=32'h028b0d43; 5: atan_turns=32'h0145d7e1;
                6: atan_turns=32'h00a2f61e; 7: atan_turns=32'h00517c55;
                8: atan_turns=32'h0028be53; 9: atan_turns=32'h00145f2f;
                10: atan_turns=32'h000a2f98; 11: atan_turns=32'h000517cc;
                12: atan_turns=32'h00028be6; 13: atan_turns=32'h000145f3;
                14: atan_turns=32'h0000a2fa; 15: atan_turns=32'h0000517d;
                16: atan_turns=32'h000028be; 17: atan_turns=32'h0000145f;
                18: atan_turns=32'h00000a30; 19: atan_turns=32'h00000518;
                20: atan_turns=32'h0000028c; 21: atan_turns=32'h00000146;
                22: atan_turns=32'h000000a3; 23: atan_turns=32'h00000051;
                default: atan_turns=0;
            endcase
        end
    endfunction
    always @(posedge clk) begin
        if (!rst_n) begin
            state<=0; done<=0; vpp_q16<=0; phase_turns<=0;
            x<=0; y<=0; z<=0; iteration<=0; magnitude_product<=0; zero_vector<=0;
        end else begin
            done<=0;
            case (state)
                0: if (start) begin
                    zero_vector <= (i_value==0 && q_value==0);
                    x <= i_value[31] ? -i_ext : i_ext;
                    y <= i_value[31] ? -q_ext : q_ext;
                    z <= i_value[31] ? 32'h80000000 : 0;
                    iteration<=0; state<=1;
                end
                1: begin
                    if (!y[35]) begin x<=x+ys; y<=y-xs; z<=z+atan_turns(iteration); end
                    else begin x<=x-ys; y<=y+xs; z<=z-atan_turns(iteration); end
                    if (iteration==23) state<=2;
                    else iteration<=iteration+1'b1;
                end
                2: begin
                    // 1/K = 0.607252935, Q18. Vpp in ADC codes Q16 = |IQ| * 8.
                    magnitude_product <= x * 19'sd159187;
                    phase_turns <= zero_vector ? 0 : z; state<=3;
                end
                3: begin
                    vpp_q16 <= zero_vector ? 0 : ((magnitude_product + 55'sd16384) >>> 15);
                    done<=1; state<=0;
                end
                default: state<=0;
            endcase
        end
    end
endmodule

module lockin_math (
    input wire clk, rst_n, start,
    input wire signed [55:0] sum_ia, sum_qa, sum_ib, sum_qb,
    input wire [31:0] sample_count, min_vpp_q16,
    output reg done,
    output reg signed [31:0] ia, qa, ib, qb,
    output reg [31:0] amp_a, amp_b, gain_q16,
    output reg signed [31:0] phase_a, phase_b, phase_diff, phase_mdeg,
    output reg phase_valid, ratio_overflow
);
    reg [3:0] state;
    reg [1:0] index_r;
    reg signed [55:0] sums[0:3];
    reg [31:0] n_r, threshold_r;
    reg sign_r;
    reg div_start, cor_start;
    reg [63:0] div_num;
    reg [31:0] div_den;
    wire div_done, div_busy;
    wire [63:0] div_q;
    reg signed [31:0] cor_i, cor_q;
    wire cor_done;
    wire [31:0] cor_amp;
    wire signed [31:0] cor_phase;
    reg signed [51:0] degrees_product;
    wire signed [63:0] selected_sum = {{8{sums[index_r][55]}}, sums[index_r]};
    wire signed [31:0] signed_mean = sign_r ? -$signed(div_q[31:0]) : $signed(div_q[31:0]);
    lockin_udiv64 div_u(clk,rst_n,div_start,div_num,div_den,div_busy,div_done,div_q);
    lockin_cordic cor_u(clk,rst_n,cor_start,cor_i,cor_q,cor_done,cor_amp,cor_phase);
    always @(posedge clk) begin
        if (!rst_n) begin
            state<=0; index_r<=0; done<=0; div_start<=0; cor_start<=0;
            div_num<=0; div_den<=1; n_r<=1; threshold_r<=0; sign_r<=0;
            cor_i<=0; cor_q<=0; ia<=0; qa<=0; ib<=0; qb<=0;
            amp_a<=0; amp_b<=0; gain_q16<=0; phase_a<=0; phase_b<=0;
            phase_diff<=0; phase_mdeg<=0; phase_valid<=0; ratio_overflow<=0;
            degrees_product<=0; sums[0]<=0; sums[1]<=0; sums[2]<=0; sums[3]<=0;
        end else begin
            done<=0; div_start<=0; cor_start<=0;
            case (state)
                0: if (start) begin
                    sums[0]<=sum_ia; sums[1]<=sum_qa; sums[2]<=sum_ib; sums[3]<=sum_qb;
                    n_r<=sample_count; threshold_r<=min_vpp_q16;
                    index_r<=0; ratio_overflow<=0; phase_valid<=0; state<=1;
                end
                1: begin
                    sign_r<=selected_sum[63];
                    div_num<=selected_sum[63] ? -selected_sum : selected_sum;
                    div_den<=n_r; div_start<=1; state<=2;
                end
                2: if (div_done) begin
                    case (index_r)
                        0: ia<=signed_mean; 1: qa<=signed_mean;
                        2: ib<=signed_mean; 3: qb<=signed_mean;
                    endcase
                    if (index_r==3) state<=3;
                    else begin index_r<=index_r+1'b1; state<=1; end
                end
                3: begin cor_i<=ia; cor_q<=qa; cor_start<=1; state<=4; end
                4: if (cor_done) begin
                    amp_a<=cor_amp; phase_a<=cor_phase;
                    cor_i<=ib; cor_q<=qb; cor_start<=1; state<=5;
                end
                5: if (cor_done) begin
                    amp_b<=cor_amp; phase_b<=cor_phase;
                    phase_diff<=cor_phase-phase_a; state<=6;
                end
                6: begin
                    degrees_product <= phase_diff * 20'sd360000;
                    phase_valid <= (amp_a>=threshold_r && amp_b>=threshold_r);
                    if (amp_a!=0) begin
                        div_num<={16'b0,amp_b,16'b0}; div_den<=amp_a; div_start<=1; state<=7;
                    end else begin gain_q16<=0; state<=8; end
                end
                7: if (div_done) begin
                    ratio_overflow <= |div_q[63:32];
                    gain_q16 <= |div_q[63:32] ? 32'hffffffff : div_q[31:0]; state<=8;
                end
                8: begin phase_mdeg<=degrees_product >>> 32; done<=1; state<=0; end
                default: state<=0;
            endcase
        end
    end
endmodule
