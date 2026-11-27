// Quest Search and Notification Overlay: the box layer (0.16.0). One stack for both kinds of box:
// - a quest box (QSO 0.11.0): kind, heading, quest, objective line, place;
// - a game line (merged from HUD Notification Replacer 0.2.0): the game's white top-left text, Flash HTML.
// The plugin holds boxes while a menu, a conversation, a fade, a loading screen or the J overlay is up, and sends them
// in order afterwards; this page shows them. Kept in its own file so a notifications-only build can ship it alone.
//
// From the plugin: window.qsoToast(json), window.qsoToastPause('1' | '0').
// From overlay.js (if present): window.qsoBoxesPanel(true | false) while the J overlay is open / closed.
// To the plugin:   window.qsoCommand('toasts:idle:<newest id>') once nothing is on screen or waiting; 'log:<text>' on errors.
// qsoToast json, quest box: {"id":3,"pos":1,"secs":8,"scale":100,"max":5,"kind":"arrive|leave|near|updated",
//   "head":"Here in Whiterun","quest":"Hunt and Gather","line":"Bring venison to Anoriath","where":"...","more":0,"key":"J"}
// qsoToast json, game line: {"id":4,"pos":1,"secs":6,"scale":100,"max":5,"box":"game","fold":false,"text":"<raw line>"}
// 0.17.0: every box also has "min": the player's "Min time on screen" in seconds (1-30, iMinBoxSeconds).
// 0.17.0: ids are consecutive (the plugin numbers every box it sends). Boxes are shown in id order even if the calls
//   arrive out of order (TESTING-0.16.1 A3: a 51-call burst showed "1, 6, 8, 10, +30 more ..." first, the plugin's log
//   had them in order); a missing id is waited for GAP_MS, then skipped with a log line.
// 0.17.0: the stack reads oldest to newest from top to bottom in every position, like the game's own lines: at the top
//   and middle spots new boxes join at the bottom of the stack, at the bottom spots the newest sits at the spot.
//
// Never drops a box: at the box limit new ones WAIT and the oldest on screen is hurried out, so a backlog (say 30 lines
// after a load door) plays through faster than one box per 6 s, but still readably (0.16.1):
// - a hurried box stays at least MIN_MS on screen;
// - while boxes wait, at most one leaves every PACE_MS, oldest first, and the next one comes in at once in its place.
// 0.16.0 hurried with 1.5 s and no pace: once the oldest had been up 1.5 s, every box behind it had been up as long, so
// the whole stack left in the same millisecond and the next 10 replaced it (a full-screen swap every 1.5 s; user test
// B13 "too fast to see", and B14's "+N more" box arrived after only five swaps).
(function () {
  'use strict';

  var MIN_MS = 3000;     // least time a hurried box stays visible (default; the plugin sends the player's "min")
  var LONG_WORDS = 8;    // 0.17.0: a box with more words than this gets EXTRA_MS_PER_WORD for each extra word,
  var EXTRA_MS_PER_WORD = 300;  // both on its own clock and as its minimum (about 200 words a minute)
  var MIN_CAP_MS = 30000;       // the minimum never goes above 30 s (user: "No longer than 30 seconds")
  var GAP_MS = 250;      // 0.17.0: how long to wait for a box id that has not arrived yet
  var PACE_MS = 1000;    // while others wait, at most one box leaves per this many ms
  var WAIT_CAP = 50;     // more waiting than this: the oldest waiting ones fold into one "+N more" box

  var layer = document.getElementById('toasts');
  var boxes = [];        // on screen, oldest first: {el, total, remaining, started, timer, hurried}
  var waiting = [];      // arrived while full or paused, oldest first
  var folded = 0;        // waiting boxes folded away by WAIT_CAP (the plugin's log has every line)
  var pluginPaused = false;  // from the plugin: a menu / fade / loading screen / the overlay
  var panelOpen = false;     // from overlay.js: the J overlay is on screen
  var lastId = 0;
  var idleSentFor = -1;
  var leaving = 0;
  var max = 5;
  var lastLayout = { pos: 1, scale: 100 };
  var lastLeave = 0;     // Date.now() when the last box started to leave
  var pausedTotal = 0;   // ms the layer has been paused, in total (for visibleMs)
  var pauseStart = 0;    // Date.now() when the current pause began, 0 = not paused
  var nextId = 1;        // 0.17.0: the id expected next from the plugin
  var early = {};        // id -> box that arrived before an earlier id
  var gapTimer = 0;

  function paused() { return pluginPaused || panelOpen; }

  function send(command) {
    if (typeof window.qsoCommand === 'function') window.qsoCommand(command);
  }

  window.addEventListener('error', function (e) {
    send('log:JS error ' + (e && e.message ? e.message : '?') + ' at ' + (e && e.filename ? e.filename : '?') + ':' +
      (e && e.lineno ? e.lineno : '?'));
  });

  function textEl(tagName, cls, text) {
    var el = document.createElement(tagName);
    if (cls) el.className = cls;
    el.textContent = text;
    return el;
  }

  // ---- a game line's text: Flash HTML ----
  // Kept: <font color="#rrggbb">, <b>, <i>, <u>, <br>. Every other tag is dropped (as Flash drops tags it does not know)
  // and its text kept. Built with DOM nodes and textContent only, never innerHTML, so no line can inject anything.
  var ENTITIES = { lt: '<', gt: '>', amp: '&', quot: '"', apos: "'", nbsp: ' ' };

  function decode(s) {
    return s.replace(/&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);/g, function (all, e) {
      if (e.charAt(0) === '#') {
        var n = e.charAt(1) === 'x' || e.charAt(1) === 'X' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
        return n > 0 && n < 0x110000 ? String.fromCodePoint(n) : all;
      }
      var v = ENTITIES[e.toLowerCase()];
      return v === undefined ? all : v;
    });
  }

  function colourOf(attrs) {
    var m = /color\s*=\s*["']?\s*(#[0-9a-fA-F]{6}|#[0-9a-fA-F]{3})(?![0-9a-fA-F])/i.exec(attrs || '');
    return m ? m[1] : '';
  }

  function renderLine(target, raw) {
    var stack = [{ name: '', el: target }];
    var re = /<(\/?)([a-zA-Z][a-zA-Z0-9]*)([^<>]*)>/g;
    var at = 0;
    var m;
    function top() { return stack[stack.length - 1].el; }
    function text(s) { if (s) top().appendChild(document.createTextNode(decode(s))); }
    while ((m = re.exec(raw)) !== null) {
      text(raw.slice(at, m.index));
      at = re.lastIndex;
      var closing = m[1] === '/';
      var name = m[2].toLowerCase();
      if (name === 'br' && !closing) { top().appendChild(document.createElement('br')); continue; }
      if (name !== 'font' && name !== 'b' && name !== 'i' && name !== 'u') continue;  // dropped, text kept
      if (closing) {
        for (var k = stack.length - 1; k > 0; k--) {
          if (stack[k].name === name) { stack.length = k; break; }
        }
        continue;
      }
      var span = document.createElement('span');
      if (name === 'font') {
        var c = colourOf(m[3]);
        if (c) span.style.color = c;
      } else {
        span.className = 't-' + name;
      }
      top().appendChild(span);
      stack.push({ name: name, el: span });
    }
    text(raw.slice(at));
  }

  // ---- the two kinds of box ----
  function questBoxEl(d) {
    var el = textEl('div', 'box pre kind-' + String(d.kind || 'near'), '');
    var head = textEl('div', 'b-head', '');
    head.appendChild(textEl('span', 'b-kind', String(d.head || '')));
    if (d.key) head.appendChild(textEl('span', 'b-key', String(d.key)));
    el.appendChild(head);
    el.appendChild(textEl('div', 'b-quest', String(d.quest || '')));
    if (d.line) {
      var line = textEl('div', 'b-line', String(d.line));
      if (d.more > 0) line.appendChild(textEl('span', 'b-more', '+' + d.more + ' more'));
      el.appendChild(line);
    }
    if (d.where) el.appendChild(textEl('div', 'b-where', String(d.where)));
    return el;
  }

  function lineBoxEl(d) {
    var el = textEl('div', 'box pre game' + (d.fold ? ' more' : ''), '');
    var t = textEl('div', 'b-text', '');
    renderLine(t, String(d.text == null ? '' : d.text));
    el.appendChild(t);
    return el;
  }

  // ---- clocks ----
  // A backlog is waiting: then only the oldest box's clock runs (the others keep their place until it is their turn),
  // so boxes leave one at a time instead of together when their own clocks run out.
  function backlog() { return waiting.length > 0 || folded > 0; }

  // Time a box has really been visible (wall clock, minus time paused).
  function visibleMs(b) {
    var pausedNow = pausedTotal + (pauseStart ? Date.now() - pauseStart : 0);
    return Date.now() - b.addedAt - (pausedNow - b.pausedAtAdd);
  }

  function startClock(b) {
    if (b.timer || paused()) return;
    if (backlog() && b !== boxes[0]) return;
    b.started = Date.now();
    b.timer = setTimeout(function () { dismiss(b); }, Math.max(0, b.remaining));
  }

  function stopClock(b) {
    if (!b.timer) return;
    clearTimeout(b.timer);
    b.timer = 0;
    b.remaining -= Date.now() - b.started;
  }

  // Others are waiting: let this box go once it has been seen for MIN_MS, and not sooner than PACE_MS after the box
  // before it left. Called for the oldest box only; the next one is hurried when this one has gone.
  function hurry(b) {
    if (!b || b.hurried) return;
    b.hurried = true;
    stopClock(b);
    b.remaining = Math.min(b.remaining, Math.max(0, b.minMs - visibleMs(b), lastLeave + PACE_MS - Date.now()));
    startClock(b);  // it is the oldest: its clock runs (unless paused)
  }

  function dismiss(b) {
    var at = boxes.indexOf(b);
    if (at === -1) return;
    boxes.splice(at, 1);
    if (b.timer) { clearTimeout(b.timer); b.timer = 0; }
    b.el.classList.add('out');
    lastLeave = Date.now();
    leaving += 1;
    setTimeout(function () {
      if (b.el.parentNode) b.el.parentNode.removeChild(b.el);
      leaving -= 1;
      checkIdle();
    }, 420);
    pump();
  }

  function checkIdle() {
    if (boxes.length || waiting.length || folded || leaving || gapTimer) return;
    if (idleSentFor === lastId || lastId === 0) return;  // nothing ever shown: nothing to report
    idleSentFor = lastId;
    send('toasts:idle:' + lastId);
  }

  function layerClass() {
    var pos = Math.max(0, Math.min(5, Number(lastLayout.pos) || 0));
    return 'toasts pos-' + pos + (paused() ? ' paused' : '');
  }

  function wordsOf(el) {
    var t = (el.textContent || '').trim();
    return t ? t.split(/\s+/).length : 0;
  }

  function addBox(d) {
    layer.className = layerClass();
    layer.style.fontSize = (Math.max(50, Math.min(200, Number(lastLayout.scale) || 100)) / 100) + 'rem';
    var game = d.box === 'game';
    var el = game ? lineBoxEl(d) : questBoxEl(d);
    var extra = Math.max(0, wordsOf(el) - LONG_WORDS) * EXTRA_MS_PER_WORD;
    var total = Math.min(60000, Math.max(1, Math.min(60, Number(d.secs) || (game ? 6 : 8))) * 1000 + extra);
    var minMs = Math.min(MIN_CAP_MS, (Number(d.min) > 0 ? Math.min(30, Number(d.min)) * 1000 : MIN_MS) + extra);
    var b = { el: el, total: total, remaining: total, started: 0, timer: 0, hurried: false, addedAt: Date.now(),
      pausedAtAdd: pausedTotal, minMs: minMs };
    layer.appendChild(el);  // 0.17.0: oldest first in the DOM; the CSS anchors the bottom spots at the bottom
    void el.offsetHeight;  // lay it out folded first, so the unfold animates
    el.classList.remove('pre');
    boxes.push(b);
    startClock(b);
  }

  // Shows waiting boxes while there is room; while some still wait, the oldest box on screen is hurried.
  function pump() {
    if (paused()) return;
    while (boxes.length < max && (waiting.length || folded)) {
      if (waiting.length) {
        addBox(waiting.shift());
      } else {
        var n = folded;
        folded = 0;
        addBox({ box: 'game', fold: true, secs: 6,
          text: '+' + n + ' more notification' + (n === 1 ? '' : 's') + ', not shown (all in QuestSearchOverlay.log)' });
      }
    }
    if (backlog()) {
      boxes.forEach(function (b, i) { if (i > 0) stopClock(b); });
      hurry(boxes[0]);
    } else {
      boxes.forEach(startClock);  // the backlog is through: every box's own clock runs again
    }
    checkIdle();
  }

  function applyPause() {
    var p = paused();
    if (p && !pauseStart) pauseStart = Date.now();
    if (!p && pauseStart) { pausedTotal += Date.now() - pauseStart; pauseStart = 0; }
    layer.classList.toggle('paused', p);
    boxes.forEach(p ? stopClock : startClock);
    if (!p) pump();
    checkIdle();
  }

  function enqueue(d) {
    lastLayout = { pos: d.pos, scale: d.scale };  // one stack: the newest box's position and size apply to all
    max = Math.max(1, Math.min(10, Number(d.max) || 5));
    waiting.push(d);
    while (waiting.length > WAIT_CAP) { waiting.shift(); folded += 1; }
  }

  // Puts every box that is next in line into the queue, in id order.
  function drain() {
    while (early[nextId]) {
      enqueue(early[nextId]);
      delete early[nextId];
      nextId += 1;
    }
    var left = Object.keys(early).length > 0;
    if (left && !gapTimer) {
      gapTimer = setTimeout(giveUpGap, GAP_MS);
    } else if (!left && gapTimer) {
      clearTimeout(gapTimer);
      gapTimer = 0;
    }
    pump();
  }

  function giveUpGap() {
    gapTimer = 0;
    var ids = Object.keys(early).map(Number).sort(function (a, b) { return a - b; });
    if (!ids.length) return;
    send('log:boxes ' + nextId + (ids[0] - 1 > nextId ? '-' + (ids[0] - 1) : '') + ' never reached the page; going on with ' + ids[0]);
    nextId = ids[0];
    drain();
  }

  window.qsoToast = function (json) {
    var d;
    try { d = JSON.parse(json || '{}'); } catch (e) { send('log:bad qsoToast json'); return; }
    var id = Number(d.id) || 0;
    lastId = Math.max(lastId, id);
    if (!id || id < nextId) {  // no id, or one we already gave up waiting for: show it now rather than never
      enqueue(d);
      pump();
      return;
    }
    early[id] = d;
    drain();
  };

  // 0.17.0: the Notifications tab (overlay.js) shows game lines with the same renderer.
  window.qsoRenderLine = renderLine;

  window.qsoToastPause = function (flag) {
    pluginPaused = String(flag) === '1' || flag === true;
    applyPause();
  };

  window.qsoBoxesPanel = function (open) {
    panelOpen = !!open;
    applyPause();
  };
})();
