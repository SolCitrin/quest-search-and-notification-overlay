// Quest Search and Notification Overlay view: the J panel (0.10.0: list, search, filters, tracking, detail pane, sort, NEW badges, untrack all,
// show on map, objective locations, nearby quests).
// C++ -> JS: window.qsoOpened(json), window.qsoRefresh(listJson), window.qsoClosed(), window.qsoNotice(text)
//            0.11.0: window.qsoToast(json), window.qsoToastPause('1' | '0'), window.qsoSettings(json)
// 0.13.0: qsoOpened json has "pins":["0001A2B3",...] (pinned quest FormIDs, from the save's co-save) and
//   settings.bGroupHeadings; the page sends 'pin:<formID hex>:<1|0>'.
// 0.14.0: qsoOpened json has "hidden":[...] too; the page sends 'hide:<formID hex>:<1|0>'. Tabs down the left: Pinned,
//   Nearby and Hidden are single views (`single`), Main / Side / Misc / Completed combine (`filters`).
// 0.15.0: each quest has "notes":[{"book":"000BA9C","name":"Missive: ...","text":"plain text"}] (running quests only,
//   notes the player carries or has read). Shown folded under the description; the open state is page-only.
// 0.16.0: the notification boxes moved to boxes.js (one stack with the game's own lines); this file tells it when the
//   panel opens / closes (window.qsoBoxesPanel). qsoOpened json has "captureHooked":true|false (the game-notification
//   hook went in at startup) and settings.bCaptureEnabled / iLineSeconds (Settings page "Game notifications").
// 0.16.1: qsoOpened json has "b612":true|false (B612 NotificationManager installed; the Settings page mentions it only then).
// 0.17.0: the Notifications tab (single view 'history') sends 'history'; the plugin answers window.qsoHistory(json):
//   {"cap":300,"total":N,"entries":[{"n","real":"20:14:05","game":"8:14 PM, 17 Last Seed","kind":"quest"|"game","how",
//   "folded", quest fields or "text"}]} newest first. Settings key iMinBoxSeconds (Min time on screen, 1-30).
// JS -> C++: window.qsoCommand('close' | 'track:<formID hex>:<1|0>' | 'untrackall' |
//            'map:<formID hex>:<instance>:<objective index>' |
//            0.11.0: 'toasts:idle:<newest box id>' | 'settings:<key>=<n>;<key>=<n>' | 'settings:reset' | 'toasttest')
// qsoToast json: {"id":3,"pos":1,"secs":8,"scale":100,"max":5,"kind":"arrive|leave|near|updated","head":"Here in Whiterun",
//   "quest":"Hunt and Gather","line":"Bring venison to Anoriath","where":"The Drunken Huntsman","more":0,"key":"J"}
// qsoOpened json also has "settings":{"bOpenOnMap":1,"bNotifyArrive":1,...,"iMaxBoxes":5} (the Settings page keys, see Settings.cpp).
// qsoOpened json: {"build":"0.7.0","loadId":1,"journalKey":36,
//   "options":{"showCompleted":false,"rememberSearch":false,"maxObjectives":3},"list":{"valid":true,"problem":"","quests":[
//   {"id":"0001A2B3","inst":0,"type":1,"typeName":"Main","name":"...","plugin":"Skyrim.esm","desc":"journal text","tracked":true,"completed":false,
//    "failed":false,"objectives":[{"state":1,"index":10,"map":true,"loc":"0001A2B3","place":"Dragonsreach",
//    "town":"Whiterun","hold":"Whiterun Hold","area":"00018A56","holdId":"00016772","text":"..."}]}],
//   "here":{"area":"00018A56","hold":"00016772","placed":true,"radius":300,"areaName":"Whiterun","holdName":"Whiterun Hold"}}}
//   (area = town/city, else the dungeon or site under the hold; "00000000" = none)
//   0.10.4: each objective also has "dist": metres from the player, both taken outdoors (-1 = unknown); "radius" is
//   iNearbyRadius. Nearby = in the town you are in (HERE) or within the radius (NEAR).
//   (objective state 1 shown, 3 completed, 5 failed; map = the game has a map target for it, like the journal's button)
(function () {
  'use strict';
  var panel = document.getElementById('panel');
  var search = document.getElementById('search');
  var build = document.getElementById('build');
  var list = document.getElementById('list');
  var empty = document.getElementById('empty');
  var status = document.getElementById('status');
  var scroll = document.getElementById('scroll');
  var jkeyLabel = document.getElementById('jkey');
  var toast = document.getElementById('toast');
  var detail = document.getElementById('detail');
  var sortButton = document.getElementById('sort');
  var untrackButton = document.getElementById('untrackall');
  var nearbyButton = document.getElementById('nearby');
  var here = { area: '', hold: '', placed: false, radius: 300, areaName: '', holdName: '' };  // where the player is, from the plugin
  // 0.14.0: '' = the combined tabs (Main / Side / Misc / Completed), else one single view: 'pinned', 'nearby' or 'hidden'.
  var single = '';
  var nearbyWanted = false; // the player left NEARBY on when closing: open on it again while it has something to show
  var hiddenSet = {};       // quest FormID -> true; from the plugin at each open (kept per save in the co-save)
  var singleTabs = Array.prototype.slice.call(document.querySelectorAll('.tab[data-view]'));
  var toastTimer = 0;
  var chips = Array.prototype.slice.call(document.querySelectorAll('.chip[data-filter]'));

  var quests = [];       // prepared rows
  var listProblem = '';
  var journalCode = 'KeyJ';
  var filters = { main: true, side: true, misc: true, done: false };
  var options = { showCompleted: false, rememberSearch: false, maxObjectives: 3 };
  var visible = [];      // rows currently on screen, in order
  var selKey = '';       // keyboard selection (quest key), '' = none
  var PAGE = 5;
  var openedBefore = false;
  var hoverKey = '';      // quest under the mouse (drives the detail pane, like the keyboard selection)
  var SORTS = [
    { key: 'tracked', label: 'Tracked first' },
    { key: 'newest', label: 'Newest' },
    { key: 'az', label: 'A\u2013Z' },
    { key: 'type', label: 'Type' }
  ];
  var sortIndex = 0;      // kept for the whole game session
  // ---- 0.13.0 group headings + pins ----
  var pinSet = {};        // quest FormID -> true; from the plugin at each open (kept per save in the co-save)
  var folded = {};        // group key -> true; kept for the whole game session, like the sort
  var openNotes = {};     // 0.15.0: quest key + '#' + book -> true; kept for the whole game session
  var groupedNow = false; // this render shows type headings (so rows drop their type badge)
  // Engine quest types (QuestReader TypeName): 0 None, 1 Main, 2 College, 3 Thieves, 4 DarkBrotherhood, 5 Companions,
  // 6 Misc, 7 Daedric, 8 Side, 9 CivilWar, 10 Dawnguard, 11 Dragonborn. CC and mod quests use these too.
  var GROUPS = [
    { key: 'pinned', label: 'Pinned' }, { key: 't1', label: 'Main' }, { key: 't9', label: 'Civil War' },
    { key: 't10', label: 'Dawnguard' }, { key: 't11', label: 'Dragonborn' }, { key: 't5', label: 'Companions' },
    { key: 't2', label: 'College of Winterhold' }, { key: 't3', label: 'Thieves Guild' },
    { key: 't4', label: 'Dark Brotherhood' }, { key: 't7', label: 'Daedric' }, { key: 't8', label: 'Side' },
    { key: 't6', label: 'Misc' }, { key: 'done', label: 'Completed' }
  ];
  var GROUP_RANK = {};
  GROUPS.forEach(function (g, i) { GROUP_RANK[g.key] = i; });

  function headingsOn() { return Number(settingsValues.bGroupHeadings) !== 0; }  // missing = on (the default)

  // Which group a quest is listed under. Headings off: only the Pinned block is separate ('' = the flat list).
  function groupOf(q) {
    if (q.done) return headingsOn() ? 'done' : '';
    if (q.pinned) return 'pinned';
    if (!headingsOn()) return '';
    var t = q.type === 0 ? 6 : q.type;
    return GROUP_RANK['t' + t] !== undefined ? 't' + t : 't8';  // an unknown type goes with Side
  }
  var seen = null;        // key -> signature at the last open; null = nothing to compare yet (first open after a load)
  var lastLoadId = -1;  // with rememberSearch, the INI defaults still apply on the first open of a session

  // DirectInput scan code (what the game's control map stores) -> KeyboardEvent.code, for the keys a player
  // would plausibly bind Journal to. Unknown codes fall back to J.
  var SCAN_TO_CODE = (function () {
    var map = {};
    function row(start, keys) { for (var i = 0; i < keys.length; i++) map[start + i] = keys[i]; }
    row(0x02, ['Digit1', 'Digit2', 'Digit3', 'Digit4', 'Digit5', 'Digit6', 'Digit7', 'Digit8', 'Digit9', 'Digit0']);
    row(0x10, ['KeyQ', 'KeyW', 'KeyE', 'KeyR', 'KeyT', 'KeyY', 'KeyU', 'KeyI', 'KeyO', 'KeyP']);
    row(0x1E, ['KeyA', 'KeyS', 'KeyD', 'KeyF', 'KeyG', 'KeyH', 'KeyJ', 'KeyK', 'KeyL']);
    row(0x2C, ['KeyZ', 'KeyX', 'KeyC', 'KeyV', 'KeyB', 'KeyN', 'KeyM']);
    row(0x3B, ['F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10']);
    map[0x57] = 'F11'; map[0x58] = 'F12'; map[0x0F] = 'Tab'; map[0x29] = 'Backquote'; map[0x1C] = 'Enter';
    map[0xC7] = 'Home'; map[0xCF] = 'End'; map[0xC9] = 'PageUp'; map[0xD1] = 'PageDown'; map[0xD2] = 'Insert';
    map[0xD3] = 'Delete'; map[0x0E] = 'Backspace';
    return map;
  })();

  // KeyboardEvent.code -> old-style keyCode, for engines that do not fill event.code.
  function codeToKeyCode(code) {
    if (/^Key[A-Z]$/.test(code)) return code.charCodeAt(3);
    if (/^Digit[0-9]$/.test(code)) return code.charCodeAt(5);
    var m = /^F([0-9]{1,2})$/.exec(code);
    if (m) return 111 + Number(m[1]);
    return { Tab: 9, Backquote: 192, Enter: 13, Home: 36, End: 35, PageUp: 33, PageDown: 34, Insert: 45, Delete: 46, Backspace: 8 }[code] || -1;
  }

  function isJournalKey(event) {
    if (event.code) return event.code === journalCode;
    return event.keyCode === codeToKeyCode(journalCode);
  }

  function keyLabel(code) {
    if (/^Key[A-Z]$/.test(code)) return code.charAt(3);
    if (/^Digit[0-9]$/.test(code)) return code.charAt(5);
    return code;
  }

  // Lower case, accents removed, so "dragon" finds "Dragón" and the like.
  function fold(text) {
    var s = String(text || '').toLowerCase();
    try { s = s.normalize('NFD').replace(/[\u0300-\u036f]/g, ''); } catch (e) { /* engine without normalize */ }
    return s;
  }

  function realId(id) { id = String(id || ''); return /^0*$/.test(id) ? '' : id; }

  function withinRadius(o) { return here.placed && o.dist >= 0 && o.dist <= here.radius; }

  // 2 = an open objective in the town/area the player is in, 1 = one within the nearby radius (0.10.4; was "same
  // hold"), 0 = not nearby.
  function nearness(q) {
    if (q.done || q.hidden) return 0;  // 0.14.0: a hidden quest is silent
    var best = 0;
    q.objectives.forEach(function (o) {
      if (o.state !== 1) return;
      if (here.area && o.area === here.area) best = 2;
      else if (best < 1 && withinRadius(o)) best = 1;
    });
    return best;
  }

  // Metres to the quest's closest open objective that counts as nearby (in this town, or within the radius), rounded
  // to 10; -1 = no distance known. Used by both tags (0.10.5: HERE keeps its distance too, user request).
  function nearestMetres(q) {
    var best = -1;
    q.objectives.forEach(function (o) {
      if (o.state !== 1 || !here.placed || o.dist < 0) return;
      if (!withinRadius(o) && !(here.area && o.area === here.area)) return;
      if (best < 0 || o.dist < best) best = o.dist;
    });
    return best < 0 ? -1 : Math.round(best / 10) * 10;
  }

  function withMetres(label, q) { var m = nearestMetres(q); return m < 0 ? label : label + ' · ' + m + ' m'; }
  function nearTag(q) { return tag(withMetres('Near', q), 'tag-hold'); }
  function hereTag(q) { return tag(withMetres('Here', q), 'tag-here'); }

  function setHere(raw) {
    raw = raw || {};
    var radius = Number(raw.radius);
    here = { area: realId(raw.area), hold: realId(raw.hold), placed: raw.placed === true, radius: radius > 0 ? radius : 300,
      areaName: String(raw.areaName || ''), holdName: String(raw.holdName || '') };
  }

  function bucketOf(type) {
    if (type === 1) return 'main';
    if (type === 6 || type === 0) return 'misc';
    return 'side';
  }

  function prepare(raw) {
    var objectives = (raw.objectives || []).map(function (o) {
      var place = String(o.place || ''), town = String(o.town || ''), hold = String(o.hold || '');
      // Short form for the list ("Dragonsreach, Whiterun"), full form for the detail pane (+ the hold).
      var parts = [];
      [place, town, hold].forEach(function (p) { if (p && parts.indexOf(p) === -1) parts.push(p); });
      var shortWhere = parts.slice(0, parts.length > 2 ? 2 : parts.length).join(', ');
      if (parts.length === 1 && hold && parts[0] === hold) shortWhere = hold;
      return { state: o.state, index: o.index, map: !!o.map && o.state === 1, text: String(o.text || ''), folded: fold(o.text),
        loc: String(o.loc || ''), place: place, town: town, hold: hold, where: shortWhere, whereFull: parts.join(' \u00B7 '),
        area: realId(o.area), holdId: realId(o.holdId), dist: typeof o.dist === 'number' ? o.dist : -1,
        whereFolded: fold(parts.join(' ')) };
    });
    // The objective "Show on map" uses for the whole quest: the newest open one with a target (the journal's default).
    var mapObjective = null;
    for (var m = 0; m < objectives.length && !mapObjective; m++) if (objectives[m].map) mapObjective = objectives[m];
    var plugin = String(raw.plugin || '');
    var leftover = !raw.completed && !raw.failed && bucketOf(raw.type) === 'misc' &&
      (raw.objectives || []).length > 0 && !(raw.objectives || []).some(function (o) { return o.state === 1; });
    var desc = String(raw.desc || '');
    var notes = (raw.notes || []).map(function (n) {
      var name = String(n.name || 'Note');
      var text = String(n.text || '');
      return { book: String(n.book || ''), name: name, text: text, folded: fold(name + ' ' + text) };
    });
    return {
      id: raw.id,
      inst: raw.inst,
      // A quest with no name (radiant favours) goes by its newest objective, as in the vanilla journal's Misc list.
      name: String(raw.name || (objectives[0] && objectives[0].text) || '(unnamed quest)'),
      folded: fold(raw.name || (objectives[0] && objectives[0].text)),
      plugin: plugin,
      pluginShort: plugin.replace(/\.(esp|esm|esl)$/i, ''),
      pluginFolded: fold(plugin),
      desc: desc,
      descFolded: fold(desc),
      notes: notes,
      type: raw.type,
      // What "changed" means for the NEW badge: the objectives (text + state) and the journal text.
      sig: desc + '|' + (raw.objectives || []).map(function (o) { return o.state + ':' + o.text; }).join('|'),
      isNew: false,
      typeName: String(raw.typeName || ''),
      bucket: bucketOf(raw.type),
      tracked: !!raw.tracked,
      pinned: !!pinSet[String(raw.id || '')],
      hidden: !!hiddenSet[String(raw.id || '')],
      done: !!raw.completed || !!raw.failed || leftover,
      failed: !!raw.failed,
      // "Finished": a misc quest that is still running but has no objective left to do (e.g. the vanilla
      // "An enemy's gratitude" letter quests, WIKill*). The vanilla journal's Misc list shows only open objectives, so
      // these never appear there; here they go with the completed ones instead of cluttering the open list.
      leftover: leftover,
      status: raw.failed ? 'Failed' : (raw.completed ? 'Completed' : (leftover ? 'Finished' : '')),
      objectives: objectives,
      mapObjective: mapObjective,
      order: 0
    };
  }

  function tokens() {
    return fold(search.value).split(/\s+/).filter(function (t) { return t.length > 0; });
  }

  // Every search word must appear somewhere in the quest: name, objectives, journal text or plugin name.
  function matches(quest, words) {
    for (var i = 0; i < words.length; i++) {
      var w = words[i];
      if (quest.folded.indexOf(w) !== -1 || quest.descFolded.indexOf(w) !== -1 || quest.pluginFolded.indexOf(w) !== -1) continue;
      var found = false;
      for (var n = 0; n < quest.notes.length && !found; n++) found = quest.notes[n].folded.indexOf(w) !== -1;
      for (var k = 0; k < quest.objectives.length && !found; k++) {
        found = quest.objectives[k].folded.indexOf(w) !== -1 ||
          (quest.objectives[k].state === 1 && quest.objectives[k].whereFolded.indexOf(w) !== -1);
      }
      if (!found) return false;
    }
    return true;
  }

  // Appends text to a node with every search word wrapped in <mark>. Built with text nodes only (no innerHTML),
  // so quest text can never inject markup.
  function appendHighlighted(node, text, words) {
    var folded = fold(text);
    if (folded.length !== text.length || words.length === 0) {
      // Folding changed the length (rare characters): no offsets to trust, show plain text.
      node.appendChild(document.createTextNode(text));
      return;
    }
    var marks = new Array(text.length);
    words.forEach(function (w) {
      var at = folded.indexOf(w);
      while (at !== -1) {
        for (var i = at; i < at + w.length; i++) marks[i] = true;
        at = folded.indexOf(w, at + w.length);
      }
    });
    var i = 0;
    while (i < text.length) {
      var j = i;
      while (j < text.length && !!marks[j] === !!marks[i]) j++;
      var piece = document.createTextNode(text.slice(i, j));
      if (marks[i]) {
        var mark = document.createElement('mark');
        mark.appendChild(piece);
        node.appendChild(mark);
      } else {
        node.appendChild(piece);
      }
      i = j;
    }
  }

  function objectiveHits(objective, words) {
    for (var i = 0; i < words.length; i++) if (objective.folded.indexOf(words[i]) !== -1 || objective.whereFolded.indexOf(words[i]) !== -1) return true;
    return false;
  }

  function renderRow(quest, words) {
    var li = document.createElement('li');
    li.className = 'quest' + (quest.tracked ? ' tracked' : '') + (quest.done ? ' done' : '');
    li.setAttribute('data-index', String(quests.indexOf(quest)));
    li.title = quest.done && !quest.tracked ? 'Nothing left to track in this quest' : (quest.tracked ? 'Click to stop tracking' : 'Click to track');

    var head = document.createElement('div');
    head.className = 'head';
    var star = textEl('span', 'star' + (quest.pinned ? ' on' : ''), quest.pinned ? '\u2605' : '\u2606');
    star.title = quest.pinned ? 'Unpin (P)' : 'Pin to the top (P)';
    head.appendChild(star);
    var name = document.createElement('span');
    name.className = 'name';
    appendHighlighted(name, quest.name, words);
    head.appendChild(name);
    var tags = document.createElement('span');
    tags.className = 'tags';
    if (quest.isNew) tags.appendChild(tag('New', 'tag-new'));
    if (quest.tracked) tags.appendChild(tag('Tracked', 'tag-tracked'));
    if (quest.done) tags.appendChild(tag(quest.status, 'tag-done'));
    var near = nearness(quest);
    if (near === 2) tags.appendChild(hereTag(quest));
    else if (near === 1) tags.appendChild(nearTag(quest));
    if (!groupedNow) tags.appendChild(tag(typeLabel(quest), 'tag-type'));  // the heading says it (0.13.0)
    head.appendChild(tags);
    li.appendChild(head);
    if (quest.plugin) {
      var from = document.createElement('div');
      from.className = 'from';
      appendHighlighted(from, quest.pluginShort, words);
      li.appendChild(from);
    }

    // Current objectives first (newest first, like the journal); finished ones only when they match the search.
    var shown = quest.objectives.filter(function (o) { return o.state === 1; });
    var extra = quest.objectives.filter(function (o) { return o.state !== 1 && words.length > 0 && objectiveHits(o, words); });
    if (quest.done && shown.length === 0 && extra.length === 0) shown = quest.objectives.slice(0, 1);
    var max = options.maxObjectives;
    var hidden = Math.max(0, shown.length - max);
    shown = shown.slice(0, max).concat(extra.slice(0, max));
    if (shown.length > 0) {
      var ul = document.createElement('ul');
      ul.className = 'objectives';
      shown.forEach(function (o) {
        var item = document.createElement('li');
        if (o.state !== 1) item.className = 'finished';
        appendHighlighted(item, o.text, words);
        if (o.state === 1 && o.where) {
          var at = textEl('span', 'where', '');
          appendHighlighted(at, o.where, words);
          item.appendChild(at);
        }
        ul.appendChild(item);
      });
      if (hidden > 0) {
        var more = document.createElement('li');
        more.className = 'more';
        more.textContent = '+' + hidden + ' more objective' + (hidden === 1 ? '' : 's');
        ul.appendChild(more);
      }
      li.appendChild(ul);
    }
    return li;
  }

  function headingRow(key, count, isFolded) {
    var li = textEl('li', 'group' + (isFolded ? ' folded' : '') + (key === 'pinned' ? ' g-pinned' : ''), '');
    li.setAttribute('data-group', key);
    li.appendChild(textEl('span', 'g-arrow', isFolded ? '\u25B8' : '\u25BE'));
    li.appendChild(textEl('span', 'g-name', GROUPS[GROUP_RANK[key]].label));
    li.appendChild(textEl('span', 'g-count', String(count)));
    li.title = isFolded ? 'Click to show these quests' : 'Click to fold this group';
    return li;
  }

  function typeLabel(quest) {
    return quest.typeName === 'None' ? 'Misc' : quest.typeName.replace(/([a-z])([A-Z])/g, '$1 $2');
  }

  function tag(text, cls) {
    var s = document.createElement('span');
    s.className = 'tag ' + cls;
    s.textContent = text;
    return s;
  }

  function render(resetScroll) {
    var words = tokens();
    columnsEl.classList.toggle('history-mode', single === 'history');
    panel.classList.toggle('history-on', single === 'history');  // 0.17.1: footer key hints
    var histShown = [];
    if (single === 'history') {
      visible = [];  // no quest rows: keyboard selection, Enter, P, H and M do nothing here
      histShown = history ? history.entries.filter(function (e) { return historyMatches(e, words); }) : [];
    } else if (single === 'nearby') {
      // Nearby mode ignores the type filters: everything with work in this town first, then what is within the radius.
      visible = quests.filter(function (q) { return nearness(q) > 0 && matches(q, words); });
      visible.sort(function (a, b) { return (nearness(b) - nearness(a)) || (quests.indexOf(a) - quests.indexOf(b)); });
    } else if (single) {
      // 0.14.0 Pinned / Hidden: every quest of that kind, finished ones last (quests[] is in sort order).
      visible = quests.filter(function (q) { return inSingle(q, single) && matches(q, words); });
    } else {
      visible = quests.filter(function (q) { return inCombined(q) && matches(q, words); });
    }
    while (list.firstChild) list.removeChild(list.firstChild);
    var frag = document.createDocumentFragment();
    if (single === 'history') {
      groupedNow = false;
      histShown.forEach(function (e) { frag.appendChild(historyRow(e, words)); });
    } else if (single) {
      groupedNow = false;  // the single views are flat lists; pinned rows keep their star
      visible.forEach(function (q) { frag.appendChild(renderRow(q, words)); });
    } else {
      // 0.13.0: group order first, then the chosen sort (quests[] is already in sort order).
      var pos = {};
      quests.forEach(function (q, i) { pos[keyOf(q)] = i; });
      visible.forEach(function (q) { q.group = groupOf(q); });
      visible.sort(function (a, b) {
        var ra = a.group ? GROUP_RANK[a.group] : 0.5, rb = b.group ? GROUP_RANK[b.group] : 0.5;  // flat rows after Pinned
        return (ra - rb) || (pos[keyOf(a)] - pos[keyOf(b)]);
      });
      groupedNow = headingsOn();
      var counts = {};
      visible.forEach(function (q) { counts[q.group] = (counts[q.group] || 0) + 1; });
      var openAll = words.length > 0;  // while searching every group is open, so a hit is never hidden
      var shown = [];
      var current = null;
      visible.forEach(function (q) {
        if (q.group !== current) {
          current = q.group;
          if (current) frag.appendChild(headingRow(current, counts[current], !openAll && !!folded[current]));
        }
        if (current && !openAll && folded[current]) return;
        shown.push(q);
        frag.appendChild(renderRow(q, words));
      });
      visible = shown;  // keyboard selection moves over the shown rows only
    }
    list.appendChild(frag);
    if (resetScroll) scroll.scrollTop = 0;
    if (selKey && selIndex() === -1) selKey = visible.length ? keyOf(visible[0]) : '';
    paintSelection();
    renderDetail(words);
    untrackButton.disabled = !quests.some(function (q) { return q.tracked; });

    var total = quests.filter(function (q) { return !q.hidden && (filters.done || !q.done); }).length;
    var tracked = quests.filter(function (q) { return q.tracked; }).length;
    var fresh = quests.filter(function (q) { return q.isNew && !q.hidden; }).length;
    var nHere = quests.filter(function (q) { return nearness(q) === 2; }).length;
    var nNear = quests.filter(function (q) { return nearness(q) === 1; }).length;
    var nHidden = quests.filter(function (q) { return q.hidden; }).length;
    singleTabs.forEach(function (t) {
      var v = t.getAttribute('data-view');
      if (v === 'history') {  // its count is known only once it has been opened in this game session
        tabText(t, history ? (words.length ? history.entries.filter(function (e) { return historyMatches(e, words); }).length : history.entries.length) : '');
        t.classList.toggle('on', single === v);
        return;
      }
      var n = v === 'nearby' ? nHere + nNear : quests.filter(function (q) { return inSingle(q, v) && matches(q, words); }).length;
      tabText(t, n);
      t.classList.toggle('on', single === v);
    });
    nearbyButton.classList.toggle('glow', nHere > 0);
    nearbyButton.classList.toggle('warm', nHere === 0 && nNear > 0);
    nearbyButton.title = (here.areaName ? 'You are in ' + here.areaName + '. ' : '') +
      nHere + ' quest' + (nHere === 1 ? '' : 's') + ' with an objective in this town, ' + nNear + ' more within ' + here.radius +
      ' m. Click to show only those.';
    if (single === 'history') {
      status.textContent = 'Notifications: ' + (history ? 'showing ' + histShown.length + ' of ' + history.entries.length +
        ' (this game session, the newest ' + history.cap + ' kept)' : 'loading\u2026');
    } else status.textContent = (single ? SINGLE_NAMES[single] + ': ' : '') + 'Showing ' + visible.length + ' of ' + total +
      (tracked ? ' \u00b7 ' + tracked + ' tracked' : '') + (fresh ? ' \u00b7 ' + fresh + ' new' : '') +
      (nHidden ? ' \u00b7 ' + nHidden + ' hidden' : '');
    // Each tab shows how many quests it covers with the current search; the combined tabs are lit only in that view.
    chips.forEach(function (chip) {
      var key = chip.getAttribute('data-filter');
      var n = quests.filter(function (q) {
        if (q.hidden || !matches(q, words)) return false;
        if (key === 'done') return q.done;
        return q.bucket === key && (filters.done || !q.done);
      }).length;
      tabText(chip, n);
      chip.classList.toggle('on', !single && !!filters[key]);
    });
    if (single === 'history') {
      empty.textContent = !history ? 'Loading the notifications\u2026' : histShown.length ? '' : history.entries.length ?
        'No notification matches "' + search.value.trim() + '".' :
        'No notifications yet in this game session. Quest boxes and the game\'s own lines are listed here as they come (not saved: the list is gone when the game closes). Click NOTIFICATIONS again to go back.';
    } else if (listProblem) {
      empty.textContent = 'Could not read the quest list (' + listProblem + '). The pause menu Quests tab still works.';
    } else if (quests.length === 0) {
      empty.textContent = 'Your journal has no quests yet.';
    } else if (visible.length === 0 && single === 'pinned') {
      empty.textContent = 'No pinned quests' + (words.length > 0 ? ' match "' + search.value.trim() + '"' : '') +
        '. Pin one with its star or P. Click PINNED again to see everything.';
    } else if (visible.length === 0 && single === 'hidden') {
      empty.textContent = 'No hidden quests' + (words.length > 0 ? ' match "' + search.value.trim() + '"' : '') +
        '. Hide one with H or the Hide button on the right. Click HIDDEN again to see everything.';
    } else if (visible.length === 0 && single === 'nearby') {
      empty.textContent = (here.areaName ? 'Nothing to do in ' + here.areaName + ' or within ' + here.radius + ' m' : 'Nothing to do within ' + here.radius + ' m') +
        (words.length > 0 ? ' that matches "' + search.value.trim() + '"' : '') +
        '. Only objectives with a quest marker count. Click NEARBY again to see everything.';
    } else if (visible.length === 0) {
      empty.textContent = words.length > 0 ? 'No quest matches "' + search.value.trim() + '" with these filters.' : 'No quests with these filters.';
    } else {
      empty.textContent = '';
    }
    // Say when the search would find quests that a filter is hiding, so an empty list is never a dead end.
    var hiddenHits = (words.length === 0 || single) ? 0 : quests.filter(function (q) {  // (never in the single views)
      return visible.indexOf(q) === -1 && !q.hidden && matches(q, words);  // groups are all open while searching
    }).length;
    if (hiddenHits > 0) {
      empty.textContent += (empty.textContent ? ' ' : '') + hiddenHits + (visible.length ? ' more' : '') + (hiddenHits === 1 ? ' quest matches' : ' quests match') +
        ' with other tabs (Main / Side / Misc / Completed on the left).';
    }
    empty.hidden = empty.textContent === '';
  }

  // ---- 0.14.0 tabs ----
  var SINGLE_NAMES = { pinned: 'Pinned', nearby: 'Nearby', hidden: 'Hidden', history: 'Notifications' };

  // ---- 0.17.0 Notifications tab: this game session's notifications, from the plugin (memory only) ----
  var history = null;    // {cap, total, entries[]} newest first; null = not asked yet / waiting
  var HOW = { 'box': '', 'vanilla': "game's own text", 'vanilla-disabled': "game's own text (switched off)",
    'vanilla-B612': "game's own text (B612)", 'vanilla-not-ready': "game's own text (page not ready)" };

  function stripTags(s) { return String(s || '').replace(/<[^<>]*>/g, ''); }

  function historyText(e) {
    if (e.kind === 'game') return stripTags(e.text);
    return [e.head, e.quest, e.line, e.where].filter(function (x) { return x; }).join(' \u00b7 ');
  }

  function historyMatches(e, words) {
    if (!words.length) return true;
    var hay = fold(historyText(e) + ' ' + (e.real || '') + ' ' + (e.game || ''));
    return words.every(function (w) { return hay.indexOf(w) !== -1; });
  }

  function historyRow(e, words) {
    var li = textEl('li', 'hist', '');
    var meta = textEl('div', 'h-meta', '');
    meta.appendChild(textEl('span', 'h-kind', e.kind === 'game' ? 'Game' : 'Quest'));
    meta.appendChild(textEl('span', 'h-time', (e.real || '') + (e.game ? ' \u00b7 ' + e.game : '')));
    if (e.folded) meta.appendChild(textEl('span', 'h-flag warn', 'not shown: too many at once'));
    if (HOW[e.how]) meta.appendChild(textEl('span', 'h-flag', HOW[e.how]));
    li.appendChild(meta);
    var body = textEl('div', 'h-body', '');
    if (e.kind === 'game') {
      if (words.length || typeof window.qsoRenderLine !== 'function') appendHighlighted(body, stripTags(e.text), words);
      else window.qsoRenderLine(body, String(e.text || ''));
    } else {
      var head = textEl('div', 'h-sub', '');
      appendHighlighted(head, String(e.head || ''), words);
      body.appendChild(head);
      var q = textEl('div', '', '');
      appendHighlighted(q, String(e.quest || ''), words);
      body.appendChild(q);
      if (e.line) { var l = textEl('div', 'h-sub', ''); appendHighlighted(l, '\u25c7 ' + e.line + (e.more > 0 ? ' (+' + e.more + ' more)' : ''), words); body.appendChild(l); }
      if (e.where) { var w = textEl('div', 'h-sub', ''); appendHighlighted(w, '\u2014 ' + e.where, words); body.appendChild(w); }
    }
    li.appendChild(body);
    return li;
  }

  window.qsoHistory = function (json) {
    try { history = JSON.parse(json || '{}'); } catch (e) { history = { cap: 0, total: 0, entries: [] }; }
    history.entries = history.entries || [];
    if (single === 'history') render(false);
  };

  // The combined tabs: Main / Side / Misc / Completed as switched on; hidden quests never.
  function inCombined(q) {
    if (q.hidden) return false;
    if (q.done && !filters.done) return false;
    return !!filters[q.bucket];
  }

  function inSingle(q, view) {
    if (view === 'pinned') return q.pinned && !q.hidden;  // hidden wins: a hidden quest shows only under Hidden
    if (view === 'hidden') return q.hidden;
    if (view === 'nearby') return nearness(q) > 0;
    return false;
  }

  function tabText(tab, n) {
    while (tab.firstChild) tab.removeChild(tab.firstChild);
    tab.appendChild(textEl('span', 't-label', tab.getAttribute('data-label')));
    tab.appendChild(textEl('span', 't-count', String(n)));
  }

  function setSingle(view) {
    single = single === view ? '' : view;  // clicking the lit single tab again goes back to the combined tabs
    nearbyWanted = single === 'nearby';
    if (single === 'history') { history = null; send('history'); }  // fresh each time the tab is opened
    selKey = '';
    render(true);
  }

  // 0.14.0: hidden per quest (all instances share it), saved by the plugin in the co-save. Silent: no boxes, not
  // nearby. Tracking and map markers are left alone.
  function toggleHide(quest) {
    if (!quest) return;
    var on = !quest.hidden;
    if (on) hiddenSet[quest.id] = true; else delete hiddenSet[quest.id];
    quests.forEach(function (q) { if (q.id === quest.id) q.hidden = on; });
    send('hide:' + quest.id + ':' + (on ? 1 : 0));
    showToast(on ? 'Hidden: ' + quest.name + '. It is under the Hidden tab now.' : 'Shown again: ' + quest.name, on ? 'off' : 'on');
    render();
  }

  function send(command) {
    if (typeof window.qsoCommand === 'function') window.qsoCommand(command);
  }

  // ---- detail pane: the selected quest (keyboard), else the one under the mouse, else the first one shown ----
  function focusedQuest() {
    var key = selKey || hoverKey;
    for (var i = 0; i < visible.length; i++) if (keyOf(visible[i]) === key) return visible[i];
    return visible[0] || null;
  }

  function renderDetail(words) {
    while (detail.firstChild) detail.removeChild(detail.firstChild);
    var q = focusedQuest();
    if (!q) {
      detail.appendChild(textEl('p', 'd-empty', 'No quest selected.'));
      return;
    }
    var title = textEl('h2', '', '');
    appendHighlighted(title, q.name, words);
    detail.appendChild(title);
    var meta = document.createElement('div');
    meta.className = 'd-meta';
    if (q.isNew) meta.appendChild(tag('New', 'tag-new'));
    if (q.hidden) meta.appendChild(tag('Hidden', 'tag-done'));
    if (q.tracked) meta.appendChild(tag('Tracked', 'tag-tracked'));
    if (q.done) meta.appendChild(tag(q.status, 'tag-done'));
    var qNear = nearness(q);
    if (qNear === 2) meta.appendChild(hereTag(q));
    else if (qNear === 1) meta.appendChild(nearTag(q));
    if (q.leftover) detail.appendChild(textEl('p', 'd-hint', 'Still running in the game, but it has nothing left to do. The vanilla journal does not list it; it is filed with the completed quests here.'));
    meta.appendChild(tag(typeLabel(q), 'tag-type'));
    detail.appendChild(meta);
    var pinButton = textEl('button', 'chip action d-pinbutton' + (q.pinned ? ' on' : ''), q.pinned ? '\u2605 Unpin' : '\u2606 Pin to the top');
    pinButton.type = 'button';
    pinButton.setAttribute('data-pin', '1');
    pinButton.title = q.pinned ? 'Remove the pin (P)' : 'Keep this quest at the top of the list, for this character (P)';
    detail.appendChild(pinButton);
    var hideButton = textEl('button', 'chip action d-hidebutton' + (q.hidden ? ' on' : ''), q.hidden ? 'Unhide' : 'Hide');
    hideButton.type = 'button';
    hideButton.setAttribute('data-hide', '1');
    hideButton.title = q.hidden ? 'Show this quest in the other tabs again (H)' :
      'Hide this quest: it shows only under the Hidden tab and sends no notifications. Tracking is not changed (H)';
    detail.appendChild(hideButton);
    if (q.mapObjective) {
      var mapButton = textEl('button', 'chip action d-mapbutton', 'Show on map');
      mapButton.type = 'button';
      mapButton.setAttribute('data-map', String(q.mapObjective.index));
      mapButton.title = 'Close this and open the map at: ' + q.mapObjective.text + ' (M)';
      detail.appendChild(mapButton);
    }
    if (q.plugin) {
      var from = textEl('div', 'd-from', 'From ');
      appendHighlighted(from, q.plugin, words);
      detail.appendChild(from);
    }
    var desc = textEl('p', 'd-desc', '');
    if (q.desc) appendHighlighted(desc, q.desc, words);
    else { desc.textContent = 'The journal has no description for this quest.'; desc.className += ' d-none'; }
    detail.appendChild(desc);
    // 0.15.0: the quest's notes (a missive, a letter), folded; a search word inside a note opens it.
    q.notes.forEach(function (n) {
      var noteKey = keyOf(q) + '#' + n.book;
      var hit = words.length > 0 && words.some(function (w) { return n.folded.indexOf(w) !== -1; });
      var open = !!openNotes[noteKey] || hit;
      var line = textEl('button', 'd-note' + (open ? ' open' : ''), '');
      line.type = 'button';
      line.setAttribute('data-note', noteKey);
      line.title = open ? 'Fold the note' : 'Show what the note says (the item is not opened)';
      line.appendChild(textEl('span', 'd-note-arrow', open ? '▾' : '▸'));
      var label = textEl('span', 'd-note-name', '');
      appendHighlighted(label, n.name, words);
      line.appendChild(label);
      detail.appendChild(line);
      if (open) {
        var body = textEl('p', 'd-note-text', '');
        appendHighlighted(body, n.text, words);
        detail.appendChild(body);
      }
    });
    if (q.objectives.length) {
      detail.appendChild(textEl('h3', '', 'Objectives'));
      var ul = document.createElement('ul');
      ul.className = 'objectives';
      q.objectives.forEach(function (o) {  // newest first, finished ones crossed out
        var item = document.createElement('li');
        if (o.state !== 1) item.className = 'finished';
        appendHighlighted(item, o.text, words);
        if (o.state === 1 && o.whereFull) {
          var at = textEl('div', 'd-where', '');
          appendHighlighted(at, o.whereFull, words);
          item.appendChild(at);
        }
        if (o.map) {
          var pin = textEl('button', 'd-pin', 'map');
          pin.type = 'button';
          pin.setAttribute('data-map', String(o.index));
          pin.title = 'Show this objective on the map';
          item.appendChild(pin);
        }
        ul.appendChild(item);
      });
      detail.appendChild(ul);
    }
    var hint = textEl('p', 'd-hint', q.done && !q.tracked ? '' : (q.tracked ? 'Enter or click the quest: stop tracking' : 'Enter or click the quest: track'));
    hint.textContent += (hint.textContent ? ' \u00b7 ' : '') + 'P or the star: ' + (q.pinned ? 'unpin' : 'pin') +
      ' \u00b7 H: ' + (q.hidden ? 'unhide' : 'hide');
    detail.appendChild(hint);
    if (!q.mapObjective && !q.done) {
      detail.appendChild(textEl('p', 'd-hint', 'Nothing to show on the map: no open objective has a marker (the vanilla journal has no Show on Map for it either).'));
    }
  }

  var columnsEl = document.getElementById('columns');

  function textEl(tagName, cls, text) {
    var el = document.createElement(tagName);
    if (cls) el.className = cls;
    el.textContent = text;
    return el;
  }

  // ---- sorting: applied on open and when the Sort button is pressed, never while clicking (rows stay put) ----
  function sortQuests() {
    if (headingsOn() && SORTS[sortIndex].key === 'type') sortIndex = 0;  // the headings already group by type
    var mode = SORTS[sortIndex].key;
    quests.sort(function (a, b) {
      if (a.done !== b.done) return a.done ? 1 : -1;
      if (mode === 'tracked' && a.tracked !== b.tracked) return a.tracked ? -1 : 1;
      if (mode === 'newest' && a.isNew !== b.isNew) return a.isNew ? -1 : 1;
      if (mode === 'az') { var c = a.name.localeCompare(b.name); if (c) return c; }
      if (mode === 'type') {
        var ta = a.type === 1 ? -1 : a.type, tb = b.type === 1 ? -1 : b.type;  // Main first
        if (ta !== tb) return ta - tb;
      }
      return a.order - b.order;
    });
    sortButton.textContent = 'Sort: ' + SORTS[sortIndex].label;
  }

  // ---- keyboard selection ----
  function selIndex() {
    for (var i = 0; i < visible.length; i++) if (keyOf(visible[i]) === selKey) return i;
    return -1;
  }

  function paintSelection() {
    var rows = list.querySelectorAll('li.quest');  // heading rows are not selectable
    var at = selIndex();
    for (var i = 0; i < rows.length; i++) {
      if (i === at) rows[i].classList.add('sel'); else rows[i].classList.remove('sel');
    }
    if (at >= 0 && rows[at]) {
      // Keep the selected row in view (manual, so it works without scrollIntoView options).
      var row = rows[at];
      var top = row.offsetTop - list.offsetTop;
      var prev = row.previousElementSibling;  // the first row of a group: keep its heading in view too
      if (prev && prev.classList.contains('group') && top - prev.offsetHeight < scroll.scrollTop) top -= prev.offsetHeight;
      if (top < scroll.scrollTop) scroll.scrollTop = top;
      else if (top + row.offsetHeight > scroll.scrollTop + scroll.clientHeight) scroll.scrollTop = top + row.offsetHeight - scroll.clientHeight;
    }
  }

  function select(index) {
    if (visible.length === 0) { selKey = ''; paintSelection(); return; }
    index = Math.max(0, Math.min(visible.length - 1, index));
    selKey = keyOf(visible[index]);
    paintSelection();
    renderDetail(tokens());
  }

  function leaveBox() {
    search.blur();
    if (selIndex() === -1) select(0);
  }

  function backToBox() {
    selKey = '';
    paintSelection();
    renderDetail(tokens());
    search.focus();
  }

  function syncChips() {
    chips.forEach(function (chip) {
      var key = chip.getAttribute('data-filter');
      if (filters[key]) chip.classList.add('on'); else chip.classList.remove('on');
    });
  }

  window.qsoOpened = function (json) {
    var data = {};
    try { data = JSON.parse(json || '{}'); } catch (e) { data = { list: { valid: false, problem: 'bad data from the plugin' } }; }
    build.textContent = 'Quest Search and Notification Overlay' + (data.build ? ' · build ' + data.build : '');
    captureHooked = !!data.captureHooked;
    b612Present = !!data.b612;
    var o = data.options || {};
    options.showCompleted = !!o.showCompleted;
    options.rememberSearch = !!o.rememberSearch;
    options.maxObjectives = Math.max(1, Math.min(10, Number(o.maxObjectives) || 3));
    journalCode = SCAN_TO_CODE[data.journalKey] || 'KeyJ';
    jkeyLabel.textContent = keyLabel(journalCode);
    if (data.settings) settingsValues = data.settings;
    pinSet = {};
    (data.pins || []).forEach(function (id) { pinSet[String(id).toUpperCase()] = true; });
    hiddenSet = {};
    (data.hidden || []).forEach(function (id) { hiddenSet[String(id).toUpperCase()] = true; });
    showSettings(false);
    loadList(data.list || {});
    // NEW = a quest that was not there, or whose objectives / journal text changed, since the last open. After a save
    // is loaded there is nothing to compare with, so the first open marks nothing.
    if (data.loadId !== lastLoadId) { seen = null; lastLoadId = data.loadId; }
    var nowSeen = {};
    quests.forEach(function (q) {
      q.isNew = seen !== null && seen[keyOf(q)] !== q.sig;
      nowSeen[keyOf(q)] = q.sig;
    });
    seen = nowSeen;
    sortQuests();
    if (!options.rememberSearch || !openedBefore) {
      openedBefore = true;
      search.value = '';
      filters = { main: true, side: true, misc: true, done: options.showCompleted };
      single = '';
    }
    // Reopen on NEARBY if it was on when the overlay was closed, unless nothing is nearby now (0.10.1, user request).
    if (nearbyWanted) {
      single = quests.some(function (q) { return nearness(q) > 0; }) ? 'nearby' : '';
    } else if (single === 'pinned' && !quests.some(function (q) { return inSingle(q, 'pinned'); })) {
      single = '';  // an empty single view would open on nothing (e.g. another save was loaded)
    } else if (single === 'hidden' && !quests.some(function (q) { return q.hidden; })) {
      single = '';
    }
    if (single === 'history') { history = null; send('history'); }
    syncChips();
    selKey = '';
    hoverKey = '';
    toast.textContent = '';
    toast.className = '';
    panel.hidden = false;
    applyPause();
    render(true);
    search.focus();
  };

  function loadList(raw) {
    listProblem = raw.valid === false ? String(raw.problem || 'unknown problem') : '';
    setHere(raw.here);
    quests = (raw.quests || []).map(prepare);
    quests.forEach(function (q, i) { q.order = i; });
  }

  function keyOf(q) { return q.id + '/' + q.inst; }

  // The game's real state after a track call: keep the current row order, search and filters, update the flags,
  // add any quest that appeared, drop any that left the journal.
  window.qsoRefresh = function (json) {
    if (panel.hidden) return;
    var raw;
    try { raw = JSON.parse(json || '{}'); } catch (e) { return; }
    if (raw.valid === false) return;  // keep showing what we have rather than blanking the list
    setHere(raw.here);
    var fresh = {};
    (raw.quests || []).forEach(function (r, i) { var q = prepare(r); q.order = i; fresh[keyOf(q)] = q; });
    var kept = [];
    quests.forEach(function (q) {
      var f = fresh[keyOf(q)];
      if (!f) return;
      f.order = q.order;
      f.isNew = q.isNew;
      kept.push(f);
      delete fresh[keyOf(q)];
    });
    Object.keys(fresh).forEach(function (k) { kept.push(fresh[k]); if (seen) seen[k] = fresh[k].sig; });
    quests = kept;
    render();
  };

  function showToast(text, cls) {
    toast.textContent = text;
    toast.className = 'show ' + (cls || '');
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { toast.className = ''; }, 2600);
  }

  list.addEventListener('mousedown', function (event) {
    var t = event.target;
    if (t && t.classList && (t.classList.contains('star') || t.classList.contains('group'))) event.preventDefault();
  });
  list.addEventListener('click', function (event) {
    var node = event.target;
    var g = node;
    while (g && g !== list && !(g.classList && g.classList.contains('group'))) g = g.parentNode;
    if (g && g !== list) {
      var gk = g.getAttribute('data-group');
      if (tokens().length > 0) { showToast('Groups stay open while you search.', 'warn'); return; }
      folded[gk] = !folded[gk];
      render();
      return;
    }
    var onStar = !!(node && node.classList && node.classList.contains('star'));
    while (node && node !== list && !(node.classList && node.classList.contains('quest'))) node = node.parentNode;
    if (!node || node === list) return;
    var quest = quests[Number(node.getAttribute('data-index'))];
    if (!quest) return;
    selKey = keyOf(quest);
    if (onStar) togglePin(quest); else toggleTrack(quest);
  });

  // 0.13.0: pins are per quest (all instances of a radiant quest share one), saved by the plugin in the co-save.
  function togglePin(quest) {
    if (!quest) return;
    var on = !quest.pinned;
    if (on) pinSet[quest.id] = true; else delete pinSet[quest.id];
    quests.forEach(function (q) { if (q.id === quest.id) q.pinned = on; });
    send('pin:' + quest.id + ':' + (on ? 1 : 0));
    showToast((on ? 'Pinned: ' : 'Unpinned: ') + quest.name, on ? 'on' : 'off');
    render();
  }

  // Same as the vanilla journal's "Show on Map": the plugin closes the overlay and opens the map at the target.
  function showOnMap(quest, index) {
    if (!quest) return;
    var objective = null;
    quest.objectives.forEach(function (o) { if (o.map && (index === undefined || o.index === index)) objective = objective || o; });
    if (!objective) {
      showToast(quest.done ? quest.status + ' quests have nothing to show on the map.' : 'This quest has nothing to show on the map.', 'warn');
      return;
    }
    send('map:' + quest.id + ':' + quest.inst + ':' + objective.index);
  }

  detail.addEventListener('mousedown', function (event) {
    var t = event.target;
    if (t && t.getAttribute && (t.getAttribute('data-map') !== null || t.getAttribute('data-pin') !== null ||
      t.getAttribute('data-hide') !== null)) event.preventDefault();
    var nb = noteButton(t);
    if (nb) event.preventDefault();  // keep the focus in the search box
  });
  function noteButton(node) {
    while (node && node !== detail) {
      if (node.getAttribute && node.getAttribute('data-note') !== null) return node;
      node = node.parentNode;
    }
    return null;
  }
  detail.addEventListener('click', function (event) {
    var node = event.target;
    var nb = noteButton(node);
    if (nb) {
      var nk = nb.getAttribute('data-note');
      if (openNotes[nk]) delete openNotes[nk]; else openNotes[nk] = true;
      renderDetail(tokens());
      return;
    }
    if (node && node.getAttribute && node.getAttribute('data-pin') !== null) { togglePin(focusedQuest()); return; }
    if (node && node.getAttribute && node.getAttribute('data-hide') !== null) { toggleHide(focusedQuest()); return; }
    if (!node || !node.getAttribute || node.getAttribute('data-map') === null) return;
    showOnMap(focusedQuest(), Number(node.getAttribute('data-map')));
  });

  window.qsoNotice = function (text) { showToast(String(text || ''), 'warn'); };

  function toggleTrack(quest) {
    if (quest.done && !quest.tracked) {  // a finished quest that is somehow still tracked can still be untracked
      showToast(quest.status + ' quests cannot be tracked.', 'warn');
      return;
    }
    var on = !quest.tracked;
    quest.tracked = on;  // shown at once; the plugin sends the game's real state back when the call finishes
    send('track:' + quest.id + ':' + (on ? 1 : 0));
    showToast((on ? 'Tracking: ' : 'Stopped tracking: ') + quest.name, on ? 'on' : 'off');
    render();
  }

  list.addEventListener('mouseover', function (event) {
    var node = event.target;
    while (node && node !== list && !(node.classList && node.classList.contains('quest'))) node = node.parentNode;
    if (!node || node === list) return;
    var quest = quests[Number(node.getAttribute('data-index'))];
    if (!quest || keyOf(quest) === hoverKey) return;
    hoverKey = keyOf(quest);
    if (!selKey) renderDetail(tokens());  // the keyboard selection wins while there is one
  });

  sortButton.addEventListener('mousedown', function (event) { event.preventDefault(); });
  sortButton.addEventListener('click', function () {
    sortIndex = (sortIndex + 1) % SORTS.length;
    if (headingsOn() && SORTS[sortIndex].key === 'type') sortIndex = (sortIndex + 1) % SORTS.length;
    sortQuests();
    render(true);
  });

  singleTabs.forEach(function (t) {
    t.addEventListener('mousedown', function (event) { event.preventDefault(); });  // keep focus where it is
    t.addEventListener('click', function () { setSingle(t.getAttribute('data-view')); });
  });

  untrackButton.addEventListener('mousedown', function (event) { event.preventDefault(); });
  untrackButton.addEventListener('click', function () {
    var n = 0;
    quests.forEach(function (q) { if (q.tracked) { q.tracked = false; n += 1; } });
    if (!n) return;
    send('untrackall');
    showToast('Stopped tracking ' + n + ' quest' + (n === 1 ? '' : 's') + '.', 'off');
    render();
  });

  window.qsoClosed = function () {
    panel.hidden = true;
    search.blur();
    showSettings(false);
    applyPause();
  };

  search.addEventListener('input', function () { selKey = ''; render(true); });

  chips.forEach(function (chip) {
    chip.addEventListener('mousedown', function (event) { event.preventDefault(); });  // keep focus where it is
    chip.addEventListener('click', function () {
      var key = chip.getAttribute('data-filter');
      // In a single view (Pinned / Nearby / Hidden) a combined tab first takes you back to the combined list.
      if (single) { single = ''; nearbyWanted = false; } else { filters[key] = !filters[key]; }
      syncChips();
      render(true);
    });
  });

  document.addEventListener('keydown', function (event) {
    if (panel.hidden) return;
    if (settingsOpen) {  // the Settings page: Esc goes back to the quests, the Journal key closes, nothing else
      if (event.key === 'Escape' || event.keyCode === 27) { event.preventDefault(); showSettings(false); search.focus(); }
      else if (isJournalKey(event)) { event.preventDefault(); send('close'); }
      return;
    }
    if (event.key === 'Escape' || event.keyCode === 27) {
      event.preventDefault();
      send('close');
      return;
    }
    var k = event.keyCode;
    var inBox = document.activeElement === search;
    if (inBox) {
      if (event.key === 'ArrowDown' || event.key === 'Tab' || k === 40 || k === 9) {
        event.preventDefault();
        leaveBox();
      } else if (event.key === 'Enter' || k === 13) {
        event.preventDefault();  // Enter in the box tracks the top result
        if (visible.length) { selKey = keyOf(visible[0]); toggleTrack(visible[0]); }
      }
      return;
    }
    if (isJournalKey(event)) {
      event.preventDefault();
      send('close');
      return;
    }
    if (single === 'history') {  // 0.17.0: no rows to select; the arrows scroll the list
      var step = { 40: 60, 38: -60, 34: scroll.clientHeight, 33: -scroll.clientHeight }[k];
      if (step) { event.preventDefault(); scroll.scrollTop += step; return; }
      if (event.key === 'Home' || k === 36) { event.preventDefault(); scroll.scrollTop = 0; return; }
      if (event.key === 'End' || k === 35) { event.preventDefault(); scroll.scrollTop = scroll.scrollHeight; return; }  // 0.17.1
      if (event.key === 'Tab' || k === 9) { event.preventDefault(); backToBox(); return; }
      if (event.key && event.key.length === 1 && !event.ctrlKey && !event.altKey) backToBox();
      return;
    }
    var at = selIndex();
    var handled = true;
    if (event.key === 'ArrowDown' || k === 40) select(at + 1);
    else if (event.key === 'ArrowUp' || k === 38) { if (at <= 0) backToBox(); else select(at - 1); }
    else if (event.key === 'PageDown' || k === 34) select(at + PAGE);
    else if (event.key === 'PageUp' || k === 33) select(Math.max(0, at - PAGE));
    else if (event.key === 'Home' || k === 36) select(0);
    else if (event.key === 'End' || k === 35) select(visible.length - 1);
    else if (event.key === 'Enter' || k === 13 || event.key === ' ' || k === 32) { if (at >= 0) toggleTrack(visible[at]); }
    else if (event.key === 'Tab' || k === 9) backToBox();
    else if ((event.key === 'm' || event.key === 'M' || k === 77) && !event.ctrlKey && !event.altKey) showOnMap(at >= 0 ? visible[at] : focusedQuest());
    else if ((event.key === 'p' || event.key === 'P' || k === 80) && !event.ctrlKey && !event.altKey) togglePin(at >= 0 ? visible[at] : focusedQuest());
    else if ((event.key === 'h' || event.key === 'H' || k === 72) && !event.ctrlKey && !event.altKey) toggleHide(at >= 0 ? visible[at] : focusedQuest());
    else handled = false;
    if (handled) {
      event.preventDefault();
      return;
    }
    if (event.key && event.key.length === 1 && !event.ctrlKey && !event.altKey) {
      backToBox();  // typing anywhere else goes back into the search box
    }
  });

  // ======== 0.11.0 notification boxes: since 0.16.0 in boxes.js ========
  // The boxes pause (and new ones wait) while this panel is open.
  function applyPause() {
    if (typeof window.qsoBoxesPanel === 'function') window.qsoBoxesPanel(!panel.hidden);
  }

  // ======== 0.11.0 Settings page ========
  // Every change is sent at once (the plugin applies it and saves it in My Games); the INI stays as it is.
  var settingsEl = document.getElementById('settings');
  var gearButton = document.getElementById('gear');
  var titleEl = document.getElementById('title');
  var settingsOpen = false;
  var settingsValues = {};
  var captureHooked = false;  // 0.16.0: the game-notification hook went in at startup (it can only go in then)
  var b612Present = false;    // 0.16.1: B612 NotificationManager is installed (the plugin checks at startup)
  var savedTimer = 0;
  var POSITIONS = ['Top left', 'Top right', 'Middle left', 'Middle right', 'Bottom left', 'Bottom right'];
  var STEPS = {
    iBoxScale: { min: 50, max: 200, step: 10, unit: ' %' },
    iBoxSeconds: { min: 1, max: 60, step: 1, unit: ' s' },
    iLineSeconds: { min: 1, max: 60, step: 1, unit: ' s' },
    iMaxBoxes: { min: 1, max: 10, step: 1, unit: '' },
    iMinBoxSeconds: { min: 1, max: 30, step: 1, unit: ' s' },
    iNearbyRadius: { min: 50, max: 5000, step: 50, unit: ' m' },
    iNearbyCooldown: { min: 0, max: 3600, step: 15, unit: ' s' }
  };

  function sv(key, fallback) { var v = Number(settingsValues[key]); return isNaN(v) ? fallback : v; }

  function setSetting(key, value) {
    settingsValues[key] = value;
    send('settings:' + key + '=' + value);
    if (key === 'bGroupHeadings') { sortQuests(); render(true); }
    renderSettings();
    var saved = document.getElementById('s-saved');
    if (saved) {
      saved.textContent = 'Saved.';
      if (savedTimer) clearTimeout(savedTimer);
      savedTimer = setTimeout(function () { saved.textContent = ''; }, 1600);
    }
  }

  function button(text, cls, onClick, title) {
    var b = textEl('button', 'chip ' + (cls || ''), text);
    b.type = 'button';
    if (title) b.title = title;
    b.addEventListener('mousedown', function (event) { event.preventDefault(); });
    b.addEventListener('click', onClick);
    return b;
  }

  function row(label, control, hint) {
    var r = textEl('div', 's-row', '');
    r.appendChild(textEl('span', 's-label', label));
    r.appendChild(control);
    if (hint) r.appendChild(textEl('span', 's-hint', hint));
    return r;
  }

  function stepper(key, fallback) {
    var c = STEPS[key];
    var v = sv(key, fallback);
    var box = textEl('span', 's-step', '');
    box.appendChild(button('−', 'action', function () { setSetting(key, Math.max(c.min, v - c.step)); }));
    box.appendChild(textEl('span', 's-value', (key === 'iNearbyCooldown' && v === 0 ? 'off' : v + c.unit)));
    box.appendChild(button('+', 'action', function () { setSetting(key, Math.min(c.max, v + c.step)); }));
    return box;
  }

  function toggle(key, label, title, fallback) {
    var on = sv(key, fallback === undefined ? 1 : fallback) !== 0;
    return button(label, on ? 'on' : '', function () { setSetting(key, on ? 0 : 1); }, title);
  }

  function group(title) {
    var g = textEl('div', 's-group', '');
    g.appendChild(textEl('h2', '', title));
    settingsEl.appendChild(g);
    return g;
  }

  function renderSettings() {
    while (settingsEl.firstChild) settingsEl.removeChild(settingsEl.firstChild);
    var captureOn = sv('bCaptureEnabled', 0) !== 0;
    // 0.16.0: with game notifications on, quest messages always use boxes (the game's own line would be caught anyway).
    var boxesOn = sv('iBoxStyle', 1) !== 0 || captureOn;

    var look = group('Notifications');
    var style = textEl('span', 's-chips', '');
    if (captureOn) {
      var why = 'Game notifications are on: quest messages always use boxes';
      style.appendChild(button('Quest boxes', 'on off-limits', function () {}, why));
      style.appendChild(button("The game's own text (top left)", 'off-limits', function () {}, why));
      look.appendChild(row('Style', style, 'Greyed out: game notifications are on (below), so quest messages always use boxes.'));
    } else {
      style.appendChild(button('Quest boxes', boxesOn ? 'on' : '', function () { setSetting('iBoxStyle', 1); }));
      style.appendChild(button("The game's own text (top left)", boxesOn ? '' : 'on', function () { setSetting('iBoxStyle', 0); }));
      look.appendChild(row('Style', style));
    }
    var screen = textEl('div', 's-screen', '');
    screen.appendChild(textEl('span', 's-mid', 'screen'));
    var pos = sv('iBoxPosition', 1);
    POSITIONS.forEach(function (name, i) {
      screen.appendChild(button(name, 'p' + i + (i === pos ? ' on' : ''), function () { setSetting('iBoxPosition', i); }));
    });
    look.appendChild(row('Position', screen, boxesOn ? 'Boxes slide in from that side and read oldest to newest, top to bottom. Game notifications use the same spot.' : 'Only for the boxes.'));
    look.appendChild(row('Size', stepper('iBoxScale', 100)));
    look.appendChild(row('Each quest box stays', stepper('iBoxSeconds', 8), 'Its clock runs only while it is on screen.'));
    look.appendChild(row('Most boxes at once', stepper('iMaxBoxes', 5), 'More wait their turn: then the oldest leaves early, one a second. None is dropped.'));
    look.appendChild(row('Min time on screen', stepper('iMinBoxSeconds', 3), 'How long a box stays before a waiting one may push it off. Long boxes get a little more by themselves (at most 30 s). Missed one? The Notifications tab lists them all.'));

    var game = group('Game notifications');
    var gameChips = textEl('span', 's-chips', '');
    gameChips.appendChild(toggle('bCaptureEnabled', "Show the game's notifications as boxes",
      "The game's white top-left lines (items added, saving, other mods' messages) become boxes in the same stack", 0));
    var gameHint = captureOn && captureHooked ? "On: the game's lines are boxes, in the same stack as the quest boxes." :
      captureOn ? 'Takes effect after you restart the game.' :
      captureHooked ? 'Off: the game shows its own lines again, from now on. (The hook stays in, doing nothing, until you restart.)' :
      "Off: the game shows its own white lines.";
    var gameRow = row('Notifications', gameChips, gameHint);
    if (captureOn && !captureHooked) gameRow.lastChild.className = 's-hint s-note';
    game.appendChild(gameRow);
    game.appendChild(row('Each game line stays', stepper('iLineSeconds', 6), 'Position, size and the most boxes at once are shared with the quest boxes above.' +
      (b612Present ? " Lines that B612 handles (the Feeding Perk) stay the game's own text." : '')));

    var listGroup = group('Quest list');
    var listChips = textEl('span', 's-chips', '');
    listChips.appendChild(toggle('bGroupHeadings', 'Group by quest type', 'Headings for Main, Civil War, Daedric, Side, Misc...; click one to fold it'));
    listGroup.appendChild(row('Headings', listChips, sv('bGroupHeadings', 1) !== 0 ?
      'Rows leave out their type badge: the heading says it. Pinned quests always come first.' :
      'One flat list; pinned quests still come first.'));

    var keyGroup = group('Journal key');
    var keyChips = textEl('span', 's-chips', '');
    keyChips.appendChild(toggle('bOpenOnMap', 'Open on the world map too', 'On the world map the Journal key opens this list instead of the game\'s journal; Show on map then moves the open map'));
    keyGroup.appendChild(row('World map', keyChips, sv('bOpenOnMap', 1) !== 0 ? 'Takes effect at the next key press.' : 'On the map the key opens the game\'s own journal.'));

    var which = group('Which messages');
    var chipsRow = textEl('span', 's-chips', '');
    chipsRow.appendChild(toggle('bNotifyArrive', 'Arriving in a town', 'Quests with work in the town, city or dungeon you walk into'));
    chipsRow.appendChild(toggle('bNotifyLeave', 'Leaving a town', 'Quests still open in the place you leave'));
    chipsRow.appendChild(toggle('bNotifyNearby', 'Nearby (distance)', 'Outdoors, an open objective within the distance below'));
    chipsRow.appendChild(toggle('bNotifyUpdates', 'Quest updates here', 'A quest updates and its new objective is in this town or within the distance'));
    chipsRow.appendChild(toggle('bNearbyIncludeMisc', 'Include Misc quests', 'Radiant favours and other Misc quests count too'));
    which.appendChild(row('Show', chipsRow));
    which.appendChild(row('Nearby means within', stepper('iNearbyRadius', 300), 'One map cell is about 58 m.'));
    which.appendChild(row('Same town message again after', stepper('iNearbyCooldown', 90), 'Stops repeats when you step out and back in.'));

    var actions = textEl('div', 's-actions', '');
    actions.appendChild(button('Show test notifications', 'action', function () { send('toasttest'); },
      'Closes this and shows three sample boxes in the game, as they will look'));
    actions.appendChild(button('Reset to the INI', 'action', function () { send('settings:reset'); },
      'Forget the choices made here and use QuestSearchOverlay.ini again (game notifications go back to off unless the INI says on)'));
    actions.appendChild(button('Back to quests', 'action', function () { showSettings(false); search.focus(); }));
    actions.appendChild(textEl('span', '', ''));
    var saved = textEl('span', '', '');
    saved.id = 's-saved';
    actions.appendChild(saved);
    settingsEl.appendChild(actions);
    settingsEl.appendChild(textEl('p', 's-hint', 'Quest Search and Notification Overlay. Saved in My Games\\Skyrim Special Edition\\SKSE\\QuestSearchOverlay_Settings.ini, for every profile and character. Esc: back to the quests.'));
  }

  function showSettings(on) {
    settingsOpen = !!on;
    settingsEl.hidden = !settingsOpen;
    panel.classList.toggle('settings-open', settingsOpen);
    titleEl.textContent = settingsOpen ? 'Settings' : 'Quests';
    gearButton.textContent = settingsOpen ? 'Quests' : 'Settings';
    if (settingsOpen) { search.blur(); renderSettings(); }
  }

  window.qsoSettings = function (json) {
    try { settingsValues = JSON.parse(json || '{}'); } catch (e) { return; }
    if (settingsOpen) renderSettings();
    var saved = document.getElementById('s-saved');
    if (saved) saved.textContent = 'Back to the INI values.';
  };

  gearButton.addEventListener('mousedown', function (event) { event.preventDefault(); });
  gearButton.addEventListener('click', function () {
    showSettings(!settingsOpen);
    if (!settingsOpen) search.focus();
  });
})();
