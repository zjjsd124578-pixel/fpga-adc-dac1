`timescale 1ns/1ps
// Event-driven fixed-point incremental PI. ADC and DDS share this clock.
// A fresh coherent amplitude estimate is the only control update event.
// e_gain = gain * (target-measured)/measured estimates the DAC gain error.
// Delta gain = Kp*(e_gain-previous_e_gain) + Ki*e_gain.
module amplitude_pi (
    input wire clk,rst_n,enabled,
    input wire [31:0] generation,
    input wire [31:0] target_q16,seed_gain,min_gain,max_gain,tolerance_q16,
    input wire [31:0] stable_windows,kp_shift,ki_shift,
    input wire reference_changed,measurement_strobe,measurement_good,measurement_clipped,
    input wire [31:0] measured_q16,
    output reg [16:0] gain_o,
    output reg gain_update_o,
    output reg [31:0] flags_o,stable_count_o,updates_o,
    output reg signed [31:0] error_gain_o
);
    reg [31:0] last_generation;
    reg signed [31:0] previous_error;
    reg [31:0] measured_r;
    reg negative_r,start_r;
    reg [63:0] numerator_r;
    wire div_busy,div_done;
    wire [63:0] quotient;
    wire changed=generation!=last_generation;
    wire div_reset=rst_n && enabled && !changed && !reference_changed;
    wire [31:0] magnitude=target_q16>measured_q16 ? target_q16-measured_q16 : measured_q16-target_q16;
    wire within_tolerance=magnitude<=tolerance_q16;
    wire [63:0] next_numerator=magnitude*gain_o;
    lockin_udiv64 divider_u(.clk(clk),.rst_n(div_reset),.start(start_r),
        .numerator(numerator_r),.denominator(measured_r),
        .busy(div_busy),.done(div_done),.quotient(quotient));
    reg [2:0] state;
    reg signed [31:0] error_r;
    reg signed [33:0] delta_r,requested_r;
    reg [31:0] limited_r;
    wire signed [33:0] current_gain=$signed({17'b0,gain_o});
    wire signed [33:0] lower_slew=current_gain-34'sd4096;
    wire signed [33:0] upper_slew=current_gain+34'sd4096;
    wire signed [33:0] error_ext={{2{error_r[31]}},error_r};
    wire signed [33:0] previous_ext={{2{previous_error[31]}},previous_error};
    wire signed [33:0] bounded_request=requested_r<$signed({2'b0,min_gain}) ? $signed({2'b0,min_gain}) :
        (requested_r>$signed({2'b0,max_gain}) ? $signed({2'b0,max_gain}) : requested_r);
    wire signed [33:0] slew_request=bounded_request<lower_slew ? lower_slew :
        (bounded_request>upper_slew ? upper_slew : bounded_request);
    always @(posedge clk) begin
        if(!rst_n) begin
            last_generation<=0;gain_o<=0;gain_update_o<=0;flags_o<=0;
            stable_count_o<=0;updates_o<=0;error_gain_o<=0;previous_error<=0;
            measured_r<=1;negative_r<=0;start_r<=0;numerator_r<=0;
            state<=0;error_r<=0;delta_r<=0;requested_r<=0;limited_r<=0;
        end else begin
            gain_update_o<=0;start_r<=0;
            if(changed || !enabled) begin
                last_generation<=generation;state<=0;previous_error<=0;
                stable_count_o<=0;flags_o<=enabled ? 32'h41 : 0;
                if(changed) begin gain_o<=seed_gain[16:0];gain_update_o<=enabled;updates_o<=0; end
            end else if(reference_changed) begin
                state<=0;previous_error<=0;stable_count_o<=0;flags_o<=32'h41;
            end else if(measurement_strobe && state==0) begin
                if(measurement_clipped) begin
                    stable_count_o<=0;previous_error<=0;flags_o<=32'h11;
                    // Reduce output progressively; never grow from a clipped estimate.
                    if(gain_o>min_gain) begin
                        gain_o <= gain_o>min_gain+4096 ? gain_o-17'd4096 : min_gain[16:0];
                        gain_update_o<=1;updates_o<=updates_o+1;
                    end
                end else if(!measurement_good || measured_q16==0) begin
                    stable_count_o<=0;previous_error<=0;flags_o<=32'h09;
                    // Missing/weak feedback freezes gain; firmware aborts the point.
                end else if(within_tolerance) begin
                    flags_o<=32'h01;
                    if(stable_count_o<stable_windows) stable_count_o<=stable_count_o+1;
                    if(stable_count_o+1>=stable_windows) flags_o<=32'h03;
                    previous_error<=0;error_gain_o<=0;
                end else begin
                    measured_r<=measured_q16;numerator_r<=next_numerator;
                    negative_r<=measured_q16>target_q16;
                    start_r<=1;state<=1;stable_count_o<=0;flags_o<=32'h21;
                end
            end else case(state)
                1: if(div_done) begin
                    // Limit pathological errors before signed arithmetic.
                    error_r<=negative_r ? -$signed(quotient>65536 ? 32'd65536 : quotient[31:0]) :
                                              $signed(quotient>65536 ? 32'd65536 : quotient[31:0]);
                    state<=2;
                end
                2: begin
                    delta_r<=((error_ext-previous_ext) >>> kp_shift[4:0])+(error_ext >>> ki_shift[4:0]);
                    error_gain_o<=error_r;previous_error<=error_r;state<=3;
                end
                3: begin requested_r<=current_gain+delta_r;state<=4;end
                4: begin
                    limited_r<=slew_request<$signed({2'b0,min_gain}) ? min_gain :
                        (slew_request>$signed({2'b0,max_gain}) ? max_gain : slew_request[31:0]);
                    flags_o<= (requested_r<$signed({2'b0,min_gain}) || requested_r>$signed({2'b0,max_gain})) ? 5 : 1;
                    state<=5;
                end
                5: begin
                    if(limited_r[16:0]!=gain_o) begin
                        gain_o<=limited_r[16:0];gain_update_o<=1;updates_o<=updates_o+1;
                    end
                    state<=0;
                end
                default: ;
            endcase
        end
    end
endmodule
