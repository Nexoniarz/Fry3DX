// Builds the page from window.FRY (content.js). Nothing here needs changing for a content update.
(function () {
  const F = window.FRY;
  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };

  $("long").textContent = F.long;
  $("tagline").textContent = F.tagline;
  $("intro").textContent = F.intro;
  for (const id of ["gh-top", "gh-hero"]) $(id).href = F.repo;
  $("author").textContent = F.author;
  $("author").href = F.authorUrl;

  const why = $("why");
  why.append(el("h2", null, F.why.title));
  for (const p of F.why.text) why.append(el("p", null, p));

  const grid = $("feature-grid");
  for (const f of F.features) {
    const card = el("article", "card");
    card.append(el("div", "icon", f.icon), el("h3", null, f.title), el("p", null, f.text));
    grid.append(card);
  }

  // Screenshots: a tab each.
  const tabs = $("shot-tabs");
  const show = (i) => {
    const s = F.shots[i];
    $("shot-img").src = s.img;
    $("shot-img").alt = s.title;
    $("shot-text").textContent = s.text;
    [...tabs.children].forEach((b, j) => b.setAttribute("aria-selected", i === j));
  };
  F.shots.forEach((s, i) => {
    const b = el("button", "tab", s.title);
    b.setAttribute("role", "tab");
    b.onclick = () => show(i);
    tabs.append(b);
    new Image().src = s.img; // (loaded ahead, so switching is instant)
  });
  show(0);

  const fm = $("formats");
  const row = (label, items) => {
    const d = el("div", "fmt");
    d.append(el("span", "fmt-label", label));
    for (const x of items) d.append(el("span", "chip", x));
    fm.append(d);
  };
  row("Save", F.formats.save);
  row("Export", F.formats.export);
  fm.append(el("p", "muted", F.formats.note));

  const keys = $("keys");
  for (const [k, what] of F.keys) {
    const tr = el("tr");
    const td = el("td");
    td.append(el("kbd", null, k));
    tr.append(td, el("td", null, what));
    keys.append(tr);
  }

  const dl = $("downloads");
  for (const d of F.downloads) {
    const a = el("a", "download");
    a.href = F.releases;
    a.append(el("strong", null, d.os), el("span", "muted", d.what), el("span", "file", "Fry3DX-…-" + d.file), el("span", null, d.how));
    dl.append(a);
  }

  $("build-text").textContent = F.build.text;
  $("build-steps").textContent = F.build.steps.join("\n");
  $("build-needs").textContent = F.build.needs;
  $("copy").onclick = async () => {
    try {
      await navigator.clipboard.writeText(F.build.steps.join("\n"));
      $("copy").textContent = "Copied";
    } catch {
      $("copy").textContent = "Select and copy";
    }
    setTimeout(() => ($("copy").textContent = "Copy"), 1800);
  };

  const vl = $("version-list");
  for (const v of F.versions) {
    const d = el("div", "version");
    const h = el("h3");
    h.append(el("span", "v", "v" + v.v), el("span", "muted", " " + v.date));
    const ul = el("ul");
    for (const it of v.items) ul.append(el("li", null, it));
    d.append(h, ul);
    vl.append(d);
  }
})();
