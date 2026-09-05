module axis_status_latch (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME aclk, ASSOCIATED_BUSIF s_axis_status, ASSOCIATED_RESET aresetn" *)
    input  wire       aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME aresetn, POLARITY ACTIVE_LOW" *)
    input  wire       aresetn,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_status TDATA" *)
    input  wire [7:0] s_axis_status_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_status TVALID" *)
    input  wire       s_axis_status_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_status TREADY" *)
    output wire       s_axis_status_tready,
    output reg  [7:0] status
);

assign s_axis_status_tready = 1'b1;

always @(posedge aclk) begin
    if (!aresetn)
        status <= 8'b00000000;
    else if (s_axis_status_tvalid)
        status <= s_axis_status_tdata;
end

endmodule