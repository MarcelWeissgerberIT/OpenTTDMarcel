/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file pixelstudio_gui.cpp Pixel-Studio (Fork-Feature): Fahrzeuggrafiken
 * direkt im Spiel bearbeiten.
 *
 * Der Spieler waehlt ein Standard-Strassenfahrzeug, malt auf einem
 * vergroesserten Pixelraster (Stift, Fuellen, Pipette, Radierer,
 * 256-Farben-Palette, Rueckgaengig) und speichert. Die Aenderung ersetzt
 * das Sprite zur Laufzeit (alle Zoomstufen werden neu berechnet) und
 * wird in pixelstudio.dat im Nutzerverzeichnis dauerhaft abgelegt.
 */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "zoom_func.h"
#include "strings_func.h"
#include "engine_base.h"
#include "roadveh.h"
#include "spritecache.h"
#include "fileio_func.h"
#include "core/geometry_func.hpp"

#include "widgets/pixelstudio_widget.h"

#include "table/strings.h"
#include "table/sprites.h"

#include <fstream>

#ifdef __EMSCRIPTEN__
#	include <emscripten.h>
#endif

#include "safeguards.h"

/** Leinwandgroesse (unskaliert); die Zellgroesse passt sich dem Sprite an. */
static const uint PS_CANVAS_W = 400;
static const uint PS_CANVAS_H = 300;
static const uint PS_PAL_CELL = 10; ///< Zellgroesse der Palette (unskaliert).
static const uint PS_VIEWS = 8;     ///< Blickrichtungen je Fahrzeug.

/** Malwerkzeuge. */
enum class PsTool : uint8_t {
	Pencil, ///< Einzelne Pixel setzen.
	Fill,   ///< Zusammenhaengende Flaeche fuellen.
	Pick,   ///< Farbe aus dem Bild aufnehmen.
	Erase,  ///< Pixel transparent machen.
};

/** Ein bearbeitbares Fahrzeug in der Liste. */
struct PsEntry {
	EngineID engine;
	SpriteID base; ///< Sprite der ersten Blickrichtung.
};

/* ---------- Dauerhafte Ablage (pixelstudio.dat im Nutzerverzeichnis) ---------- */

static std::string PixelStudioFilePath()
{
	return _personal_dir + "pixelstudio.dat";
}

/** Alle Overrides der Strassenfahrzeuge in die Datei schreiben. */
static void PixelStudioSaveToDisk()
{
	std::ofstream f(PixelStudioFilePath(), std::ios::binary | std::ios::trunc);
	if (!f.is_open()) return;
	f.write("PXS1", 4);
	for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
		SpriteID base = GetRoadVehBaseSprite(e->index);
		if (base == 0) continue;
		for (uint v = 0; v < PS_VIEWS; v++) {
			if (!PixelStudioHasOverride(base + v)) continue;
			PixelStudioSprite ps;
			if (!PixelStudioReadSprite(base + v, ps)) continue;
			uint16_t engine_id = e->index.base();
			uint8_t view = static_cast<uint8_t>(v);
			f.write(reinterpret_cast<const char *>(&engine_id), 2);
			f.write(reinterpret_cast<const char *>(&view), 1);
			f.write(reinterpret_cast<const char *>(&ps.width), 2);
			f.write(reinterpret_cast<const char *>(&ps.height), 2);
			f.write(reinterpret_cast<const char *>(&ps.x_offs), 2);
			f.write(reinterpret_cast<const char *>(&ps.y_offs), 2);
			f.write(reinterpret_cast<const char *>(ps.pixels.data()), ps.pixels.size());
		}
	}
	f.close();
#ifdef __EMSCRIPTEN__
	EM_ASM(if (window["openttd_syncfs"]) openttd_syncfs());
#endif
}

