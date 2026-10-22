// The co-op multiplayer event log: an ActionScript 2 panel for the game's
// Scaleform GFx 3, built into ui/coop_eventlog.swf by tools/movie.py and added
// as a HUD screen by src/EventLogUi.cpp (dao::gui::AddScreen).
//
// The plugin fills _root.lines (an array of strings), _root.count and
// _root.revision with Movie::SetArray / SetVariable; this script redraws only
// when the revision changes. Outside the game (a Flash player such as Ruffle)
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

var panelW = 440;   // screen pixels
var lineH = 23;
var titleH = 34;
var pad = 14;
var maxLines = 8;

var lastRevision = -1;
var lastViewW = 0;
var lastViewH = 0;
var frameCounter = 0;

var titleField;
var lineFields = [];
var panel;

function stageUnitsFor(width, height) {
    // One screen pixel in stage units (Stage.scaleMode "showAll").
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

function sampleLines() {
    return [
        "Co-op Event Log initialized.",
        "Waiting for the other player...",
        "Alex joined the game.",
        "Combat started."
    ];
}

function lineAt(index) {
    var count = Number(_root.count);
    if (isNaN(count) || count <= 0) {
        var sample = sampleLines();
        return index < sample.length ? sample[index] : "";
    }
    var lines = _root.lines;
    if (lines == undefined || lines[index] == undefined) {
        return "";
    }
    return String(lines[index]);
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
    return field;
}

function draw() {
    var viewW = viewWidth();
    var viewH = viewHeight();
    var unit = stageUnitsFor(viewW, viewH);

    var rows = 0;
    for (var i = 0; i < maxLines; i++) {
        if (lineAt(i) != "") {
            rows++;
        }
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
        panel.hitTestDisable = true; // decorative: never steal a world click
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
        titleField.text = "EVENT LOG";
    }
    titleField._x = left + pad * unit;
    titleField._y = top + pad * unit;
    titleField._width = width - pad * 2 * unit;
    titleField._height = titleH * unit;
    titleField.setTextFormat(titleField.getNewTextFormat());

    for (var row = 0; row < maxLines; row++) {
        if (lineFields[row] == undefined) {
            lineFields[row] = makeField("coopLine" + row, 20 + row, 14 * unit, false);
        }
        var field = lineFields[row];
        field.text = lineAt(row);
        field.setTextFormat(field.getNewTextFormat());
        field._x = left + pad * unit;
        field._y = (top + (titleH + pad) * unit) + row * lineH * unit;
        field._width = (panelW - pad * 2) * unit;
        field._height = lineH * unit;
        field._visible = (lineAt(row) != "");
    }

    lastViewW = viewW;
    lastViewH = viewH;
}

onEnterFrame = function () {
    frameCounter++;
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

