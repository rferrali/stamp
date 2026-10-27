const editor = document.getElementById('message-body');

/* Undo / Redo */
document.addEventListener('keydown', (e) => {
  if (e.ctrlKey && e.key === 'z' && !e.shiftKey) {
    e.preventDefault();
    document.execCommand('undo');
  }

  if ((e.ctrlKey && e.key === 'y') || (e.ctrlKey && e.shiftKey && e.key === 'Z')) {
    e.preventDefault();
    document.execCommand('redo');
  }
});

document.body.addEventListener('keydown', (e) => {
  if (e.key !== 'Enter' || e.shiftKey) return;

  const sel = window.getSelection();
  if (!sel || !sel.rangeCount) return;

  let node = sel.anchorNode;
  if (!node) return;
  if (node.nodeType === 3) node = node.parentNode;

  let n = node;
  while (n && n !== document.body) {
    if (n.nodeName === 'BLOCKQUOTE') return;
    if (n.nodeName === 'LI' || n.nodeName === 'UL' || n.nodeName === 'OL') return;
    n = n.parentNode;
  }

  e.preventDefault();
  document.execCommand('insertLineBreak', false, null);
}, true);

document.body.addEventListener('keyup', (e) => {
  if (e.key !== 'Enter' || e.shiftKey) return;

  const sel = window.getSelection();
  if (!sel || !sel.rangeCount) return;

  let node = sel.anchorNode;
  if (!node) return;
  if (node.nodeType === 3) node = node.parentNode;

  /* Count how many blockquote ancestors we are inside */
  let depth = 0;
  let n = node;
  while (n && n !== document.body) {
    if (n.nodeName === 'BLOCKQUOTE') depth++;
    n = n.parentNode;
  }

  /* Outdent once per level to break out of all enclosing blockquotes */
  for (let i = 0; i < depth; i++) {
    document.execCommand('outdent', false, null);
  }
}, true);

document.addEventListener('paste', (e) => {
  const data = e.clipboardData;
  if (!data) return;

  const html = data.getData('text/html');
  const text = data.getData('text/plain');

  e.preventDefault();

  if (!html) {
    text.replace(/\r\n?/g, '\n').split('\n').forEach((line, i) => {
      if (i > 0) document.execCommand('insertLineBreak', false, null);
      if (line) document.execCommand('insertText', false, line);
    });
    return;
  }

  const doc = new DOMParser().parseFromString(html, 'text/html');
  doc.querySelectorAll('script, style, meta, link, title').forEach((el) => el.remove());
  doc.querySelectorAll('[style]').forEach((el) => {
    ['margin', 'margin-top', 'margin-bottom', 'line-height', 'padding-top', 'padding-bottom']
      .forEach((p) => el.style.removeProperty(p));
  });

  const blocks = Array.from(doc.body.querySelectorAll('p, div')).reverse();
  blocks.forEach((el) => {
    if (el.closest('blockquote, li')) {
      el.style.margin = '0';
      return;
    }
    const next = el.nextSibling;
    const frag = doc.createDocumentFragment();
    while (el.firstChild) frag.appendChild(el.firstChild);
    if (next || el.parentNode !== doc.body)
      frag.appendChild(doc.createElement('br'));
    el.replaceWith(frag);
  });

  document.execCommand('insertHTML', false, doc.body.innerHTML);
}, true);
