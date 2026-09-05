module axis_status_latch (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME aclk, ASSOCIATED_BUSIF s_axis_status, ASSOCIATED_RESET aresetn" *)
    input  wire       aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME aresetn, POLARITY ACTIVE_LOW" *)
    input  wire       aresetn,
    // TEMPORARY DIAGNOSTIC: widened 8->32 bits (see barrier_arbiter.h's
    // scheduler_status_t comment) to carry programLoader's seed-loop
    // progress bits. Revert to [7:0] once the busy-forever investigation
    // concludes.
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_status TDATA" *)
    input  wire [31:0] s_axis_status_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_status TVALID" *)
    input  wire       s_axis_status_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_status TREADY" *)
    output wire       s_axis_status_tready,
    output reg  [31:0] status
);

assign s_axis_status_tready = 1'b1;

always @(posedge aclk) begin
    if (!aresetn)
        status <= 32'b0;
    else if (s_axis_status_tvalid)
        status <= s_axis_status_tdata;
end

endmodule