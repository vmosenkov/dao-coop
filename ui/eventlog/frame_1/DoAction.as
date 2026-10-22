// The co-op multiplayer event log: an ActionScript 2 panel for the game's
// Scaleform GFx 3, built into ui/coop_eventlog.swf by tools/movie.py and added
// as a HUD screen by src/EventLogUi.cpp (dao::gui::AddScreen).
//
// The plugin fills _root.lines (an array of strings), _root.categories (their
// category: 0 multiplayer, 1 world, 2 combat, 3 system), _root.count and
// _root.revision with Movie::SetArray / SetVariable; this script redraws only
// when the revision changes. Tabs filter the list; a click on the panel folds
// it to its latest line. Outside the game (a Flash player such as Ruffle)
// there is no plugin, so it shows sample lines.
//
// The panel is laid out in screen pixels and converted to stage units: the
// game shows its own interface at its pixel size at every resolution while
// this movie's stage is scaled to the view (the same problem DAOTurnBased
// solved in its DoAction.as).

var STAGE_W = 1280;
var STAGE_H = 720;

// The movie's stage is scaled to the view; the layout below converts pixels.
Stage.scaleMode = "showAll";

var inGame = false;
try {
    inGame = flash.external.ExternalInterface.available;
} catch (error) {
    inGame = false;
}

var panelW = 520;   // screen pixels
var lineH = 22;
var titleH = 46;    // title + the filtered tabs
var pad = 14;
var maxLines = 8;
var tabW = 84;
var tabH = 20;

var lastRevision = -1;
var lastViewW = 0;
var lastViewH = 0;

var titleField;
var lineFields = [];
var panel;
var tabs = [];
var collapsed = false; // a click on the panel folds it to its latest line
var filterMode = 0;    // 0 all, 1 combat, 2 multiplayer, 3 system
var tabNames = ["ALL", "COMBAT", "MULTI", "SYSTEM"];
var tabCategory = [-1, 2, 0, 3]; // GameEvent category: 0 multiplayer, 1 world, 2 combat, 3 system

function stageUnitsFor(width, height) {
    var scale = Math.min(width / STAGE_W, height / STAGE_H);
    if (!(scale > 0)) {
        scale = 1;
    }
    return 1 / scale;
}

function viewWidth() {
    var w = Number(_root.viewWidth);
    if (!(w > 100)) {
        w = Stage.width;
    }
    if (!(w > 100)) {
        w = STAGE_W;
    }
    return w;
}

function viewHeight() {
    var h = Number(_root.viewHeight);
    if (!(h > 100)) {
        h = Stage.height;
    }
    if (!(h > 100)) {
        h = STAGE_H;
    }
    return h;
}

function lineCount() {
    var count = Number(_root.count);
    return isNaN(count) ? 0 : count;
}

function rawLine(index) {
    var lines = _root.lines;
    if (lines == undefined || lines[index] == undefined) {
        return "";
    }
    return String(lines[index]);
}

function categoryAt(index) {
    var categories = _root.categories;
    if (categories == undefined || categories[index] == undefined) {
        return 0;
    }
    return Number(categories[index]);
}

function passesFilter(index) {
    var wanted = tabCategory[filterMode];
    return wanted < 0 || categoryAt(index) == wanted;
}

function sampleLines() {
    return [
        "Co-op Event Log initialized.",
        "Waiting for the other player...",
        "Aedan joined the game.",
        "Combat started.",
        "Aedan attacks the ogre.",
        "The enemy attacks Aedan.",
        "The ogre died."
    ];
}

function shownLines() {
    var count = lineCount();
    if (count <= 0) {
        return sampleLines();
    }
    var out = [];
    for (var i = 0; i < count; i++) {
        if (passesFilter(i)) {
            out.push(rawLine(i));
        }
    }
    return out;
}

function makeField(name, depth, sizePx, bold) {
    var field = _root.createTextField(name, depth, 0, 0, 10, 10);
    field.embedFonts = inGame;
    var format = new TextFormat();
    format.font = inGame ? (bold ? "$TitleFont" : "$BodyFont") : "Georgia";
    format.size = sizePx;
    format.color = 0xE8DCC0;
    format.align = "left";
    field.setNewTextFormat(format);
    field.selectable = false;
    field.multiline = true;
    field.wordWrap = true;
    field.hitTestDisable = true;
    return field;
}

function makeTab(index, unit) {
    var clip = _root.createEmptyMovieClip("coopTab" + index, 30 + index);
    clip.hitTestDisable = false;
    var label = makeField("coopTabText" + index, 0, 11 * unit, false);
    label.text = tabNames[index];
    clip.label = label;
    clip.index = index;
    clip.onPress = function () {
        filterMode = this.index;
        lastViewW = -1;
        draw();
    };
    tabs[index] = clip;
    return clip;
}

