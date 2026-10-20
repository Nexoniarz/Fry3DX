-- Scatter: copies of what's picked, strewn round it on the ground, each turned and sized a bit
-- differently (rocks, trees, crates). In the Object menu.
fry.plugin{name = "Scatter", about = "Copies of the picked object strewn round it, each a little different."}

fry.command{
  name = "Scatter copies", menu = "object",
  help = "Copies of the last picked object round it on the ground.",
  settings = {{"copies", 12}, {"radius", 2}, {"size_change", 0.3}, {"seed", 1}},
  run = function(s)
    local picked = fry.selected()
    local o = picked[#picked]
    if not o then error("pick an object to scatter first") end
    math.randomseed(math.floor(s.seed))
    local base = o.position
    local made = {}
    for i = 1, math.max(1, math.floor(s.copies)) do
      local c = o:copy()
      local a, r = math.random() * 2 * math.pi, math.sqrt(math.random()) * s.radius
      c.position = {base.x + math.cos(a) * r, base.y + math.sin(a) * r, base.z}
      c:rotate(0, 0, math.random() * 360)
      c:scale(1 + (math.random() * 2 - 1) * s.size_change)
      made[#made + 1] = c
    end
    fry.group(o.name .. " scattered", made)
    fry.select(made)
  end,
}
