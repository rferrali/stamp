(function () {
  "use strict";

  if (document.body.isContentEditable)
    return;

  if (!document.getElementById("stamp-quote-style")) {
    var css = document.createElement("style");

    css.id = "stamp-quote-style";

    css.textContent =
      ":root {" +
        "--stamp-quote-bg: #e0e0e0;" +
        "--stamp-quote-bg-clear: rgba(224,224,224,0);" +
        "--stamp-quote-btn-bg: #cfcfcf;" +
        "--stamp-quote-btn-bg-hover: #bdbdbd;" +
        "--stamp-quote-fg: #666" +
      "}" +
      "@media(prefers-color-scheme:dark) {" +
        ":root {" +
          "--stamp-quote-bg: #333;" +
          "--stamp-quote-bg-clear: rgba(51,51,51,0);" +
          "--stamp-quote-btn-bg: #484848;" +
          "--stamp-quote-btn-bg-hover: #585858;" +
          "--stamp-quote-fg: #aaa" +
        "}" +
      "}" +
      "blockquote.stamp-quote-processed {" +
        "margin-left: 12px !important;" +
        "padding-left: 25px !important;" +
        "border-left: none !important;" +
        "background-image: linear-gradient(#aaa, #aaa);" +
        "background-repeat: no-repeat;" +
        "background-size: 3px calc(100% - 20px);" +
        "background-position: 12px 10px" +
      "}" +
      "blockquote.stamp-quote-collapsed {" +
        "max-height: 6em;" +
        "overflow: hidden;" +
        "border-radius: 8px 8px 0 0;" +
        "padding-top: 10px;" +
        "position: relative;" +
        "background-color: var(--stamp-quote-bg) !important" +
      "}" +
      "blockquote.stamp-quote-collapsed::after {" +
        "content: \"\";" +
        "position: absolute;" +
        "bottom: 0;" +
        "left: 15px;" +
        "right: 0;" +
        "height: 1.5em;" +
        "background-image: linear-gradient(var(--stamp-quote-bg-clear), var(--stamp-quote-bg));" +
        "pointer-events: none" +
      "}" +
      ".stamp-quote-expand-btn {" +
        "display: block;" +
        "box-sizing: border-box;" +
        "padding: 2px 9px 3px;" +
        "border: none;" +
        "border-radius: 0 0 8px 8px;" +
        "background-color: var(--stamp-quote-btn-bg) !important;" +
        "color: var(--stamp-quote-fg) !important;" +
        "cursor: pointer;" +
        "font-size: 12px;" +
        "line-height: 1.2;" +
        "text-align: center" +
      "}" +
      ".stamp-quote-expand-btn:hover {" +
        "background-color: var(--stamp-quote-btn-bg-hover) !important" +
      "}";
    document.head.appendChild(css);
  }

  var body = document.getElementById("message-body");
  if (body && body.stampProcessedHtml === body.innerHTML)
    return;

  [].forEach.call(document.querySelectorAll(".stamp-quote-expand-btn"), function (b) {
    b.remove();
  });
  [].forEach.call(document.querySelectorAll("blockquote.stamp-quote-processed"), function (q) {
    q.classList.remove("stamp-quote-processed", "stamp-quote-collapsed");
    q.style.removeProperty("margin-bottom");
  });

  if (body && !body.querySelector("blockquote")) {
    var parts = body.innerHTML.split(/(<br\s*\/?>)/i);
    var result = [];
    var buf = [];
    var quote = false;

    function flush() {
      if (buf.length) {
        result.push("<blockquote>" + buf.join("") + "</blockquote>");
        buf = [];
      }
    }

    for (var i = 0; i < parts.length; i++) {
      var p = parts[i];

      if (/^<br\s*\/?>$/i.test(p)) {
        quote ? buf.push(p) : result.push(p);
      } else if (/^\s*(?:&gt;|>)/.test(p.replace(/<[^>]*>/g, ""))) {
        if (!quote) {
          if (result.length && /^<br\s*\/?>$/i.test(result[result.length - 1]))
            buf.push(result.pop());

          quote = true;
        }

        buf.push(p);
      } else {
        flush();
        result.push(p);

        quote = false;
      }
    }

    flush();
    body.innerHTML = result.join("");
  }

  function isEmpty(n) {
    if (n.nodeType === 3)
      return !n.nodeValue.trim();

    return n.nodeType === 1 &&
           !n.querySelector("img,hr,table,pre,svg,video") &&
           !n.textContent.trim();
  }

  function trimLeading(q) {
    var n;

    while ((n = q.firstChild) && isEmpty(n))
      n.remove();

    if (n && n.nodeType === 1 && n.tagName === "DIV")
      trimLeading(n);
  }

  [].forEach.call(document.querySelectorAll("blockquote"), function (q) {
    if (q.classList.contains("stamp-quote-processed"))
      return;

    q.classList.add("stamp-quote-processed");

    trimLeading(q);

    if (q.parentNode.closest("blockquote"))
      return;

    if (q.scrollHeight <= 150)
      return;

    q.classList.add("stamp-quote-collapsed");

    var btn = document.createElement("button");
    btn.innerHTML = "&#9660;";
    btn.className = "stamp-quote-expand-btn";
    btn.addEventListener("click", function () {
      q.classList.remove("stamp-quote-collapsed");
      q.style.removeProperty("margin-bottom");
      btn.remove();
      window.webkit.messageHandlers.stampResize.postMessage("");
    });

    q.parentNode.insertBefore(btn, q.nextSibling);

    var cs = getComputedStyle(q);
    btn.style.width = q.offsetWidth + "px";
    btn.style.marginLeft = cs.marginLeft;
    btn.style.marginRight = cs.marginRight;
    btn.style.marginBottom = cs.marginBottom;
    q.style.setProperty("margin-bottom", "0", "important");
  });

  if (body)
    body.stampProcessedHtml = body.innerHTML;
})();
