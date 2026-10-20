-- A little town: random houses on a grid, each a folder of its own.
math.randomseed(7)
local roofs = {"#a8432f", "#5a6470", "#8c5a3c"}
for x = -2, 2 do
  for y = -2, 2 do
    local w, d, h = 0.6 + math.random() * 0.3, 0.6 + math.random() * 0.3, 0.5 + math.random() * 0.9
    local walls = fry.add("cube", {width = w, depth = d, height = h, color = "#e8e0d0", name = "Walls"})
    walls.position = {x * 1.4, y * 1.4, 0}
    -- a wedge rises towards -X: two of them back to back make a ridge
    local roof = fry.add("wedge", {width = d + 0.1, length = w / 2 + 0.05, height = 0.35, name = "Roof"})
    local other = roof:copy()
    roof.position = {x * 1.4 + w / 4 + 0.025, y * 1.4, h}
    other.position = {x * 1.4 - w / 4 - 0.025, y * 1.4, h}
    other.turn = {0, 0, 180}
    local c = roofs[math.random(#roofs)]
    roof.color, other.color = c, c
    fry.group(string.format("House %d,%d", x, y), {walls, roof, other})
  end
end
fry.look{sun_turn = 60, sun_height = 35}
fry.frame()
