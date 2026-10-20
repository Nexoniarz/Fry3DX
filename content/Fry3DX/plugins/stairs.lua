-- Stairs, straight or round a post: two commands in the Add menu, each asking for its sizes.
-- A plug-in is a script that stays loaded: fry.command puts a command in a menu, and its run
-- function is called (with the settings asked for) each time it's picked.
fry.plugin{name = "Stairs", about = "Straight stairs and spiral stairs, from the Add menu."}

fry.command{
  name = "Stairs", menu = "add",
  help = "A flight of steps going up along Y.",
  settings = {{"steps", 12}, {"rise", 0.18}, {"going", 0.28}, {"width", 0.9}},
  run = function(s)
    local made = {}
    for i = 0, math.max(1, math.floor(s.steps)) - 1 do
      local step = fry.add("cube", {width = s.width, depth = s.going, height = s.rise * (i + 1), color = "#b07a4a", name = "Step"})
      step.position = {0, i * s.going, 0}
      step:drop() -- (its bottom on the ground)
      made[#made + 1] = step
    end
    fry.group("Stairs", made)
    fry.select(made)
  end,
}

fry.command{
  name = "Spiral stairs", menu = "add",
  help = "Steps turning round a post.",
  settings = {{"steps", 18}, {"rise", 0.18}, {"turn", 22}, {"width", 0.9}},
  run = function(s)
    local steps = math.max(1, math.floor(s.steps))
    local made = {fry.add("pillar", {radius = 0.12, height = steps * s.rise + 0.3, color = "#8a8a90", name = "Post"})}
    for i = 0, steps - 1 do
      local step = fry.add("cube", {width = s.width, depth = 0.32, height = 0.05, color = "#b07a4a", name = "Step"})
      local a = math.rad(i * s.turn)
      step.turn = {0, 0, i * s.turn}
      step.position = {math.cos(a) * s.width / 2, math.sin(a) * s.width / 2, (i + 1) * s.rise}
      made[#made + 1] = step
    end
    fry.group("Spiral stairs", made)
    fry.select(made)
  end,
}
