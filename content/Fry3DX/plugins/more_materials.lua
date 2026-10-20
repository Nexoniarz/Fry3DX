-- More materials for the picker (Properties > Material), and for scripts: o.material = "marble".
-- fry.material works in any script; in a plug-in they're there every time Fry3DX starts.
fry.plugin{name = "More materials", about = "Marble, rose gold, titanium, carbon, jade, walnut and others."}

local list = {
  {"Marble", "#e8e4dc", 0, 0.15}, {"Black marble", "#1d1d20", 0, 0.12}, {"Rose gold", "#f2b6a0", 1, 0.2},
  {"Silver", "#f2f2f0", 1, 0.12}, {"Titanium", "#a8a6a0", 1, 0.35}, {"Carbon", "#202224", 0, 0.25},
  {"Jade", "#4f9e74", 0, 0.2}, {"Terracotta", "#c0603c", 0, 0.9}, {"Walnut", "#5b3a26", 0, 0.6},
  {"Oak", "#c49a64", 0, 0.65}, {"Red car paint", "#b0101a", 0.3, 0.1}, {"Teal plastic", "#1f9a96", 0, 0.35},
}
for _, m in ipairs(list) do
  fry.material{name = m[1], color = m[2], metal = m[3], rough = m[4]}
end