function draw() {
    var viewW = viewWidth();
    var viewH = viewHeight();
    var unit = stageUnitsFor(viewW, viewH);

    var lines = shownLines();
    var rows = lines.length;
    if (collapsed) {
        rows = 1;
    }
    if (rows < 1) {
        rows = 1;
    }

    var panelPixelH = titleH + rows * lineH + pad * 2;
    var leftPx = viewW - 20 - panelW;
    var topPx = viewH - 24 - panelPixelH;
    if (leftPx < 0) { leftPx = 0; }
    if (topPx < 0) { topPx = 0; }

    var left = leftPx * unit;
    var top = topPx * unit;
    var width = panelW * unit;
    var height = panelPixelH * unit;

    if (panel == undefined) {
        panel = _root.createEmptyMovieClip("coopPanel", 5);
        panel.onPress = function () {
            collapsed = !collapsed;
            lastViewW = -1; // force the next frame to redraw
            draw();
        };
    }
    panel.clear();
    panel.beginFill(0x0B0B0B, 72);
    panel.lineStyle(1, 0xC9A86A, 80);
    panel.moveTo(left, top);
    panel.lineTo(left + width, top);
    panel.lineTo(left + width, top + height);
    panel.lineTo(left, top + height);
    panel.lineTo(left, top);
    panel.endFill();

    if (titleField == undefined) {
        titleField = makeField("coopTitle", 10, 17 * unit, true);
    }
    titleField.text = "EVENT LOG";
    titleField._x = left + pad * unit;
    titleField._y = top + pad * unit;
    titleField._width = (tabW * 2) * unit;
    titleField._height = 22 * unit;
    titleField.setTextFormat(titleField.getNewTextFormat());

    for (var tab = 0; tab < tabNames.length; tab++) {
        if (tabs[tab] == undefined) {
            makeTab(tab, unit);
        }
        var tabClip = tabs[tab];
        var tabLeft = left + (panelW - pad - tabW * tabNames.length) * unit + tab * tabW * unit;
        var tabTop = top + (pad + 2) * unit;
        tabClip.clear();
        tabClip.beginFill(tab == filterMode ? 0xC9A86A : 0x2A2A2A, tab == filterMode ? 85 : 70);
        tabClip.lineStyle(1, 0xC9A86A, 45);
        tabClip.moveTo(tabLeft, tabTop);
        tabClip.lineTo(tabLeft + tabW * unit, tabTop);
        tabClip.lineTo(tabLeft + tabW * unit, tabTop + tabH * unit);
        tabClip.lineTo(tabLeft, tabTop + tabH * unit);
        tabClip.lineTo(tabLeft, tabTop);
        tabClip.endFill();
        tabClip.label._x = tabLeft + 5 * unit;
        tabClip.label._y = tabTop + 3 * unit;
        tabClip.label._width = tabW * unit;
        tabClip.label._height = tabH * unit;
        tabClip.label.setTextFormat(tabClip.label.getNewTextFormat());
    }

    var shown = 0;
    var start = lines.length > rows ? lines.length - rows : 0;
    for (var row = 0; row < rows; row++) {
        if (lineFields[row] == undefined) {
            lineFields[row] = makeField("coopLine" + row, 20 + row, 14 * unit, false);
        }
        var field = lineFields[row];
        var text = "";
        if (collapsed) {
            text = lines.length > 0 ? lines[lines.length - 1] : "";
        } else if (start + row < lines.length) {
            text = lines[start + row];
        }
        field.text = text;
        field.setTextFormat(field.getNewTextFormat());
        field._x = left + pad * unit;
        field._y = (top + (titleH + pad) * unit) + row * lineH * unit;
        field._width = (panelW - pad * 2) * unit;
        field._height = lineH * unit;
        field._visible = (text != "");
        if (text != "") {
            shown++;
        }
    }
    for (var extra = rows; extra < maxLines; extra++) {
        if (lineFields[extra] != undefined) {
            lineFields[extra]._visible = false;
        }
    }

    lastViewW = viewW;
    lastViewH = viewH;
}

onEnterFrame = function () {
    var revision = Number(_root.revision);
    if (isNaN(revision)) {
        revision = 0;
    }
    var viewW = viewWidth();
    var viewH = viewHeight();
    if (revision != lastRevision || viewW != lastViewW || viewH != lastViewH) {
        lastRevision = revision;
        draw();
    }
};

draw();
