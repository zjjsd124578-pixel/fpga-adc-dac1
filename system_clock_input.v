// A single external 50 MHz clock buffer shared by the ADC and DAC MMCMs.
module system_clock_input (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk_pad_i CLK", X_INTERFACE_PARAMETER = "FREQ_HZ 50000000" *)
    input wire clk_pad_i,
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk_50m_o CLK", X_INTERFACE_PARAMETER = "FREQ_HZ 50000000" *)
    output wire clk_50m_o
);
    wire clk_ibuf;
    IBUF clock_input_buffer (.I(clk_pad_i), .O(clk_ibuf));
    BUFG shared_clock_buffer (.I(clk_ibuf), .O(clk_50m_o));
endmodule