/** Beim Start: gespeicherte Overrides einlesen und aktivieren. */
void PixelStudioLoadOverrides()
{
	static bool loaded = false;
	if (loaded) return;
	loaded = true;

	std::ifstream f(PixelStudioFilePath(), std::ios::binary);
	if (!f.is_open()) return;
	char magic[4];
	f.read(magic, 4);
	if (!f.good() || std::string_view(magic, 4) != "PXS1") return;

	for (;;) {
		uint16_t engine_id;
		uint8_t view;
		PixelStudioSprite ps;
		f.read(reinterpret_cast<char *>(&engine_id), 2);
		if (!f.good()) break;
		f.read(reinterpret_cast<char *>(&view), 1);
		f.read(reinterpret_cast<char *>(&ps.width), 2);
		f.read(reinterpret_cast<char *>(&ps.height), 2);
		f.read(reinterpret_cast<char *>(&ps.x_offs), 2);
		f.read(reinterpret_cast<char *>(&ps.y_offs), 2);
		if (!f.good() || ps.width == 0 || ps.width > 160 || ps.height == 0 || ps.height > 160 || view >= PS_VIEWS) break;
		ps.pixels.resize(static_cast<size_t>(ps.width) * ps.height);
		f.read(reinterpret_cast<char *>(ps.pixels.data()), ps.pixels.size());
		if (!f.good()) break;

		const Engine *e = Engine::GetIfValid(engine_id);
		if (e == nullptr || e->type != VehicleType::Road) continue;
		SpriteID base = GetRoadVehBaseSprite(e->index);
		if (base == 0) continue;
		PixelStudioSetOverride(base + view, std::move(ps));
	}
}

/* ---------- Das Editor-Fenster ---------- */

struct PixelStudioWindow : Window {
	std::vector<PsEntry> entries; ///< Bearbeitbare Fahrzeuge.
	int selected = -1;            ///< Index in #entries.
	uint view = 0;                ///< Blickrichtung 0..7.
	PixelStudioSprite cur;        ///< Aktueller Malpuffer.
	std::vector<PixelStudioSprite> undo; ///< Rueckgaengig-Stapel.
	uint8_t colour = 0xC4;        ///< Gewaehlter Palettenindex.
	PsTool tool = PsTool::Pencil;
	bool stroke_active = false;   ///< Laufender Malzug (fuer Undo-Gruppierung).
	Scrollbar *vscroll = nullptr;

