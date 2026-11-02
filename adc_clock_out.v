// Forward the 65 MHz sampling clock to both ADC clock inputs through I/O flops.
// The ADC data interface samples using the same clock net inside the FPGA.
module adc_clock_out (
    input  wire adc_clk_i,
    output wire adc_ch1_clk_o,
    output wire adc_ch2_clk_o
);

    ODDR #(
        .DDR_CLK_EDGE("SAME_EDGE"),
        .INIT(1'b0),
        .SRTYPE("SYNC")
    ) ch1_clock_forwarder (
        .Q (adc_ch1_clk_o),
        .C (adc_clk_i),
        .CE(1'b1),
        .D1(1'b1),
        .D2(1'b0),
        .R (1'b0),
        .S (1'b0)
    );

    ODDR #(
        .DDR_CLK_EDGE("SAME_EDGE"),
        .INIT(1'b0),
        .SRTYPE("SYNC")
    ) ch2_clock_forwarder (
        .Q (adc_ch2_clk_o),
        .C (adc_clk_i),
        .CE(1'b1),
        .D1(1'b1),
        .D2(1'b0),
        .R (1'b0),
        .S (1'b0)
    );

endmodule
