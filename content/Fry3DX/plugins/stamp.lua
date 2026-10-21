-- Stamp: a mouse tool. Click on anything (or the ground) and a copy of the picked object is put
-- there, standing on the surface. Shows how a plug-in adds a tool to the Toolbox (fry.tool).
fry.plugin{name = "Stamp", about = "A tool: click to put copies of the picked object on what you click, standing on it."}

fry.tool{
  name = "Stamp copies",
  help = "Click to put a copy of the picked object there, standing on the surface (pick one first).",
  click = function(at, normal, onto)
    local picked = fry.selected()
    local o = picked[#picked]
    if not o or (onto and o.id == onto.id) then
      -- nothing picked to copy (or clicking on it): a little ball, to show where
      o = fry.add("sphere", {radius = 0.1})
      o.position = at + normal * 0.1
      return
    end
    local c = o:copy()
    local lo, hi = o:bounds()
    c.position = at + normal * ((hi.z - lo.z) / 2)
    o:select()
  end,
}
