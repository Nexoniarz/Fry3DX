-- A gear for 3D printing, with a hole for an M5 screw.
local teeth, radius, depth, thick = 16, 0.03, 0.004, 0.006   -- metres
local outline = {}
for i = 0, teeth * 4 - 1 do
  local a = i / (teeth * 4) * 2 * math.pi
  local r = (i % 4 < 2) and radius + depth or radius
  outline[#outline + 1] = {math.cos(a) * r, math.sin(a) * r}
end
local gear = fry.extrude(outline, thick, "Gear")
gear.color = "#e0a030"
local hole = fry.add("cylinder", {radius = 0.0027, height = 0.02, sides = 32})
hole.position = {0, 0, -0.005}
gear:subtract(hole)
fry.frame()
