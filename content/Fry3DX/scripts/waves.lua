-- Points moved by a formula: a plane made into waves, then given a thickness.
local p = fry.add("plane", {width = 2, depth = 2, cuts = 40, name = "Waves"})
local pts = p:points()
for i, v in ipairs(pts) do
  v.z = 0.08 * math.sin(v.x * 6) * math.cos(v.y * 5)
end
p:setpoints(pts)
p:thicken(0.03)
p.smooth = true
p.color = "#2f8f9d"
p:move(0, 0, 0.12)
fry.frame()
