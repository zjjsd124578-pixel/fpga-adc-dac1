// Forward an inverted DDS clock through an output DDR register.
// DDS data changes at rising edge; nominal DAC capture edge is half a cycle later.
// Confirm DAC module clock circuitry/polarity and external setup/hold before use.
module dac_clock_out (
    input wire dds_clk_i,
    output wire dac_clk_o
);
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC"))
    dac_clock_forwarder (
        .Q(dac_clk_o), .C(dds_clk_i), .CE(1'b1),
        .D1(1'b0), .D2(1'b1), .R(1'b0), .S(1'b0)
    );
endmodule
