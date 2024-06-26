module dff (input d, clk, rstb,
            output reg q);
  always @(posedge clk)
    q <= d;
endmodule // dff

module mod2;
  wire [7:0] bus;
  wire       w;
  reg        r;
  initial begin
    $display("hello", $time);
    if (bus)
      r <= 1 | r;
    $finish;
    r = 1;
    #1 r <= 0;
    r = ~w;
    r = #1 5;
    r <= #5 1;
  end
  assign bus = 3;
  pullup (supply1, supply0) p1 (w);
  mod u1 (w);
  assign bus[2] = 4'b1;
endmodule // mod2