	PixelStudioWindow(WindowDesc &desc, WindowNumber) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_PS_SCROLLBAR);
		this->FinishInitNested();

		for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
			SpriteID base = GetRoadVehBaseSprite(e->index);
			if (base == 0) continue;
			this->entries.push_back({e->index, base});
		}
		this->vscroll->SetCount(this->entries.size());
		if (!this->entries.empty()) this->SelectEntry(0);
	}

	SpriteID CurrentSprite() const
	{
		if (this->selected < 0) return 0;
		return this->entries[this->selected].base + this->view;
	}

	void SelectEntry(int index)
	{
		this->selected = index;
		this->view = 0;
		this->LoadView();
	}

	void LoadView()
	{
		this->undo.clear();
		this->cur = {};
		SpriteID s = this->CurrentSprite();
		if (s != 0) PixelStudioReadSprite(s, this->cur);
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_PS_ENGINE_LIST:
				size.width = std::max(size.width, static_cast<uint>(ScaleGUITrad(140)));
				size.height = std::max(size.height, static_cast<uint>(20 * GetCharacterHeight(FontSize::Normal)));
				resize.height = GetCharacterHeight(FontSize::Normal);
				break;
			case WID_PS_CANVAS:
				size = maxdim(size, Dimension(ScaleGUITrad(PS_CANVAS_W), ScaleGUITrad(PS_CANVAS_H)));
				break;
			case WID_PS_PALETTE:
				size = maxdim(size, Dimension(16 * ScaleGUITrad(PS_PAL_CELL), 16 * ScaleGUITrad(PS_PAL_CELL)));
				break;
			case WID_PS_PREVIEW:
				size = maxdim(size, Dimension(ScaleGUITrad(8 * 44), ScaleGUITrad(44)));
				break;
			default:
				break;
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_PS_VIEW_LABEL: return GetString(STR_PIXELSTUDIO_VIEW, this->view + 1, PS_VIEWS);
			case WID_PS_COLOUR: return GetString(STR_PIXELSTUDIO_COLOUR, this->colour);
			default: return this->Window::GetWidgetString(widget, stringid);
		}
	}

	/**
	 * Leinwand-Geometrie: Zellgroesse fuellt die Flaeche moeglichst aus
	 * (Zoom passt sich dem Sprite an), Sprite liegt mittig.
	 */
	void CanvasGeometry(int cw, int ch, int &cell, int &ox, int &oy) const
	{
		if (this->cur.width == 0 || this->cur.height == 0) {
			cell = ScaleGUITrad(16);
			ox = oy = 0;
			return;
		}
		cell = std::min(cw / (int)this->cur.width, ch / (int)this->cur.height);
		cell = std::min(cell, ScaleGUITrad(28));
		if (cell < 1) cell = 1;
		ox = (cw - (int)this->cur.width * cell) / 2;
		oy = (ch - (int)this->cur.height * cell) / 2;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_PS_ENGINE_LIST: {
				int line = GetCharacterHeight(FontSize::Normal);
				int y = r.top + WidgetDimensions::scaled.framerect.top;
				auto [first, last] = this->vscroll->GetVisibleRangeIterators(this->entries);
				for (auto it = first; it != last; ++it) {
					int index = static_cast<int>(it - this->entries.begin());
					if (index == this->selected) {
						GfxFillRect(r.left + 1, y, r.right - 1, y + line - 1, PC_BLACK);
					}
					DrawString(r.left + WidgetDimensions::scaled.frametext.left, r.right - WidgetDimensions::scaled.frametext.right, y,
							GetString(STR_ENGINE_NAME, it->engine), index == this->selected ? TextColour::White : TextColour::Black);
					y += line;
				}
				break;
			}

			case WID_PS_CANVAS: {
				GfxFillRect(r.left, r.top, r.right, r.bottom, PixelColour{GREY_SCALE(3)});
				if (this->cur.width == 0) break;
				int cell, ox, oy;
				this->CanvasGeometry(r.right - r.left + 1, r.bottom - r.top + 1, cell, ox, oy);
				for (uint y = 0; y < this->cur.height; y++) {
					for (uint x = 0; x < this->cur.width; x++) {
						int left = r.left + ox + x * cell;
						int top = r.top + oy + y * cell;
						uint8_t idx = this->cur.pixels[y * this->cur.width + x];
						if (idx == 0) {
							/* Transparenz als Schachbrett. */
							PixelColour c{GREY_SCALE(((x + y) & 1) != 0 ? 5 : 7)};
							GfxFillRect(left, top, left + cell - 1, top + cell - 1, c);
						} else {
							GfxFillRect(left, top, left + cell - 1, top + cell - 1, PixelColour{idx});
						}
					}
				}
				break;
			}

			case WID_PS_PALETTE: {
				int cell = ScaleGUITrad(PS_PAL_CELL);
				for (uint i = 0; i < 256; i++) {
					int left = r.left + (i % 16) * cell;
					int top = r.top + (i / 16) * cell;
					if (i == 0) {
						GfxFillRect(left, top, left + cell - 1, top + cell - 1, PixelColour{GREY_SCALE(5)});
						GfxFillRect(left, top, left + cell / 2 - 1, top + cell / 2 - 1, PixelColour{GREY_SCALE(7)});
						GfxFillRect(left + cell / 2, top + cell / 2, left + cell - 1, top + cell - 1, PixelColour{GREY_SCALE(7)});
					} else {
						GfxFillRect(left, top, left + cell - 1, top + cell - 1, PixelColour{static_cast<uint8_t>(i)});
					}
					if (i == this->colour) {
						GfxFillRect(left, top, left + cell - 1, top, PC_WHITE);
						GfxFillRect(left, top + cell - 1, left + cell - 1, top + cell - 1, PC_WHITE);
						GfxFillRect(left, top, left, top + cell - 1, PC_WHITE);
						GfxFillRect(left + cell - 1, top, left + cell - 1, top + cell - 1, PC_WHITE);
					}
				}
				break;
			}

			case WID_PS_PREVIEW: {
				GfxFillRect(r.left, r.top, r.right, r.bottom, PixelColour{GREY_SCALE(6)});
				if (this->selected < 0) break;
				/* Aktueller Malstand in Spielgroesse ... */
				int px = ScaleGUITrad(1);
				int cx = r.left + ScaleGUITrad(22);
				int cy = CentreBounds(r.top, r.bottom, 0);
				for (uint y = 0; y < this->cur.height; y++) {
					for (uint x = 0; x < this->cur.width; x++) {
						uint8_t idx = this->cur.pixels[y * this->cur.width + x];
						if (idx == 0) continue;
						int left = cx + (this->cur.x_offs + (int)x) * px;
						int top = cy + (this->cur.y_offs + (int)y) * px;
						GfxFillRect(left, top, left + px - 1, top + px - 1, PixelColour{idx});
					}
				}
				/* ... und daneben alle gespeicherten Blickrichtungen. */
				for (uint v = 0; v < PS_VIEWS; v++) {
					DrawSprite(this->entries[this->selected].base + v, PALETTE_RECOLOUR_START,
							r.left + ScaleGUITrad(66 + 44 * v), cy);
				}
				break;
			}

			default:
				break;
		}
	}

	void PushUndo()
	{
		this->undo.push_back(this->cur);
		if (this->undo.size() > 40) this->undo.erase(this->undo.begin());
	}

	/** Werkzeug an Sprite-Pixel (px, py) anwenden. */
	void ApplyTool(int px, int py)
	{
		if (px < 0 || py < 0 || px >= (int)this->cur.width || py >= (int)this->cur.height) return;
		uint8_t &p = this->cur.pixels[py * this->cur.width + px];
		switch (this->tool) {
			case PsTool::Pencil:
				p = this->colour;
				break;
			case PsTool::Erase:
				p = 0;
				break;
			case PsTool::Pick:
				this->colour = p;
				break;
			case PsTool::Fill: {
				uint8_t from = p;
				if (from == this->colour) return;
				std::vector<std::pair<int, int>> stack = {{px, py}};
				while (!stack.empty()) {
					auto [x, y] = stack.back();
					stack.pop_back();
					if (x < 0 || y < 0 || x >= (int)this->cur.width || y >= (int)this->cur.height) continue;
					uint8_t &q = this->cur.pixels[y * this->cur.width + x];
					if (q != from) continue;
					q = this->colour;
					stack.push_back({x + 1, y});
					stack.push_back({x - 1, y});
					stack.push_back({x, y + 1});
					stack.push_back({x, y - 1});
				}
				break;
			}
		}
		this->SetDirty();
	}

	/** Fensterposition in Sprite-Pixel umrechnen; false wenn ausserhalb. */
	bool CanvasPixelAt(int wx, int wy, int &px, int &py) const
	{
		const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_PS_CANVAS);
		int cell, ox, oy;
		this->CanvasGeometry(nw->current_x, nw->current_y, cell, ox, oy);
		int rx = wx - (int)nw->pos_x - ox;
		int ry = wy - (int)nw->pos_y - oy;
		if (rx < 0 || ry < 0) return false;
		px = rx / cell;
		py = ry / cell;
		return px < (int)this->cur.width && py < (int)this->cur.height;
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_PS_ENGINE_LIST: {
				auto it = this->vscroll->GetScrolledItemFromWidget(this->entries, pt.y, this, WID_PS_ENGINE_LIST, WidgetDimensions::scaled.framerect.top);
				if (it != this->entries.end()) this->SelectEntry(static_cast<int>(it - this->entries.begin()));
				break;
			}

			case WID_PS_VIEW_PREV:
				this->view = (this->view + PS_VIEWS - 1) % PS_VIEWS;
				this->LoadView();
				break;

			case WID_PS_VIEW_NEXT:
				this->view = (this->view + 1) % PS_VIEWS;
				this->LoadView();
				break;

			case WID_PS_CANVAS: {
				int px, py;
				if (!this->CanvasPixelAt(pt.x, pt.y, px, py)) break;
				if (this->tool != PsTool::Pick && !this->stroke_active) {
					this->PushUndo();
					this->stroke_active = true;
				}
				this->ApplyTool(px, py);
				break;
			}

			case WID_PS_PALETTE: {
				const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_PS_PALETTE);
				int cell = ScaleGUITrad(PS_PAL_CELL);
				uint cx = (pt.x - nw->pos_x) / cell;
				uint cy = (pt.y - nw->pos_y) / cell;
				if (cx < 16 && cy < 16) {
					this->colour = static_cast<uint8_t>(cy * 16 + cx);
					if (this->colour == 0) this->tool = PsTool::Erase;
					this->SetDirty();
				}
				break;
			}

			case WID_PS_TOOL_PENCIL: this->tool = PsTool::Pencil; this->SetDirty(); break;
			case WID_PS_TOOL_FILL:   this->tool = PsTool::Fill;   this->SetDirty(); break;
			case WID_PS_TOOL_PICK:   this->tool = PsTool::Pick;   this->SetDirty(); break;
			case WID_PS_TOOL_ERASE:  this->tool = PsTool::Erase;  this->SetDirty(); break;

			case WID_PS_UNDO:
				if (!this->undo.empty()) {
					this->cur = this->undo.back();
					this->undo.pop_back();
					this->SetDirty();
				}
				break;

			case WID_PS_RESET: {
				SpriteID s = this->CurrentSprite();
				if (s != 0) {
					this->PushUndo();
					PixelStudioClearOverride(s);
					PixelStudioReadSprite(s, this->cur);
					PixelStudioSaveToDisk();
					MarkWholeScreenDirty();
				}
				break;
			}

			case WID_PS_SAVE: {
				SpriteID s = this->CurrentSprite();
				if (s != 0 && this->cur.width > 0) {
					PixelStudioSetOverride(s, this->cur);
					PixelStudioSaveToDisk();
					MarkWholeScreenDirty();
				}
				break;
			}

			default:
				break;
		}
	}

	void OnMouseLoop() override
	{
		if (!_left_button_down) {
			this->stroke_active = false;
			return;
		}
		/* Ziehen mit gedruecktem Stift/Radierer malt durchgehend. */
		if (this->tool != PsTool::Pencil && this->tool != PsTool::Erase) return;
		if (FindWindowFromPt(_cursor.pos.x, _cursor.pos.y) != this) return;
		int px, py;
		if (!this->CanvasPixelAt(_cursor.pos.x - this->left, _cursor.pos.y - this->top, px, py)) return;
		if (!this->stroke_active) {
			this->PushUndo();
			this->stroke_active = true;
		}
		this->ApplyTool(px, py);
	}

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_PS_TOOL_PENCIL, this->tool == PsTool::Pencil);
		this->SetWidgetLoweredState(WID_PS_TOOL_FILL, this->tool == PsTool::Fill);
		this->SetWidgetLoweredState(WID_PS_TOOL_PICK, this->tool == PsTool::Pick);
		this->SetWidgetLoweredState(WID_PS_TOOL_ERASE, this->tool == PsTool::Erase);
		this->DrawWidgets();
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_PS_ENGINE_LIST);
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_pixelstudio_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_PS_CAPTION), SetStringTip(STR_PIXELSTUDIO_CAPTION),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey),
		NWidget(NWID_HORIZONTAL), SetPIP(4, 4, 4), SetPadding(4, 4, 4, 4),
			/* Links: Fahrzeugliste */
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_INSET, Colours::Grey, WID_PS_ENGINE_LIST), SetToolTip(STR_PIXELSTUDIO_LIST_TOOLTIP), SetScrollbar(WID_PS_SCROLLBAR), SetFill(0, 1),
				EndContainer(),
				NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_PS_SCROLLBAR),
			EndContainer(),
			/* Mitte: Ansicht + Leinwand + Vorschau */
			NWidget(NWID_VERTICAL), SetPIP(0, 4, 0),
				NWidget(NWID_HORIZONTAL), SetPIP(0, 4, 0),
					NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_PS_VIEW_PREV), SetMinimalSize(24, 14), SetStringTip(STR_PIXELSTUDIO_PREV, STR_PIXELSTUDIO_VIEW_TOOLTIP),
					NWidget(WWT_TEXT, Colours::Invalid, WID_PS_VIEW_LABEL), SetFill(1, 0), SetAlignment({AlignmentH::Centre, AlignmentV::Middle}),
					NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_PS_VIEW_NEXT), SetMinimalSize(24, 14), SetStringTip(STR_PIXELSTUDIO_NEXT, STR_PIXELSTUDIO_VIEW_TOOLTIP),
				EndContainer(),
				NWidget(WWT_EMPTY, Colours::Invalid, WID_PS_CANVAS), SetToolTip(STR_PIXELSTUDIO_CANVAS_TOOLTIP),
				NWidget(WWT_EMPTY, Colours::Invalid, WID_PS_PREVIEW),
			EndContainer(),
			/* Rechts: Palette + Werkzeuge */
			NWidget(NWID_VERTICAL), SetPIP(0, 4, 0),
				NWidget(WWT_EMPTY, Colours::Invalid, WID_PS_PALETTE), SetToolTip(STR_PIXELSTUDIO_PALETTE_TOOLTIP),
				NWidget(WWT_TEXT, Colours::Invalid, WID_PS_COLOUR), SetFill(1, 0),
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_TOOL_PENCIL), SetStringTip(STR_PIXELSTUDIO_TOOL_PENCIL, STR_PIXELSTUDIO_TOOL_PENCIL_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_TOOL_FILL), SetStringTip(STR_PIXELSTUDIO_TOOL_FILL, STR_PIXELSTUDIO_TOOL_FILL_TOOLTIP), SetFill(1, 0),
				EndContainer(),
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_TOOL_PICK), SetStringTip(STR_PIXELSTUDIO_TOOL_PICK, STR_PIXELSTUDIO_TOOL_PICK_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_TOOL_ERASE), SetStringTip(STR_PIXELSTUDIO_TOOL_ERASE, STR_PIXELSTUDIO_TOOL_ERASE_TOOLTIP), SetFill(1, 0),
				EndContainer(),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_PS_UNDO), SetStringTip(STR_PIXELSTUDIO_UNDO, STR_PIXELSTUDIO_UNDO_TOOLTIP), SetFill(1, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_PS_RESET), SetStringTip(STR_PIXELSTUDIO_RESET, STR_PIXELSTUDIO_RESET_TOOLTIP), SetFill(1, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Green, WID_PS_SAVE), SetStringTip(STR_PIXELSTUDIO_SAVE, STR_PIXELSTUDIO_SAVE_TOOLTIP), SetFill(1, 0),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _pixelstudio_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::PixelStudio, WindowClass::None,
	{},
	_nested_pixelstudio_widgets
);

/** Pixel-Studio oeffnen (Fork-Feature). */
void ShowPixelStudioWindow()
{
	PixelStudioLoadOverrides();
	AllocateWindowDescFront<PixelStudioWindow>(_pixelstudio_desc, 0);
}
