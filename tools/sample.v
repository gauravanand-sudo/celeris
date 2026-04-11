module counter(input clk, input rst, output reg [3:0] count);
always @(posedge clk or posedge rst) begin
  if (rst) count <= 0;
  else count <= count + 1;
end
assign overflow = (count == 4'hF);
endmodule