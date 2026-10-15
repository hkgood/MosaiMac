/*
 * Language toggle for the MosaiMac page.
 *
 * The markup ships in Chinese and every translatable element carries
 * data-en="...". On first run the Chinese text is stashed in data-zh, so both
 * directions are just a textContent swap -- which also means the page is
 * fully readable with JavaScript disabled.
 */
(function () {
  "use strict";

  var KEY = "mosaimac.lang";
  var root = document.documentElement;
  var lang = "zh";

  // Stash the original Chinese before anything overwrites it.
  var all = Array.prototype.slice.call(document.querySelectorAll("[data-en]"));
  all.forEach(function (n) {
    if (!n.hasAttribute("data-zh")) n.setAttribute("data-zh", n.textContent);
  });

  function apply(next) {
    lang = next;
    root.setAttribute("lang", next === "en" ? "en" : "zh-CN");
    all.forEach(function (n) {
      var t = next === "en" ? n.getAttribute("data-en") : n.getAttribute("data-zh");
      if (t !== null) n.textContent = t;
    });
    var btn = document.getElementById("lang");
    if (btn) btn.textContent = next === "en" ? "中" : "EN";
    document.title = next === "en"
      ? "MosaiMac — QuickDraw for ESP32-S31"
      : "MosaiMac — 把 1984 年那套 1-bit 图形系统塞进 AMOLED";
  }

  try {
    var saved = localStorage.getItem(KEY);
    if (saved === "en" || saved === "zh") {
      lang = saved;
    } else if ((navigator.language || "").toLowerCase().indexOf("zh") !== 0) {
      /* The page is written in Chinese; only switch away for a non-Chinese
       * browser, and let the reader flip it back. */
      lang = "en";
    }
  } catch (e) { /* private mode; keep the markup's Chinese */ }

  apply(lang);

  var btn = document.getElementById("lang");
  if (btn) {
    btn.addEventListener("click", function () {
      apply(lang === "en" ? "zh" : "en");
      try { localStorage.setItem(KEY, lang); } catch (e) { /* ignore */ }
    });
  }
})();
