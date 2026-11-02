`timescale 1ns/1ps
// Runs exclusively on the raw ADC sampling clock; no preview decimation.
module lockin_core #(
    parameter ROM_FILE = "lockin_sine.mem"
)(
    input wire clk, rst_n,
    input wire [255:0] cfg_i,
    input wire [32:0] dds_reference_i,
    input wire [11:0] adc_a_i, adc_b_i,
    input wire valid_i, otr_a_i, otr_b_i,
    output reg result_strobe_o,
    output reg [511:0] result_o
);
    wire [31:0] control=cfg_i[31:0];
    wire [31:0] ftw=control[1] ? dds_reference_i[31:0] : cfg_i[63:32];
    wire enabled=control[0] && (!control[1] || dds_reference_i[32]);
    wire [31:0] cycles=cfg_i[95:64], min_samples=cfg_i[127:96];
    wire [31:0] settle_samples=cfg_i[159:128], max_samples=cfg_i[191:160];
    wire [31:0] min_amp=cfg_i[223:192], generation=cfg_i[255:224];
    wire unsupported=(ftw==0 || ftw>32'h40000000);
    reg [31:0] last_ftw, last_gen;
    reg last_enabled;
    wire changed=(ftw!=last_ftw || generation!=last_gen || enabled!=last_enabled);
    wire math_reset_n=rst_n && !changed && enabled && !unsupported;

    reg signed [15:0] sine_rom[0:1023];
    initial $readmemh(ROM_FILE,sine_rom);
    reg [31:0] phase;
    wire [32:0] phase_next={1'b0,phase}+{1'b0,ftw};
    wire [9:0] cos_address=phase[31:22]+10'd256;
    reg signed [15:0] cos_p1, neg_sin_p1;
    reg signed [12:0] a_p1,b_p1;
    (* use_dsp="yes" *) reg signed [28:0] ia_p2,qa_p2,ib_p2,qb_p2;
    reg valid_p1,valid_p2,wrap_p1,wrap_p2;
    reg clip_a_p1,clip_b_p1,clip_a_p2,clip_b_p2;
    wire [11:0] rail_a=control[2] ? (adc_a_i^12'h800) : adc_a_i;
    wire [11:0] rail_b=control[2] ? (adc_b_i^12'h800) : adc_b_i;
    always @(posedge clk) begin
        if (!rst_n || changed) begin
            phase<=0; valid_p1<=0; valid_p2<=0; wrap_p1<=0; wrap_p2<=0;
            cos_p1<=0; neg_sin_p1<=0; a_p1<=0; b_p1<=0;
            ia_p2<=0; qa_p2<=0; ib_p2<=0; qb_p2<=0;
            clip_a_p1<=0; clip_b_p1<=0; clip_a_p2<=0; clip_b_p2<=0;
        end else begin
            phase<=phase_next[31:0];
            cos_p1<=sine_rom[cos_address]; neg_sin_p1<=-sine_rom[phase[31:22]];
            a_p1<=control[2] ? $signed({adc_a_i[11],adc_a_i}) : $signed({1'b0,adc_a_i})-13'sd2048;
            b_p1<=control[2] ? $signed({adc_b_i[11],adc_b_i}) : $signed({1'b0,adc_b_i})-13'sd2048;
            ia_p2<=a_p1*cos_p1; qa_p2<=a_p1*neg_sin_p1;
            ib_p2<=b_p1*cos_p1; qb_p2<=b_p1*neg_sin_p1;
            valid_p1<=valid_i; valid_p2<=valid_p1;
            wrap_p1<=phase_next[32]; wrap_p2<=wrap_p1;
            clip_a_p1<=otr_a_i || rail_a<=4 || rail_a>=4091;
            clip_b_p1<=otr_b_i || rail_b<=4 || rail_b>=4091;
            clip_a_p2<=clip_a_p1; clip_b_p2<=clip_b_p1;
        end
    end
    reg [2:0] state;
    reg [31:0] settle_count,n,cycle_count,sequence_r;
    reg signed [55:0] sum_ia,sum_qa,sum_ib,sum_qb;
    reg clip_a,clip_b,timed_out,math_start;
    wire math_done,phase_valid,ratio_overflow;
    wire signed [31:0] mean_ia,mean_qa,mean_ib,mean_qb;
    wire [31:0] amp_a,amp_b,gain;
    wire signed [31:0] phase_a,phase_b,phase_diff,phase_mdeg;
    wire terminal_sample=(n+1>=max_samples || (wrap_p2 && cycle_count+1>=cycles && n+1>=min_samples));
    lockin_math math_u(
        .clk(clk),.rst_n(math_reset_n),.start(math_start),
        .sum_ia(sum_ia),.sum_qa(sum_qa),.sum_ib(sum_ib),.sum_qb(sum_qb),
        .sample_count(n),.min_vpp_q16(min_amp),.done(math_done),
        .ia(mean_ia),.qa(mean_qa),.ib(mean_ib),.qb(mean_qb),
        .amp_a(amp_a),.amp_b(amp_b),.gain_q16(gain),
        .phase_a(phase_a),.phase_b(phase_b),.phase_diff(phase_diff),.phase_mdeg(phase_mdeg),
        .phase_valid(phase_valid),.ratio_overflow(ratio_overflow));
    wire [31:0] flags={23'b0,ratio_overflow,
        (phase_valid && !clip_a && !clip_b && !timed_out),1'b0,timed_out,
        clip_b,clip_a,(amp_b>=min_amp),(amp_a>=min_amp),1'b1};
    always @(posedge clk) begin
        if (!rst_n) begin
            last_ftw<=0; last_gen<=0; last_enabled<=0; state<=0;
            settle_count<=0; n<=0; cycle_count<=0; sequence_r<=0;
            sum_ia<=0; sum_qa<=0; sum_ib<=0; sum_qb<=0;
            clip_a<=0; clip_b<=0; timed_out<=0; math_start<=0;
            result_strobe_o<=0; result_o<=0;
        end else begin
            last_ftw<=ftw; last_gen<=generation; last_enabled<=enabled;
            result_strobe_o<=0; math_start<=0;
            if (changed) begin
                state<=0; settle_count<=0; n<=0; cycle_count<=0;
                sum_ia<=0; sum_qa<=0; sum_ib<=0; sum_qb<=0;
                clip_a<=0; clip_b<=0; timed_out<=0;
                // Explicit invalid record after a reference/configuration change.
                sequence_r<=sequence_r+1;
                result_o<={352'b0,32'b0,(unsupported ? 32'h40 : 32'b0),ftw,generation,sequence_r+32'd1};
                result_strobe_o<=1;
            end else if (!enabled || unsupported) begin
                state<=0; settle_count<=0;
            end else case(state)
                0: begin
                    sum_ia<=0; sum_qa<=0; sum_ib<=0; sum_qb<=0;
                    n<=0; cycle_count<=0; clip_a<=0; clip_b<=0; timed_out<=0;
                    if (valid_p2) begin
                        if (settle_count>=settle_samples) state<=1;
                        else settle_count<=settle_count+1;
                    end else settle_count<=0;
                end
                1: if (valid_p2 && wrap_p2) state<=2;
                2: if (!valid_p2) begin state<=0; settle_count<=0; end
                   else begin
                       sum_ia<=sum_ia+{{27{ia_p2[28]}},ia_p2};
                       sum_qa<=sum_qa+{{27{qa_p2[28]}},qa_p2};
                       sum_ib<=sum_ib+{{27{ib_p2[28]}},ib_p2};
                       sum_qb<=sum_qb+{{27{qb_p2[28]}},qb_p2};
                       n<=n+1; if(wrap_p2) cycle_count<=cycle_count+1;
                       clip_a<=clip_a || clip_a_p2; clip_b<=clip_b || clip_b_p2;
                       if(terminal_sample) begin
                           timed_out<=!(wrap_p2 && cycle_count+1>=cycles && n+1>=min_samples);
                           state<=3;
                       end
                   end
                3: begin math_start<=1; state<=4; end
                4: if(math_done) begin
                    sequence_r<=sequence_r+1;
                    result_o<={mean_qb,mean_ib,mean_qa,mean_ia,gain,phase_mdeg,phase_diff,
                        phase_b,phase_a,amp_b,amp_a,n,flags,ftw,generation,sequence_r+32'd1};
                    result_strobe_o<=1; state<=0; settle_count<=settle_samples;
                end
                default: state<=0;
            endcase
        end
    end
endmodule
