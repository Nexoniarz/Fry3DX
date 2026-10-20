-- A box to print, with a lid and holes for M3 screws in its corners.
local w, d, h, wall = 0.08, 0.06, 0.04, 0.002
local box = fry.add("rounded_cube", {width = w, depth = d, height = h, name = "Box", color = "#d9d4c7"})
local inside = fry.add("cube", {width = w - 2 * wall, depth = d - 2 * wall, height = h})
inside.position = {0, 0, wall}
box:subtract(inside)
local lid = fry.add("cube", {width = w, depth = d, height = wall, name = "Lid", color = "#ff7828"})
lid.position = {w + 0.02, 0, 0}
for sx = -1, 1, 2 do
  for sy = -1, 1, 2 do
    local hole = fry.add("cylinder", {radius = 0.0016, height = 0.01, sides = 24})
    hole.position = {w + 0.02 + sx * (w / 2 - 0.006), sy * (d / 2 - 0.006), -0.002}
    lid:subtract(hole)
  end
end
fry.frame()
