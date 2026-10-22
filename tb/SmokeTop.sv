`timescale 1ns/1ns

// Phase 1 smoke DUT. After reset, the 200th rising edge sets exit to all ones.
module SimTop (
    input clock,
    input reset,
    output reg [63:0] exit
);
  localparam [31:0] ExitCycle = 32'd200;
  reg [31:0] cycles;

  initial begin
    cycles = 32'd0;
    exit = 64'd0;
  end

  always @(posedge clock) begin
    if (reset) begin
      cycles <= 32'd0;
      exit <= 64'd0;
    end
    else begin
      if (cycles + 32'd1 == ExitCycle) begin
        exit <= 64'hffffffffffffffff;
      end
      cycles <= cycles + 32'd1;
    end
  end
endmodule
