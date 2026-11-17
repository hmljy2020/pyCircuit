module tb;
  logic [7:0] a;
  wire [15:0] calculated;
  wire [7:0] shifted,oversized;
  pyc_root dut(.*);
  task row(input logic [7:0] value,input logic [15:0] expected,
           input logic [7:0] expected_shift);
    a=value;#1;
    if(calculated!==expected || shifted!==expected_shift || oversized!==8'd0)
      $fatal(1,"integer transport arithmetic/shift mismatch");
    if(expected===16'bx) $display("WORK x %0d 0",shifted);
    else $display("WORK %0d %0d 0",calculated,shifted);
  endtask
  initial begin
    row(0,6,0);
    row(7,27,0);
    row(255,771,31);
`ifdef NUMERIC_FOUR_STATE
    row(8'b0000000x,16'bx,0);
    row(8'b0000000z,16'bx,0);
`endif
    $finish;
  end
endmodule
