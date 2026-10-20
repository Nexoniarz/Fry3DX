-- Spiral stairs round a pillar: change the numbers and run it again (F5).
local steps, rise, turn = 18, 0.18, 22   -- how many, how high each, degrees each
local width = 0.9

fry.add("pillar", {radius = 0.12, height = steps * rise + 0.3, color = "#8a8a90", name = "Post"})
local made = {}
for i = 0, steps - 1 do
  local step = fry.add("cube", {width = width, depth = 0.32, height = 0.05, color = "#b07a4a", name = "Step"})
  step.turn = {0, 0, i * turn}
  local a = math.rad(i * turn)
  step.position = {math.cos(a) * width / 2, math.sin(a) * width / 2, (i + 1) * rise}
  made[#made + 1] = step
end
fry.group("Stairs", made)
fry.frame()
