-- A vase turned on a lathe: the profile is {out, up} points from the bottom.
local profile = {}
local height, n = 0.4, 24
for i = 0, n do
  local t = i / n
  -- a belly low down, a neck, a lip
  local r = 0.06 + 0.07 * math.sin(t * math.pi * 1.1) + 0.02 * t * t
  profile[#profile + 1] = {r, t * height}
end
table.insert(profile, 1, {0, 0})
local vase = fry.lathe(profile, 64, "Vase")
vase.color = "#3c6e9f"
vase.rough = 0.25
vase:thicken(0.004)   -- walls, so it prints hollow
fry.frame()
