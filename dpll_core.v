`timescale 1ns/1ps
// ADC-referenced type-II digital PLL. All ports belong to the ADC clock domain.
// Independent ADC A rising zero crossings drive the detector, never DAC data.
// phase_o is a sine phase (zero = rising crossing), before the DAC pipeline.
module dpll_core (
    input wire clk,rst_n,
    input wire [255:0] cfg_i,
    input wire [11:0] adc_a_i,
    input wire valid_i,otr_i,
    output wire active_o,
    output wire [31:0] phase_o,ftw_o,
    output reg result_strobe_o,
    output wire [511:0] result_o
);
    wire enabled=cfg_i[0];
    wire twos=cfg_i[1];
    wire [11:0] zero_code=cfg_i[43:32];
    wire [11:0] hysteresis=cfg_i[75:64];
    wire [31:0] min_period=cfg_i[127:96],max_period=cfg_i[159:128];
    wire [4:0] kp_shift=cfg_i[164:160],ki_shift=cfg_i[196:192];
    wire [31:0] generation=cfg_i[255:224];
    // Convert two's complement to offset binary before comparing the center.
    wire [11:0] code=twos ? (adc_a_i ^ 12'h800) : adc_a_i;
    wire signed [12:0] sample=$signed({1'b0,code})-$signed({1'b0,zero_code});
    wire clipped=otr_i || code<=4 || code>=4091;
    reg [31:0] generation_r,clock_r,phase_r,ftw_r,base_ftw_r;
    reg signed [47:0] integral_q16_r;
    reg active_r,locked_r,armed_r,last_edge_valid_r,prev_valid_r;
    reg signed [12:0] prev_sample_r;
    reg [31:0] prev_phase_r,since_edge_r,last_edge_clock_r;
    reg [15:0] last_fraction_r;
    reg [31:0] seq_r,edges_r,lock_count_r,flags_r,period_q16_r;
    reg signed [31:0] phase_error_r;
    reg [3:0] state_r;
    reg abort_r,div_start_r;
    reg [63:0] div_num_r;
    reg [31:0] div_den_r;
    wire div_busy,div_done;
    wire [63:0] div_q;
    lockin_udiv64 divider_u(.clk(clk),.rst_n(rst_n && !abort_r),
        .start(div_start_r),.numerator(div_num_r),.denominator(div_den_r),
        .busy(div_busy),.done(div_done),.quotient(div_q));

    reg [31:0] edge_clock_r,edge_phase_r,edge_ftw_r;
    reg [15:0] fraction_r;
    reg [31:0] measured_ftw_r;
    reg signed [31:0] normalized_error_r;
    reg [47:0] fraction_product_r;
    reg signed [47:0] next_integral_r;
    reg signed [33:0] next_base_r,command_r;
    reg [31:0] next_ftw_r;
    reg seed_r,step_r;
    reg [31:0] seed_elapsed_r;
    reg [63:0] seed_product_r;
    reg [31:0] snapshot[0:15];
    genvar s;
    generate for(s=0;s<16;s=s+1) begin: result_words
        assign result_o[s*32+:32]=snapshot[s];
    end endgenerate
    assign active_o=active_r && enabled;
    assign phase_o=phase_r;
    assign ftw_o=ftw_r;
    wire crossing=valid_i && prev_valid_r && armed_r && prev_sample_r<=0 && sample>0;
    wire [13:0] delta_sample=$signed(sample)-$signed(prev_sample_r);
    wire [31:0] period_clocks=edge_clock_r-last_edge_clock_r;
    wire signed [48:0] fractional_period=$signed({1'b0,period_clocks,16'b0})+
        $signed({33'b0,div_q[15:0]})-$signed({33'b0,last_fraction_r});
    wire [31:0] phase_at_edge=edge_phase_r+fraction_product_r[47:16];
    wire signed [31:0] wrapped_error=-$signed(phase_at_edge);
    wire [31:0] error_magnitude=phase_error_r[31] ? -phase_error_r : phase_error_r;
    wire [31:0] frequency_difference=(measured_ftw_r>ftw_r) ? measured_ftw_r-ftw_r : ftw_r-measured_ftw_r;
    wire [31:0] acquisition_difference=(measured_ftw_r>base_ftw_r) ? measured_ftw_r-base_ftw_r : base_ftw_r-measured_ftw_r;
    wire big_step=active_r && acquisition_difference>(base_ftw_r>>3);
    wire signed [47:0] error_q16={{16{normalized_error_r[31]}},normalized_error_r} <<< 16;
    // Bound integral to +/- 1/8 of nominal frequency; keep its fractional FTW.
    wire signed [47:0] integral_limit=$signed({base_ftw_r,16'b0}) >>> 3;
    wire signed [47:0] integral_candidate=integral_q16_r+(error_q16 >>> ki_shift);
    wire signed [33:0] frequency_floor=$signed({2'b0,32'h00010000});
    // At least 256 clocks/cycle: the shared divider finishes before the next edge.
    wire signed [33:0] frequency_ceiling=34'sh001000000;
    integer k;
    task publish;
        input [31:0] flags;
        begin
            seq_r<=seq_r+1'b1; result_strobe_o<=1;
            snapshot[0]<=seq_r+1'b1; snapshot[1]<=generation;
            snapshot[2]<=flags; snapshot[3]<=ftw_r; snapshot[4]<=period_q16_r;
            snapshot[5]<=phase_error_r; snapshot[6]<=integral_q16_r >>> 16;
            snapshot[7]<=lock_count_r; snapshot[8]<=edges_r;
            snapshot[9]<={20'b0,zero_code}; snapshot[10]<=phase_r;
            for(k=11;k<16;k=k+1) snapshot[k]<=0;
        end
    endtask
    always @(posedge clk) begin
        if(!rst_n) begin
            generation_r<=0;clock_r<=0;phase_r<=0;ftw_r<=0;base_ftw_r<=0;
            integral_q16_r<=0;active_r<=0;locked_r<=0;armed_r<=0;
            last_edge_valid_r<=0;prev_valid_r<=0;prev_sample_r<=0;prev_phase_r<=0;
            since_edge_r<=0;last_edge_clock_r<=0;last_fraction_r<=0;
            seq_r<=0;edges_r<=0;lock_count_r<=0;flags_r<=0;period_q16_r<=0;
            phase_error_r<=0;state_r<=0;abort_r<=0;div_start_r<=0;div_num_r<=0;div_den_r<=1;
            edge_clock_r<=0;edge_phase_r<=0;edge_ftw_r<=0;fraction_r<=0;measured_ftw_r<=0;
            normalized_error_r<=0;fraction_product_r<=0;next_integral_r<=0;
            next_base_r<=0;command_r<=0;next_ftw_r<=0;seed_r<=0;step_r<=0;
            seed_elapsed_r<=0;seed_product_r<=0;result_strobe_o<=0;
            for(k=0;k<16;k=k+1) snapshot[k]<=0;
        end else begin
            clock_r<=clock_r+1'b1;phase_r<=phase_r+ftw_r;
            prev_sample_r<=sample;prev_phase_r<=phase_r;prev_valid_r<=valid_i;
            result_strobe_o<=0;div_start_r<=0;abort_r<=0;
            if(since_edge_r<32'hffffffff) since_edge_r<=since_edge_r+1'b1;
            if(sample<-$signed({1'b0,hysteresis})) armed_r<=1;
            if(!enabled || generation!=generation_r) begin
                generation_r<=generation;active_r<=0;locked_r<=0;armed_r<=0;
                last_edge_valid_r<=0;prev_valid_r<=0;since_edge_r<=0;
                base_ftw_r<=0;ftw_r<=0;phase_r<=0;integral_q16_r<=0;
                lock_count_r<=0;flags_r<={31'b0,enabled};state_r<=0;abort_r<=1;
                if(generation!=generation_r) publish({31'b0,enabled});
            end else if(!valid_i || clipped) begin
                active_r<=0;locked_r<=0;armed_r<=0;last_edge_valid_r<=0;
                lock_count_r<=0;state_r<=0;abort_r<=1;integral_q16_r<=0;
                flags_r<=clipped ? 32'h09 : 32'h41;
                if(active_r || locked_r) publish(clipped ? 32'h09 : 32'h41);
            end else if(since_edge_r>(max_period<<2)) begin
                active_r<=0;locked_r<=0;last_edge_valid_r<=0;lock_count_r<=0;
                state_r<=0;abort_r<=1;integral_q16_r<=0;flags_r<=32'h11;
                if(!(flags_r & 32'h10)) publish(32'h11);
                // A fresh crossing may restart acquisition on the next clock.
                if(crossing) begin since_edge_r<=0;armed_r<=0; end
            end else begin
                if(crossing) begin
                    armed_r<=0;since_edge_r<=0;
                    if(state_r==0 && !div_busy && !abort_r) begin
                        edge_clock_r<=clock_r-1'b1;edge_phase_r<=prev_phase_r;edge_ftw_r<=ftw_r;
                        div_num_r<={35'b0,(-prev_sample_r),16'b0};
                        div_den_r<={18'b0,delta_sample};div_start_r<=1;state_r<=1;
                        edges_r<=edges_r+1'b1;
                    end else begin
                        // An unexpected extra edge cannot be silently ignored.
                        active_r<=0;locked_r<=0;lock_count_r<=0;last_edge_valid_r<=0;
                        state_r<=0;abort_r<=1;flags_r<=32'h21;publish(32'h21);
                    end
                end
                if(!crossing) case(state_r)
                    1: if(div_done) begin
                        fraction_r<=div_q[15:0];fraction_product_r<=edge_ftw_r*div_q[15:0];
                        last_edge_clock_r<=edge_clock_r;last_fraction_r<=div_q[15:0];
                        last_edge_valid_r<=1;
                        if(!last_edge_valid_r) state_r<=0;
                        else if(fractional_period<($signed({17'b0,min_period,16'b0})-49'sd65536) ||
                                fractional_period>($signed({17'b0,max_period,16'b0})+49'sd65536) ||
                                fractional_period<=0 || fractional_period>49'h0ffffffff) begin
                            active_r<=0;locked_r<=0;lock_count_r<=0;flags_r<=32'h21;
                            state_r<=0;integral_q16_r<=0;publish(32'h21);
                        end else begin
                            period_q16_r<=fractional_period[31:0];
                            div_num_r<=64'h0001000000000000;div_den_r<=fractional_period[31:0];
                            div_start_r<=1;state_r<=2;
                        end
                    end
                    2: if(div_done) begin
                        measured_ftw_r<=div_q[31:0];phase_error_r<=wrapped_error;
                        div_num_r<={16'b0,(wrapped_error[31] ? -wrapped_error : wrapped_error),16'b0};
                        div_den_r<=period_q16_r;div_start_r<=1;state_r<=3;
                    end
                    3: if(div_done) begin
                        normalized_error_r<=phase_error_r[31] ? -$signed(div_q[31:0]) : $signed(div_q[31:0]);
                        seed_r<=!active_r || big_step;step_r<=big_step;state_r<=4;
                    end
                    4: begin
                        next_base_r<=seed_r ? $signed({2'b0,measured_ftw_r}) :
                            $signed({2'b0,base_ftw_r})+
                            (($signed({2'b0,measured_ftw_r})-$signed({2'b0,base_ftw_r})) >>> 3);
                        next_integral_r<=seed_r ? 0 :
                            (integral_candidate>integral_limit ? integral_limit :
                            (integral_candidate < -integral_limit ? -integral_limit : integral_candidate));
                        state_r<=5;
                    end
                    5: begin
                        command_r<=next_base_r+(next_integral_r >>> 16)+
                            (seed_r ? 34'sd0 : ($signed(normalized_error_r) >>> kp_shift));
                        seed_elapsed_r<=clock_r-edge_clock_r+32'd3;state_r<=6;
                    end
                    6: begin
                        next_ftw_r<=(command_r<frequency_floor) ? frequency_floor[31:0] :
                            ((command_r>frequency_ceiling) ? frequency_ceiling[31:0] : command_r[31:0]);
                        seed_product_r<=seed_elapsed_r*measured_ftw_r;
                        fraction_product_r<=measured_ftw_r*fraction_r;state_r<=7;
                    end
                    7: begin
                        base_ftw_r<=next_base_r[31:0];integral_q16_r<=next_integral_r;
                        ftw_r<=next_ftw_r;active_r<=1;flags_r<=3;
                        if(seed_r) begin
                            phase_r<=seed_product_r[31:0]-fraction_product_r[47:16];
                            lock_count_r<=0;locked_r<=0;
                        end else if(error_magnitude<32'h02000000 && frequency_difference<((measured_ftw_r>>8)+1)) begin
                            if(lock_count_r<8) lock_count_r<=lock_count_r+1'b1;
                            if(lock_count_r>=7) locked_r<=1;
                        end else begin lock_count_r<=0;locked_r<=0; end
                        state_r<=8;
                    end
                    8: begin publish(locked_r ? 7 : 3);state_r<=0; end
                    default: ;
                endcase
            end
        end
    end
endmodule
