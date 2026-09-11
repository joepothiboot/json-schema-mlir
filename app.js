/* ============================================================
   json-schema-mlir — tour renderer
   Zero dependencies. Renders entirely from window.TOUR.
   ============================================================ */
(function () {
  "use strict";

  var T = window.TOUR;
  if (!T) { console.error("TOUR data missing"); return; }

  /* ---------- utilities ---------- */

  function esc(s) {
    return String(s).replace(/[&<>"]/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
    });
  }

  function el(html) {
    var t = document.createElement("template");
    t.innerHTML = html.trim();
    return t.content.firstElementChild;
  }

  function inRanges(n, ranges) {
    if (!ranges) return false;
    for (var i = 0; i < ranges.length; i++) {
      if (n >= ranges[i][0] && n <= ranges[i][1]) return true;
    }
    return false;
  }

  /* ---------- syntax highlighting ----------
     Rules are ordered; the first match wins. Tokens are split on
     newlines during emission so every rendered line is independently
     tag-balanced and safe to wrap in a per-line element.        */

  var RULES = {
    mlir: [
      ["com", "\\/\\/[^\\n]*"],
      ["str", '"(?:[^"\\\\\\n]|\\\\.)*"'],
      ["ssa", "%[A-Za-z0-9_$.\\-]+"],
      ["sym", "@[A-Za-z0-9_$.\\-]+"],
      ["typ", "![A-Za-z0-9_.]+(?:<[^>\\n]*>)?"],
      ["op", "\\b[a-z][A-Za-z0-9_]*(?:\\.[a-z][A-Za-z0-9_]*)+"],
      ["kw", "\\b(?:module|attributes|private|public|loc|dense|unit|as|fields|required|validators|to|step|iter_args)\\b"],
      ["typ", "\\b(?:i1|i8|i16|i32|i64|index|f16|f32|f64|none|tensor|memref|vector)\\b"],
      ["num", "-?\\b\\d+(?:\\.\\d+)?(?:[eE][+\\-]?\\d+)?\\b"],
      ["pun", "[{}()\\[\\],:=<>\\-]"]
    ],
    tablegen: [
      ["com", "\\/\\/[^\\n]*"],
      ["str", "\\[\\{[\\s\\S]*?\\}\\]"],
      ["str", '"(?:[^"\\\\\\n]|\\\\.)*"'],
      ["kw", "\\b(?:def|defvar|class|let|include|in|multiclass|defm|field|code|dag|bit|bits|int|string|list|foreach|if|then|else)\\b"],
      ["var", "\\$[A-Za-z_][A-Za-z0-9_]*"],
      ["typ", "\\b[A-Z][A-Za-z0-9_]*\\b"],
      ["num", "\\b\\d+\\b"],
      ["pun", "[{}()\\[\\],:=<>;]"]
    ],
    cpp: [
      ["com", "\\/\\/[^\\n]*|\\/\\*[\\s\\S]*?\\*\\/"],
      ["pre", "#\\s*(?:include|define|ifndef|ifdef|endif|if|else|elif|pragma|undef)[^\\n]*"],
      ["str", '"(?:[^"\\\\\\n]|\\\\.)*"|\'(?:[^\'\\\\\\n]|\\\\.)*\''],
      ["kw", "\\b(?:auto|bool|break|case|catch|class|const|constexpr|continue|default|delete|do|double|else|enum|explicit|export|extern|false|final|float|for|friend|if|inline|int|long|mutable|namespace|new|noexcept|nullptr|operator|override|private|protected|public|return|short|signed|sizeof|static|struct|switch|template|this|throw|true|try|typedef|typename|union|unsigned|using|virtual|void|while)\\b"],
      ["typ", "\\b(?:std|mlir|llvm|Value|Type|Attribute|Location|Operation|LogicalResult|StringRef|SmallVector|ArrayRef|OpBuilder|PatternRewriter|ConversionPatternRewriter|TypeConverter|RewritePatternSet|ConversionTarget|StringAttr|IntegerAttr|FloatAttr|ArrayAttr|UnitAttr)\\b"],
      ["typ", "\\b[A-Z][A-Za-z0-9_]*\\b"],
      ["num", "-?\\b\\d+(?:\\.\\d+)?[fFuUlL]*\\b"],
      ["fn", "\\b[A-Za-z_][A-Za-z0-9_]*(?=\\s*\\()"],
      ["pun", "[{}()\\[\\],:;=<>&*!?+\\-\\/|]"]
    ],
    json: [
      ["com", "\\/\\/[^\\n]*"],
      ["key", '"(?:[^"\\\\\\n]|\\\\.)*"(?=\\s*:)'],
      ["str", '"(?:[^"\\\\\\n]|\\\\.)*"'],
      ["lit", "\\b(?:true|false|null)\\b"],
      ["num", "-?\\b\\d+(?:\\.\\d+)?(?:[eE][+\\-]?\\d+)?\\b"],
      ["pun", "[{}\\[\\],:]"]
    ],
    bash: [
      ["com", "#[^\\n]*"],
      ["str", '"(?:[^"\\\\\\n]|\\\\.)*"|\'[^\'\\n]*\''],
      ["kw", "\\b(?:cmake|ninja|export|cd|git|llvm-lit|mlir-translate|schema-opt)\\b"],
      ["var", "\\$\\{?[A-Za-z_][A-Za-z0-9_]*\\}?"],
      ["num", "--?[A-Za-z][A-Za-z0-9_\\-]*"],
      ["pun", "[=|\\\\]"]
    ]
  };

  var CACHE = {};
  function compiled(lang) {
    if (CACHE[lang]) return CACHE[lang];
    var rules = RULES[lang];
    if (!rules) return null;
    var src = rules.map(function (r) { return "(" + r[1] + ")"; }).join("|");
    CACHE[lang] = { re: new RegExp(src, "g"), classes: rules.map(function (r) { return r[0]; }) };
    return CACHE[lang];
  }

  /* Returns an array of HTML strings, one per source line. */
  function highlightLines(text, lang) {
    var lines = [""];
    function push(cls, str) {
      var parts = String(str).split("\n");
      for (var i = 0; i < parts.length; i++) {
        if (i > 0) lines.push("");
        if (!parts[i]) continue;
        var e = esc(parts[i]);
        lines[lines.length - 1] += cls ? '<span class="t-' + cls + '">' + e + "</span>" : e;
      }
    }

    var c = compiled(lang);
    if (!c) { push(null, text); return lines; }

    c.re.lastIndex = 0;
    var last = 0, m;
    while ((m = c.re.exec(text)) !== null) {
      if (m.index > last) push(null, text.slice(last, m.index));
      var cls = null;
      for (var g = 1; g < m.length; g++) {
        if (m[g] !== undefined) { cls = c.classes[g - 1]; break; }
      }
      push(cls, m[0]);
      last = c.re.lastIndex;
      if (m[0] === "") c.re.lastIndex++;   // guard against zero-width loops
    }
    if (last < text.length) push(null, text.slice(last));
    return lines;
  }

  /* ---------- code block ---------- */

  function codeBlock(code) {
    var start = code.startLine || 1;
    var lines = highlightLines(code.text.replace(/\s+$/, ""), code.lang);
    var gutterWidth = String(start + lines.length - 1).length;

    var body = lines.map(function (html, i) {
      var n = start + i;
      var em = inRanges(n, code.emphasize) ? " em" : "";
      var g = String(n).padStart(gutterWidth, " ");
      return '<span class="line' + em + '"><span class="gutter">' + g + "</span>" + (html || " ") + "</span>";
    }).join("");

    var head =
      '<div class="code-head">' +
        '<span class="lang">' + esc(code.lang) + "</span>" +
        '<span class="fname">' + esc(code.filename || "") + "</span>" +
        '<span class="spacer"></span>' +
        (code.permalink ? '<a href="' + esc(code.permalink) + '" target="_blank" rel="noopener">source ↗</a>' : "") +
        '<button type="button" class="copy">copy</button>' +
      "</div>";

    var node = el(
      '<figure class="codeblock">' + head +
      '<div class="code-body"><pre><code>' + body + "</code></pre></div></figure>"
    );

    var btn = node.querySelector(".copy");
    btn.addEventListener("click", function () {
      navigator.clipboard.writeText(code.text).then(function () {
        btn.textContent = "copied";
        setTimeout(function () { btn.textContent = "copy"; }, 1400);
      }, function () { btn.textContent = "failed"; });
    });
    return node;
  }

  /* ---------- sections ---------- */

  function renderSampleStrip() {
    if (!T.meta.sampleDataNotice) return;
    document.getElementById("sample-strip").outerHTML =
      '<div class="sample-strip"><div>Rendering <code>data.js</code> sample content — ' +
      'replace with real snippets, IR, and measurements, then set ' +
      "<code>sampleDataNotice: false</code>.</div></div>";
  }

  function renderHero() {
    var h = T.hero, m = T.meta;
    var stats = (h.stats || []).map(function (s) {
      return '<div class="stat"><span class="v">' + esc(s.value) +
        (s.unit ? '<span class="u">' + esc(s.unit) + "</span>" : "") +
        '</span><span class="l">' + esc(s.label) + "</span></div>";
    }).join("");

    document.getElementById("hero").innerHTML =
      '<div class="hero-inner">' +
        '<p class="eyebrow">' + esc(m.project) + " · guided tour · " + esc(m.readingTime) + "</p>" +
        "<h1>" + esc(m.tagline) + "</h1>" +
        '<p class="claim">' + esc(h.claim) + "</p>" +
        (h.paragraphs || []).map(function (p) { return '<p class="body">' + esc(p) + "</p>"; }).join("") +
        (stats ? '<div class="stats">' + stats + "</div>" : "") +
      "</div>";
  }

  function renderPipeline() {
    var nodes = (T.pipeline || []).map(function (p) {
      return '<div class="pipe-node"><span class="n">' + esc(p.label) +
        '</span><span class="s">' + esc(p.sub) + "</span></div>";
    }).join("");
    document.getElementById("pipeline").innerHTML =
      '<div class="wrap"><div class="pipeline">' + nodes + "</div></div>";
  }

  function renderStops() {
    var host = document.getElementById("stops");
    var rail = document.getElementById("rail-list");

    T.stops.forEach(function (stop, i) {
      var num = String(i + 1).padStart(2, "0");

      var section = el('<section class="stop" id="' + esc(stop.id) + '"></section>');
      section.innerHTML =
        '<p class="stop-kicker">' + esc(stop.kicker) + "</p>" +
        "<h2>" + esc(stop.title) + "</h2>" +
        '<p class="lede">' + esc(stop.lede) + "</p>";

      section.appendChild(codeBlock(stop.code));

      if (stop.notes && stop.notes.length) {
        var cls = stop.notes.length > 1 ? "notes cols-2" : "notes";
        section.appendChild(el(
          '<div class="' + cls + '">' + stop.notes.map(function (n) {
            return '<div class="note"><span class="nl">' + esc(n.label) +
              '</span><span class="nt">' + esc(n.text) + "</span></div>";
          }).join("") + "</div>"
        ));
      }

      if (stop.takeaway) {
        section.appendChild(el('<p class="takeaway"><strong>Takeaway.</strong> ' + esc(stop.takeaway) + "</p>"));
      }

      host.appendChild(section);
      rail.appendChild(el(
        '<li><a href="#' + esc(stop.id) + '" data-target="' + esc(stop.id) + '">' +
        '<span class="num">' + num + "</span>" + esc(stop.title) + "</a></li>"
      ));
    });
  }

  function renderDiff() {
    var d = T.diff;
    if (!d) return;

    var rows = d.lines.map(function (ln) {
      var mark = ln.t === "add" ? "+" : ln.t === "del" ? "−" : ln.t === "hunk" ? "@" : " ";
      var html = ln.t === "hunk" ? esc(ln.s) : highlightLines(ln.s, d.lang)[0] || " ";
      return '<span class="line ' + ln.t + '"><span class="mark">' + mark + "</span>" + html + "</span>";
    }).join("");

    var stats = (d.stats || []).map(function (s) {
      return '<div class="cell"><span class="v">' + esc(s.value) +
        '</span><span class="l">' + esc(s.label) + "</span></div>";
    }).join("");

    document.getElementById("diff").innerHTML =
      '<p class="section-kicker">Before / after</p>' +
      '<h2 class="section-title">' + esc(d.title) + "</h2>" +
      '<p class="section-lede">' + esc(d.lede) + "</p>" +
      '<span class="diff-pass">' + esc(d.pass) + "</span>" +
      (stats ? '<div class="diff-summary">' + stats + "</div>" : "") +
      '<figure class="codeblock diff-body">' +
        '<div class="code-head"><span class="lang">diff</span>' +
        '<span class="fname">' + esc(d.summary) + "</span></div>" +
        '<div class="code-body"><pre><code>' + rows + "</code></pre></div>" +
      "</figure>" +
      '<p class="diff-why"><b>Why this is sound.</b> ' + esc(d.why) + "</p>";
  }

  function renderResults() {
    var r = T.results;
    if (!r) return;

    var method = (r.methodology || []).map(function (m) {
      return "<div><dt>" + esc(m.label) + "</dt><dd>" + esc(m.value) + "</dd></div>";
    }).join("");

    var tbody = r.rows.map(function (row) {
      var delta = row.higherIsBetter
        ? "×" + (row.after / row.before).toFixed(2)
        : "−" + (100 * (1 - row.after / row.before)).toFixed(0) + "%";
      return "<tr><td>" + esc(row.label) + "</td>" +
        '<td class="num">' + esc(row.before) + "</td>" +
        '<td class="num">' + esc(row.after) + "</td>" +
        "<td>" + esc(row.unit) + "</td>" +
        '<td class="delta">' + delta + "</td></tr>";
    }).join("");

    var chart = r.rows.map(function (row) {
      var max = Math.max(row.before, row.after);
      function bar(tag, v, isAfter) {
        var pct = (100 * v / max).toFixed(1) + "%";
        return '<div class="bar' + (isAfter ? " after" : "") + '">' +
          '<span class="tag">' + tag + "</span>" +
          '<span class="track"><span class="fill" style="--pct:' + pct + '"></span></span>' +
          '<span class="val">' + v.toLocaleString() + " " + esc(row.unit) + "</span></div>";
      }
      return '<div class="chart-row"><div class="cl"><span>' + esc(row.label) +
        "</span><span>" + (row.higherIsBetter ? "higher is better" : "lower is better") +
        "</span></div>" + bar("before", row.before, false) + bar("after", row.after, true) + "</div>";
    }).join("");

    document.getElementById("results").innerHTML =
      '<p class="section-kicker">Results</p>' +
      '<h2 class="section-title">' + esc(r.title) + "</h2>" +
      '<p class="section-lede">' + esc(r.lede) + "</p>" +
      '<dl class="method">' + method + "</dl>" +
      '<table class="bench"><thead><tr><th>Benchmark</th><th>Before</th><th>After</th>' +
        "<th>Unit</th><th>Delta</th></tr></thead><tbody>" + tbody + "</tbody></table>" +
      '<div class="chart">' + chart + "</div>" +
      '<div class="caveats"><h3>Caveats</h3><ul>' +
        r.caveats.map(function (c) { return "<li>" + esc(c) + "</li>"; }).join("") +
      "</ul></div>";
  }

  function renderLinks() {
    var items = (T.links || []).map(function (l) {
      return '<a href="' + esc(l.href) + '" target="_blank" rel="noopener">' +
        '<span class="ll">' + esc(l.label) + "</span>" +
        '<span class="ln2">' + esc(l.note || "") + "</span></a>";
    }).join("");

    document.getElementById("links").innerHTML =
      '<p class="section-kicker">Source</p>' +
      '<h2 class="section-title">Read the whole thing</h2>' +
      '<p class="section-lede">Every snippet above is a permalink into the repository at a pinned commit.</p>' +
      '<div class="linklist">' + items + "</div>";

    document.getElementById("footer-note").textContent =
      T.meta.project + " — " + T.meta.tagline + ". Static page, no build step.";
  }

  /* ---------- scroll behaviour ---------- */

  function wireScroll() {
    var bar = document.getElementById("progress-bar");
    var links = Array.prototype.slice.call(document.querySelectorAll("#rail-list a"));
    var map = {};
    links.forEach(function (a) { map[a.dataset.target] = a; });

    function progress() {
      var h = document.documentElement;
      var max = h.scrollHeight - h.clientHeight;
      bar.style.width = (max > 0 ? (h.scrollTop / max) * 100 : 0) + "%";
    }
    document.addEventListener("scroll", progress, { passive: true });
    progress();

    if (!("IntersectionObserver" in window)) return;
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        var a = map[e.target.id];
        if (!a) return;
        if (e.isIntersecting) {
          links.forEach(function (x) { x.classList.remove("active"); });
          a.classList.add("active");
        }
      });
    }, { rootMargin: "-25% 0px -60% 0px", threshold: 0 });

    document.querySelectorAll(".stop").forEach(function (s) { io.observe(s); });
  }

  /* ---------- boot ---------- */

  if (T.meta.accent) {
    document.documentElement.style.setProperty("--accent", T.meta.accent);
  }
  document.title = T.meta.project + " — " + T.meta.tagline;

  renderSampleStrip();
  renderHero();
  renderPipeline();
  renderStops();
  renderDiff();
  renderResults();
  renderLinks();
  wireScroll();
})();