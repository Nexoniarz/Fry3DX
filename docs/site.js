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

  // Feature icons: 24px line drawings in the text's colour.
  const ICONS = {
    cube: '<path d="M12 3 20 7.5v9L12 21l-8-4.5v-9z"/><path d="m4 7.5 8 4.5 8-4.5M12 12v9"/>',
    pen: '<path d="M4 20h4L19 9l-4-4L4 16z"/><path d="m13.5 6.5 4 4"/>',
    combine: '<circle cx="9" cy="12" r="5.5"/><circle cx="15" cy="12" r="5.5"/>',
    points: '<rect x="5" y="5" width="14" height="14" rx="1"/><circle cx="5" cy="5" r="1.6" fill="currentColor"/><circle cx="19" cy="5" r="1.6" fill="currentColor"/><circle cx="5" cy="19" r="1.6" fill="currentColor"/><circle cx="19" cy="19" r="1.6" fill="currentColor"/>',
    hole: '<rect x="3.5" y="3.5" width="17" height="17" rx="2"/><path d="m12 7 4.3 2.5v5L12 17l-4.3-2.5v-5z"/><circle cx="12" cy="12" r="1.8"/>',
    copies: '<rect x="3" y="9" width="6" height="6" rx="1"/><rect x="10" y="9" width="6" height="6" rx="1"/><rect x="17" y="9" width="4" height="6" rx="1"/>',
    brush: '<path d="M14.5 4.5 19.5 9.5 11 18l-5-5z"/><path d="M6 13c-2 0-3 1.5-3 3.5S2 20 2 20s3.5.5 5.5-.5S9 17 9 16"/>',
    layout: '<rect x="3.5" y="3.5" width="17" height="17" rx="2"/><path d="M3.5 12h17M12 3.5v17"/>',
    print: '<path d="M7 9V4h10v5"/><rect x="3.5" y="9" width="17" height="8" rx="2"/><path d="M7 14h10v6H7z"/>',
    sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2.5v2.5M12 19v2.5M2.5 12H5M19 12h2.5M5.3 5.3l1.8 1.8M16.9 16.9l1.8 1.8M5.3 18.7l1.8-1.8M16.9 7.1l1.8-1.8"/>',
    folder: '<path d="M3.5 6.5a1 1 0 0 1 1-1h5l2 2h8a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1h-15a1 1 0 0 1-1-1z"/>',
    phone: '<rect x="7" y="2.5" width="10" height="19" rx="2.5"/><path d="M11 18.5h2"/>',
    save: '<path d="M4 12a8 8 0 1 0 2.4-5.7"/><path d="M4 4v4h4M12 8v4l3 2"/>',
    code: '<path d="m8 7-5 5 5 5M16 7l5 5-5 5M13.5 4l-3 16"/>',
    sculpt: '<path d="M4 18c3-1 4-6 8-6s5 5 8 6"/><circle cx="12" cy="7" r="3"/>',
    sphere: '<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12c2.5 2 14.5 2 17 0"/><circle cx="9" cy="8.5" r="1.5" fill="currentColor" stroke="none"/>',
  };
  const icon = (name) => {
    const s = el("span", "feature__icon");
    s.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[name] || ICONS.cube}</svg>`;
    return s;
  };

  // ── Hero ──
  $("latest").textContent = `Version ${F.versions[0].v}: ${F.versions[0].items[0]}`;
  $("long").textContent = F.long;
  $("tagline").textContent = F.tagline;
  $("intro").textContent = F.intro;
  for (const id of ["gh-top", "gh-hero"]) $(id).href = F.repo;
  for (const d of F.downloads) $("platforms").append(el("span", "nv-badge", d.os));
  $("author").textContent = F.author;
  $("author").href = F.authorUrl;

  $("why-title").textContent = F.why.title;
  for (const p of F.why.text) $("why-text").append(el("p", "nv-body", p));

  // ── Features ──
  for (const f of F.features) {
    const card = el("article", "nv-card feature");
    const body = el("div", "nv-card__body");
    body.append(icon(f.icon), el("h3", "nv-card__title", f.title), el("p", "nv-body nv-text-secondary", f.text));
    card.append(body);
    $("feature-grid").append(card);
  }

  // ── Screenshots: a tab each, the picture in a window ──
  F.shots.forEach((s, i) => {
    const tab = el("button", "nv-nav__item", s.title);
    Object.assign(tab, { id: `shot-tab-${i}`, role: "tab", tabIndex: i ? -1 : 0 });
    tab.setAttribute("role", "tab");
    tab.setAttribute("aria-controls", `shot-${i}`);
    tab.setAttribute("aria-selected", String(i === 0));
    $("shot-tabs").append(tab);

    const panel = el("div", "nv-tab-panel shot");
    Object.assign(panel, { id: `shot-${i}`, hidden: i !== 0 });
    panel.setAttribute("role", "tabpanel");
    panel.setAttribute("aria-labelledby", tab.id);
    const win = el("div", "nv-window");
    const bar = el("div", "nv-window__titlebar");
    const lights = el("div", "nv-window__controls");
    for (const k of ["close", "minimize", "zoom"]) {
      const l = el("span", `nv-window__control nv-window__control--${k}`);
      l.setAttribute("aria-hidden", "true");
      lights.append(l);
    }
    bar.append(lights, el("span", "nv-window__title", `Fry3DX — ${s.title}`));
    const img = el("img", "shot__img");
    Object.assign(img, { src: s.img, alt: s.title, loading: i ? "lazy" : "eager" });
    win.append(bar, img);
    panel.append(win, el("p", "nv-body nv-text-secondary", s.text));
    $("shot-panels").append(panel);
  });

  // ── Scripting ──
  if (F.scripting) {
    const S = F.scripting;
    $("scripting").hidden = false;
    $("script-title").textContent = S.title;
    $("script-text").textContent = S.text;
    for (const p of S.points) $("script-points").append(el("li", "nv-body", p));
    $("script-file").textContent = S.file;
    $("script-code").innerHTML = highlightLua(S.code);
  }

  // ── Files and keys ──
  const row = (label, items) => {
    const d = el("div", "fmt");
    d.append(el("span", "nv-overline", label));
    const c = el("div", "nv-cluster nv-cluster--tight");
    for (const x of items) c.append(el("span", "nv-badge", x));
    d.append(c);
    $("formats").append(d);
  };
  row("Save", F.formats.save);
  row("Export", F.formats.export);
  $("formats").append(el("p", "nv-caption nv-text-tertiary", F.formats.note));
  for (const [k, what] of F.keys) {
    const tr = el("tr");
    const td = el("td");
    td.append(el("kbd", null, k));
    tr.append(td, el("td", "nv-body", what));
    $("keys").append(tr);
  }

  // ── Downloads and building ──
  for (const d of F.downloads) {
    const a = el("a", "nv-card nv-card--interactive download");
    a.href = F.releases;
    const b = el("div", "nv-card__body");
    b.append(el("h3", "nv-card__title", d.os), el("p", "nv-caption nv-text-tertiary", d.what),
             el("code", "nv-code download__file", "Fry3DX-…-" + d.file), el("p", "nv-body nv-text-secondary", d.how));
    a.append(b);
    $("downloads").append(a);
  }
  $("build-text").textContent = F.build.text;
  $("build-steps").textContent = F.build.steps.join("\n");
  $("build-needs").textContent = F.build.needs;
  for (const btn of document.querySelectorAll("[data-copy]")) {
    btn.onclick = async () => {
      try {
        await navigator.clipboard.writeText($(btn.dataset.copy).textContent);
        btn.textContent = "Copied";
      } catch {
        btn.textContent = "Select and copy";
      }
      setTimeout(() => (btn.textContent = "Copy"), 1800);
    };
  }

  // ── Versions ──
  F.versions.forEach((v, i) => {
    const d = el("details", "nv-disclosure");
    d.open = i === 0;
    const s = el("summary", "nv-disclosure__summary");
    s.append(el("span", "nv-title", "v" + v.v), el("span", "nv-caption nv-text-tertiary", " " + v.date));
    const ch = el("span", "nv-disclosure__chevron");
    ch.setAttribute("aria-hidden", "true");
    s.append(ch);
    const body = el("div", "nv-disclosure__body");
    const ul = el("ul", "ticks");
    for (const it of v.items) ul.append(el("li", "nv-body", it));
    body.append(ul);
    d.append(s, body);
    $("version-list").append(d);
  });

  // ── The appearance, remembered; the bar's shadow once scrolled ──
  document.documentElement.addEventListener("nv:theme", (e) => {
    try { localStorage.setItem("fry-theme", e.detail?.theme ?? "system"); } catch (_) {}
  });
  const saved = (() => { try { return localStorage.getItem("fry-theme"); } catch (_) { return null; } })();
  if (saved) addEventListener("DOMContentLoaded", () => window.nv?.syncThemeControls(saved));
  const nav = $("top-nav");
  const onScroll = () => nav.toggleAttribute("data-scrolled", scrollY > 4);
  addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  // A little Lua colouring for the scripting sample.
  function highlightLua(src) {
    const esc = (t) => t.replace(/&/g, "&amp;").replace(/</g, "&lt;");
    const words = /\b(local|function|end|for|in|do|if|then|else|elseif|return|while|and|or|not|nil|true|false)\b/g;
    return src.split("\n").map((line) => {
      const at = line.indexOf("--");
      const code = at >= 0 ? line.slice(0, at) : line, comment = at >= 0 ? line.slice(at) : "";
      const parts = code.split(/("[^"]*")/);
      const out = parts.map((p) => (p.startsWith('"') ? `<span class="s">${esc(p)}</span>` : esc(p).replace(words, '<span class="k">$1</span>'))).join("");
      return out + (comment ? `<span class="c">${esc(comment)}</span>` : "");
    }).join("\n");
  }
})();
