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
#include "house.h"
#include "town.h"
#include "spritecache.h"
#include "fileio_func.h"
#include "core/geometry_func.hpp"
#include "core/backup_type.hpp"

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
	Select, ///< Rechteckigen Bereich markieren.
	Gradient, ///< Farbverlauf von Erst- zu Zweitfarbe ziehen.
};

/** Filterkategorie der Haeuser (hinter den vier Fahrzeugtypen). */
static const uint PS_CAT_HOUSE = 4;

/** Ein bearbeitbares Objekt in der Liste (Fahrzeug oder Gebaeude). */
struct PsEntry {
	bool is_house = false;
	bool is_flag = false; ///< Firmen-Fahne (festes Sprite).
	EngineID engine{};   ///< Fahrzeug (wenn !is_house).
	uint16_t house = 0;  ///< Basis-HouseID (wenn is_house).
	uint category = 0;   ///< Filterindex: 0-3 Fahrzeugtyp, 4 Haus.
	std::vector<SpriteID> views; ///< Eindeutige Sprites der Ansichten/Teile.
};

/* Standardsprite-Zugriffe je Fahrzeugtyp (0 = NewGRF-Grafik, nicht editierbar). */
SpriteID GetTrainDefaultSpritePS(EngineID engine, Direction direction);
SpriteID GetAircraftDefaultSpritePS(EngineID engine, Direction direction);
SpriteID GetShipDefaultSpritePS(EngineID engine, Direction direction);

/** Eindeutige Blickrichtungs-Sprites eines Fahrzeugs (leer = nicht editierbar). */
static std::vector<SpriteID> PsViewsForEngine(const Engine *e)
{
	std::vector<SpriteID> views;
	for (uint di = 0; di < 8; di++) {
		Direction d = static_cast<Direction>(di);
		SpriteID s = 0;
		switch (e->type) {
			case VehicleType::Train:    s = GetTrainDefaultSpritePS(e->index, d); break;
			case VehicleType::Road:     s = GetRoadVehBaseSprite(e->index); if (s != 0) s += to_underlying(d); break;
			case VehicleType::Ship:     s = GetShipDefaultSpritePS(e->index, d); break;
			case VehicleType::Aircraft: s = GetAircraftDefaultSpritePS(e->index, d); break;
			default: break;
		}
		if (s == 0) return {};
		if (std::find(views.begin(), views.end(), s) == views.end()) views.push_back(s);
	}
	return views;
}

/** Ist diese HouseID die Nordkachel (Basis) ihres Gebaeudes? */
static bool PsIsBaseHouse(HouseID id)
{
	HouseID copy = id;
	GetHouseNorthPart(copy);
	return copy == id;
}

/** Eindeutige Gebaeudesprites eines Hauses (alle Teile und Varianten, fertig gebaut). */
static std::vector<SpriteID> PsViewsForHouse(HouseID id)
{
	const HouseSpec *hs = HouseSpec::Get(id);
	if (!hs->enabled || hs->grf_prop.HasSpriteGroups()) return {};

	uint parts = 1;
	if (hs->building_flags.Test(BuildingFlag::Size2x2)) parts = 4;
	else if (hs->building_flags.Any(BUILDING_HAS_2_TILES)) parts = 2;

	auto data = GetTownDrawTileData();
	std::vector<SpriteID> views;
	for (uint p = 0; p < parts; p++) {
		for (uint v = 0; v < 4; v++) {
			size_t index = ((size_t)(id + p) << 4) | (v << 2) | TOWN_HOUSE_COMPLETED;
			if (index >= data.size()) return {};
			SpriteID s = data[index].building.sprite;
			if (s == 0) continue;
			if (std::find(views.begin(), views.end(), s) == views.end()) views.push_back(s);
		}
	}
	return views;
}

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
	f.write("PXS3", 4);
	auto write_entry = [&f](uint8_t kind, uint16_t id, uint8_t view, const PixelStudioSprite &ps) {
		f.write(reinterpret_cast<const char *>(&kind), 1);
		f.write(reinterpret_cast<const char *>(&id), 2);
		f.write(reinterpret_cast<const char *>(&view), 1);
		f.write(reinterpret_cast<const char *>(&ps.width), 2);
		f.write(reinterpret_cast<const char *>(&ps.height), 2);
		f.write(reinterpret_cast<const char *>(&ps.x_offs), 2);
		f.write(reinterpret_cast<const char *>(&ps.y_offs), 2);
		f.write(reinterpret_cast<const char *>(ps.pixels.data()), ps.pixels.size());
	};
	for (const Engine *e : Engine::Iterate()) {
		std::vector<SpriteID> views = PsViewsForEngine(e);
		for (uint v = 0; v < views.size(); v++) {
			if (!PixelStudioHasOverride(views[v])) continue;
			PixelStudioSprite ps;
			if (!PixelStudioReadSprite(views[v], ps)) continue;
			write_entry(0, e->index.base(), static_cast<uint8_t>(v), ps);
		}
	}
	for (size_t id = 0; id < HouseSpec::Specs().size(); id++) {
		if (!PsIsBaseHouse(static_cast<HouseID>(id))) continue;
		std::vector<SpriteID> views = PsViewsForHouse(static_cast<HouseID>(id));
		for (uint v = 0; v < views.size(); v++) {
			if (!PixelStudioHasOverride(views[v])) continue;
			PixelStudioSprite ps;
			if (!PixelStudioReadSprite(views[v], ps)) continue;
			write_entry(1, static_cast<uint16_t>(id), static_cast<uint8_t>(v), ps);
		}
	}
	if (PixelStudioHasOverride(SPR_COMPANY_FLAG)) {
		PixelStudioSprite ps;
		if (PixelStudioReadSprite(SPR_COMPANY_FLAG, ps)) write_entry(2, 0, 0, ps);
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
	if (!f.good()) return;
	std::string_view m(magic, 4);
	bool v1 = m == "PXS1"; /* nur Strassenfahrzeuge */
	bool v3 = m == "PXS3"; /* mit Kind-Byte (Fahrzeug/Haus) */
	if (!v1 && !v3 && m != "PXS2") return;

	for (;;) {
		uint8_t kind = 0;
		uint16_t engine_id;
		uint8_t view;
		PixelStudioSprite ps;
		if (v3) {
			f.read(reinterpret_cast<char *>(&kind), 1);
			if (!f.good()) break;
		}
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

		std::vector<SpriteID> views;
		if (kind == 2) {
			views = {SPR_COMPANY_FLAG};
		} else if (kind == 1) {
			if (engine_id >= HouseSpec::Specs().size()) continue;
			views = PsViewsForHouse(static_cast<HouseID>(engine_id));
		} else {
			const Engine *e = Engine::GetIfValid(engine_id);
			if (e == nullptr) continue;
			if (v1 && e->type != VehicleType::Road) continue;
			views = PsViewsForEngine(e);
		}
		if (view >= views.size()) continue;
		PixelStudioSetOverride(views[view], std::move(ps));
	}
}

/** Editierpuffer als RGBA-Bytes (fuer die Zwischenablage). */
static std::vector<uint8_t> PixelsToRGBA(const PixelStudioSprite &ps)
{
	std::vector<uint8_t> rgba(static_cast<size_t>(ps.width) * ps.height * 4);
	for (size_t i = 0; i < ps.pixels.size(); i++) {
		uint8_t idx = ps.pixels[i];
		Colour c = _cur_palette.palette[idx];
		rgba[i * 4 + 0] = c.r;
		rgba[i * 4 + 1] = c.g;
		rgba[i * 4 + 2] = c.b;
		rgba[i * 4 + 3] = idx == 0 ? 0 : 0xFF;
	}
	return rgba;
}

/** Naechster Palettenindex zu einer RGB-Farbe (ohne Firmenfarben-Rampe). */
static uint8_t NearestPaletteIndex(uint8_t r, uint8_t g, uint8_t b)
{
	uint best = 1;
	uint32_t best_d = UINT32_MAX;
	for (uint i = 1; i < 256; i++) {
		if (i >= 0xC6 && i <= 0xCD) continue; /* CC-Rampe nicht automatisch treffen */
		if (i >= PALETTE_ANIM_START && i < PALETTE_ANIM_START + PALETTE_ANIM_SIZE) continue; /* animierte Farben zappeln */
		Colour c = _cur_palette.palette[i];
		int dr = (int)c.r - r, dg = (int)c.g - g, db = (int)c.b - b;
		uint32_t d = dr * dr + dg * dg + db * db;
		if (d < best_d) { best_d = d; best = i; }
	}
	return static_cast<uint8_t>(best);
}

/* ---------- Das Editor-Fenster ---------- */

struct PixelStudioWindow : Window {
	std::vector<PsEntry> entries; ///< Bearbeitbare Fahrzeuge.
	std::vector<int> filtered;    ///< Sichtbare Indizes in #entries (Typ-Filter).
	bool show_type[5] = {true, true, true, true, true}; ///< Zug/Strasse/Schiff/Luft/Haus.
	int selected = -1;            ///< Index in #entries.
	uint view = 0;                ///< Blickrichtung 0..7.
	PixelStudioSprite cur;        ///< Aktueller Malpuffer.
	std::vector<PixelStudioSprite> undo; ///< Rueckgaengig-Stapel.
	uint8_t colour = 0xC4;        ///< Gewaehlter Palettenindex (Erstfarbe).
	uint8_t colour2 = 0x0F;       ///< Zweitfarbe (Ziel des Verlaufs).
	PsTool tool = PsTool::Pencil;
	bool stroke_active = false;   ///< Laufender Malzug (fuer Undo-Gruppierung).
	int zoom_cell = 0;            ///< Zellgroesse in Pixeln; 0 = automatisch einpassen.
	int pan_x = 0, pan_y = 0;     ///< Verschiebung des Ausschnitts (Leinwand-Pixel).
	bool panning = false;         ///< Rechtsklick-Ziehen laeuft.
	Point pan_last{};             ///< Letzte Mausposition beim Verschieben.
	bool has_sel = false;         ///< Auswahl aktiv.
	bool sel_dragging = false;    ///< Auswahl wird gerade aufgezogen.
	int sel_x0 = 0, sel_y0 = 0, sel_x1 = 0, sel_y1 = 0; ///< Auswahl (einschliesslich, normalisiert).
	int sel_ax = 0, sel_ay = 0;   ///< Anker beim Aufziehen.
	bool grad_active = false;     ///< Verlaufslinie wird gerade gezogen.
	Point grad_a{}, grad_b{};     ///< Start/Ende des Verlaufs (Sprite-Pixel).
	Scrollbar *vscroll = nullptr;

	PixelStudioWindow(WindowDesc &desc, WindowNumber) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_PS_SCROLLBAR);
		this->FinishInitNested();

		static const VehicleType order[] = {VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft};
		for (VehicleType vt : order) {
			for (const Engine *e : Engine::IterateType(vt)) {
				std::vector<SpriteID> views = PsViewsForEngine(e);
				if (views.empty()) continue;
				PsEntry en;
				en.engine = e->index;
				en.category = to_underlying(vt);
				en.views = std::move(views);
				this->entries.push_back(std::move(en));
			}
		}
		{
			/* Firmen-Fahne (weht auf gekauften Haeusern). */
			PsEntry en;
			en.is_flag = true;
			en.category = PS_CAT_HOUSE;
			en.views = {SPR_COMPANY_FLAG};
			this->entries.push_back(std::move(en));
		}
		for (size_t id = 0; id < HouseSpec::Specs().size(); id++) {
			if (!PsIsBaseHouse(static_cast<HouseID>(id))) continue;
			std::vector<SpriteID> views = PsViewsForHouse(static_cast<HouseID>(id));
			if (views.empty()) continue;
			PsEntry en;
			en.is_house = true;
			en.house = static_cast<uint16_t>(id);
			en.category = PS_CAT_HOUSE;
			en.views = std::move(views);
			this->entries.push_back(std::move(en));
		}
		this->RebuildFilter();
		if (!this->filtered.empty()) this->SelectEntry(this->filtered[0]);
	}

	void RebuildFilter()
	{
		this->filtered.clear();
		for (int i = 0; i < (int)this->entries.size(); i++) {
			if (this->show_type[this->entries[i].category]) this->filtered.push_back(i);
		}
		this->vscroll->SetCount(this->filtered.size());
		/* Auswahl unsichtbar geworden? Ersten sichtbaren Eintrag nehmen. */
		if (this->selected >= 0 && !this->show_type[this->entries[this->selected].category]) {
			if (!this->filtered.empty()) this->SelectEntry(this->filtered[0]);
		}
		this->SetDirty();
	}

	uint NumViews() const
	{
		if (this->selected < 0) return 1;
		return static_cast<uint>(this->entries[this->selected].views.size());
	}

	SpriteID CurrentSprite() const
	{
		if (this->selected < 0) return 0;
		const PsEntry &en = this->entries[this->selected];
		if (this->view >= en.views.size()) return 0;
		return en.views[this->view];
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
		/* Auswahl und Ausschnitt passen zum alten Sprite - zuruecksetzen. */
		this->has_sel = false;
		this->sel_dragging = false;
		this->grad_active = false;
		this->pan_x = this->pan_y = 0;
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
			case WID_PS_VIEW_LABEL: return GetString(STR_PIXELSTUDIO_VIEW, this->view + 1, this->NumViews());
			case WID_PS_COLOUR: return GetString(STR_PIXELSTUDIO_COLOUR, this->colour, this->colour2);
			default: return this->Window::GetWidgetString(widget, stringid);
		}
	}

	/**
	 * Leinwand-Geometrie: Zellgroesse fuellt die Flaeche moeglichst aus
	 * (oder manueller Zoom); passt das Bild nicht mehr hinein, bestimmt
	 * die Verschiebung (pan) den sichtbaren Ausschnitt.
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
		if (this->zoom_cell > 0) cell = this->zoom_cell;
		int iw = (int)this->cur.width * cell;
		int ih = (int)this->cur.height * cell;
		ox = iw <= cw ? (cw - iw) / 2 : -std::clamp(this->pan_x, 0, iw - cw);
		oy = ih <= ch ? (ch - ih) / 2 : -std::clamp(this->pan_y, 0, ih - ch);
	}

	/** Zoomstufe aendern; (cx, cy) ist der Leinwandpunkt, der stehen bleibt (-1 = Mitte). */
	void ChangeZoom(int dir, int cx = -1, int cy = -1)
	{
		if (this->cur.width == 0) return;
		const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_PS_CANVAS);
		int cell, ox, oy;
		this->CanvasGeometry(nw->current_x, nw->current_y, cell, ox, oy);
		int ncell = dir > 0 ? cell * 2 : cell / 2;
		ncell = std::clamp(ncell, 1, ScaleGUITrad(64));
		if (ncell == cell) return;
		if (cx < 0) { cx = nw->current_x / 2; cy = nw->current_y / 2; }
		/* Der Sprite-Punkt unter (cx, cy) soll nach dem Zoomen dort bleiben. */
		this->zoom_cell = ncell;
		this->pan_x = (cx - ox) * ncell / cell - cx;
		this->pan_y = (cy - oy) * ncell / cell - cy;
		this->SetDirty();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_PS_ENGINE_LIST: {
				int line = GetCharacterHeight(FontSize::Normal);
				int y = r.top + WidgetDimensions::scaled.framerect.top;
				auto [first, last] = this->vscroll->GetVisibleRangeIterators(this->filtered);
				for (auto it = first; it != last; ++it) {
					int index = *it;
					if (index == this->selected) {
						GfxFillRect(r.left + 1, y, r.right - 1, y + line - 1, PC_BLACK);
					}
					const PsEntry &en = this->entries[index];
					std::string name = en.is_flag ? GetString(STR_PIXELSTUDIO_FLAG)
							: en.is_house ? GetString(HouseSpec::Get(en.house)->building_name)
							: GetString(STR_ENGINE_NAME, en.engine);
					DrawString(r.left + WidgetDimensions::scaled.frametext.left, r.right - WidgetDimensions::scaled.frametext.right, y,
							name, index == this->selected ? TextColour::White : TextColour::Black);
					y += line;
				}
				break;
			}

			case WID_PS_CANVAS: {
				int cw = r.right - r.left + 1, ch = r.bottom - r.top + 1;
				/* Beim Hineinzoomen ragt das Bild ueber die Leinwand hinaus - clippen. */
				DrawPixelInfo tmp_dpi;
				if (!FillDrawPixelInfo(&tmp_dpi, r.left, r.top, cw, ch)) break;
				AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
				GfxFillRect(0, 0, cw - 1, ch - 1, PixelColour{GREY_SCALE(3)});
				if (this->cur.width == 0) break;
				int cell, ox, oy;
				this->CanvasGeometry(cw, ch, cell, ox, oy);
				uint x0 = static_cast<uint>(std::max(0, -ox / cell));
				uint y0 = static_cast<uint>(std::max(0, -oy / cell));
				uint x1 = std::min<uint>(this->cur.width, (cw - ox + cell - 1) / cell);
				uint y1 = std::min<uint>(this->cur.height, (ch - oy + cell - 1) / cell);
				for (uint y = y0; y < y1; y++) {
					for (uint x = x0; x < x1; x++) {
						int left = ox + x * cell;
						int top = oy + y * cell;
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
				/* Auswahlrahmen (weiss mit schwarzem Innenrand). */
				if (this->has_sel || this->sel_dragging) {
					int sl = ox + this->sel_x0 * cell;
					int st = oy + this->sel_y0 * cell;
					int sr = ox + (this->sel_x1 + 1) * cell - 1;
					int sb = oy + (this->sel_y1 + 1) * cell - 1;
					GfxDrawLine(sl, st, sr, st, PC_WHITE);
					GfxDrawLine(sl, sb, sr, sb, PC_WHITE);
					GfxDrawLine(sl, st, sl, sb, PC_WHITE);
					GfxDrawLine(sr, st, sr, sb, PC_WHITE);
					GfxDrawLine(sl + 1, st + 1, sr - 1, st + 1, PC_BLACK);
					GfxDrawLine(sl + 1, sb - 1, sr - 1, sb - 1, PC_BLACK);
					GfxDrawLine(sl + 1, st + 1, sl + 1, sb - 1, PC_BLACK);
					GfxDrawLine(sr - 1, st + 1, sr - 1, sb - 1, PC_BLACK);
				}
				/* Vorschau der Verlaufslinie waehrend des Ziehens. */
				if (this->grad_active) {
					int ax = ox + this->grad_a.x * cell + cell / 2;
					int ay = oy + this->grad_a.y * cell + cell / 2;
					int bx = ox + this->grad_b.x * cell + cell / 2;
					int by = oy + this->grad_b.y * cell + cell / 2;
					GfxDrawLine(ax, ay, bx, by, PC_WHITE, 3);
					GfxDrawLine(ax, ay, bx, by, PC_BLACK);
				}
				break;
			}

			case WID_PS_PALETTE: {
				int cell = ScaleGUITrad(PS_PAL_CELL);
				for (uint i = 0; i < 256; i++) {
					int left = r.left + (i % 16) * cell;
					int top = r.top + (i / 16) * cell;
					if (i >= PALETTE_ANIM_START && i < PALETTE_ANIM_START + PALETTE_ANIM_SIZE) {
						/* Animierte Spielfarben (Wasser/Feuer) zappeln nur -
						 * gesperrt und als dunkles Kreuzmuster gezeichnet. */
						GfxFillRect(left, top, left + cell - 1, top + cell - 1, PixelColour{GREY_SCALE(4)});
						GfxFillRect(left, top, left + cell - 1, top, PixelColour{GREY_SCALE(2)});
						GfxFillRect(left, top + cell - 1, left + cell - 1, top + cell - 1, PixelColour{GREY_SCALE(2)});
						continue;
					}
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
					} else if (i == this->colour2) {
						/* Zweitfarbe (Verlaufs-Ziel) mit schwarzem Rahmen. */
						GfxFillRect(left, top, left + cell - 1, top, PC_BLACK);
						GfxFillRect(left, top + cell - 1, left + cell - 1, top + cell - 1, PC_BLACK);
						GfxFillRect(left, top, left, top + cell - 1, PC_BLACK);
						GfxFillRect(left + cell - 1, top, left + cell - 1, top + cell - 1, PC_BLACK);
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
				const PsEntry &en = this->entries[this->selected];
				for (uint v = 0; v < en.views.size(); v++) {
					DrawSprite(en.views[v], PALETTE_RECOLOUR_START,
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
			case PsTool::Select:
			case PsTool::Gradient:
				/* Werden ueber eigenes Ziehen in OnClick/OnMouseLoop behandelt. */
				return;

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

	/** Wie #CanvasPixelAt, aber auf das Bild begrenzt (fuer Auswahl-/Verlaufsziehen). */
	void CanvasPixelClamped(int wx, int wy, int &px, int &py) const
	{
		const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_PS_CANVAS);
		int cell, ox, oy;
		this->CanvasGeometry(nw->current_x, nw->current_y, cell, ox, oy);
		int rx = wx - (int)nw->pos_x - ox;
		int ry = wy - (int)nw->pos_y - oy;
		px = std::clamp(rx < 0 ? -1 : rx / cell, 0, (int)this->cur.width - 1);
		py = std::clamp(ry < 0 ? -1 : ry / cell, 0, (int)this->cur.height - 1);
	}

	/** Verlauf von Erst- zu Zweitfarbe anwenden (nur bemalte Pixel, ggf. nur Auswahl). */
	void ApplyGradient()
	{
		if (this->grad_a.x == this->grad_b.x && this->grad_a.y == this->grad_b.y) return;
		this->PushUndo();
		int bx0 = this->has_sel ? this->sel_x0 : 0;
		int by0 = this->has_sel ? this->sel_y0 : 0;
		int bx1 = this->has_sel ? this->sel_x1 : (int)this->cur.width - 1;
		int by1 = this->has_sel ? this->sel_y1 : (int)this->cur.height - 1;
		double dx = this->grad_b.x - this->grad_a.x;
		double dy = this->grad_b.y - this->grad_a.y;
		double len2 = dx * dx + dy * dy;
		Colour ca = _cur_palette.palette[this->colour];
		Colour cb = _cur_palette.palette[this->colour2];
		for (int y = by0; y <= by1; y++) {
			for (int x = bx0; x <= bx1; x++) {
				uint8_t &p = this->cur.pixels[y * this->cur.width + x];
				if (p == 0) continue;
				double t = ((x - this->grad_a.x) * dx + (y - this->grad_a.y) * dy) / len2;
				t = std::clamp(t, 0.0, 1.0);
				uint8_t rr = static_cast<uint8_t>(ca.r + t * (cb.r - ca.r));
				uint8_t gg = static_cast<uint8_t>(ca.g + t * (cb.g - ca.g));
				uint8_t bb = static_cast<uint8_t>(ca.b + t * (cb.b - ca.b));
				p = NearestPaletteIndex(rr, gg, bb);
			}
		}
		this->SetDirty();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_PS_ENGINE_LIST: {
				auto it = this->vscroll->GetScrolledItemFromWidget(this->filtered, pt.y, this, WID_PS_ENGINE_LIST, WidgetDimensions::scaled.framerect.top);
				if (it != this->filtered.end()) this->SelectEntry(*it);
				break;
			}

			case WID_PS_FILTER_TRAIN:
			case WID_PS_FILTER_ROAD:
			case WID_PS_FILTER_SHIP:
			case WID_PS_FILTER_AIR:
			case WID_PS_FILTER_HOUSE: {
				uint idx = widget - WID_PS_FILTER_TRAIN;
				this->show_type[idx] = !this->show_type[idx];
				this->RebuildFilter();
				break;
			}

			case WID_PS_VIEW_PREV:
				this->view = (this->view + this->NumViews() - 1) % this->NumViews();
				this->LoadView();
				break;

			case WID_PS_VIEW_NEXT:
				this->view = (this->view + 1) % this->NumViews();
				this->LoadView();
				break;

			case WID_PS_ZOOM_IN:  this->ChangeZoom(+1); break;
			case WID_PS_ZOOM_OUT: this->ChangeZoom(-1); break;

			case WID_PS_CANVAS: {
				int px, py;
				if (this->tool == PsTool::Select) {
					if (!this->CanvasPixelAt(pt.x, pt.y, px, py)) {
						this->has_sel = false;
						this->SetDirty();
						break;
					}
					this->sel_dragging = true;
					this->sel_ax = px; this->sel_ay = py;
					this->sel_x0 = this->sel_x1 = px;
					this->sel_y0 = this->sel_y1 = py;
					this->SetDirty();
					break;
				}
				if (this->tool == PsTool::Gradient) {
					if (!this->CanvasPixelAt(pt.x, pt.y, px, py)) break;
					this->grad_active = true;
					this->grad_a = this->grad_b = {px, py};
					this->SetDirty();
					break;
				}
				if (!this->CanvasPixelAt(pt.x, pt.y, px, py)) break;
				if (this->tool == PsTool::Fill && this->has_sel
						&& px >= this->sel_x0 && px <= this->sel_x1 && py >= this->sel_y0 && py <= this->sel_y1) {
					/* Fuellen in einer Auswahl fuellt den ganzen markierten Bereich. */
					this->PushUndo();
					for (int y = this->sel_y0; y <= this->sel_y1; y++) {
						for (int x = this->sel_x0; x <= this->sel_x1; x++) {
							this->cur.pixels[y * this->cur.width + x] = this->colour;
						}
					}
					this->SetDirty();
					break;
				}
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
					uint8_t idx = static_cast<uint8_t>(cy * 16 + cx);
					/* Animierte Spielfarben sind nicht waehlbar. */
					if (idx >= PALETTE_ANIM_START && idx < PALETTE_ANIM_START + PALETTE_ANIM_SIZE) break;
					if (_ctrl_pressed) {
						/* Strg+Klick waehlt die Zweitfarbe (Verlaufs-Ziel). */
						this->colour2 = idx;
					} else {
						this->colour = idx;
						if (this->colour == 0) this->tool = PsTool::Erase;
					}
					this->SetDirty();
				}
				break;
			}

			case WID_PS_COPY:
#ifdef __EMSCRIPTEN__
				if (this->cur.width > 0) {
					std::vector<uint8_t> rgba = PixelsToRGBA(this->cur);
					EM_ASM({ if (window["openttd_ps_copy"]) openttd_ps_copy($0, $1, $2); },
							rgba.data(), this->cur.width, this->cur.height);
				}
#endif
				break;

			case WID_PS_PASTE:
#ifdef __EMSCRIPTEN__
				if (this->cur.width > 0) {
					EM_ASM({ if (window["openttd_ps_paste"]) openttd_ps_paste($0, $1); },
							this->cur.width, this->cur.height);
				}
#endif
				break;

			case WID_PS_TOOL_PENCIL: this->tool = PsTool::Pencil; this->SetDirty(); break;
			case WID_PS_TOOL_FILL:   this->tool = PsTool::Fill;   this->SetDirty(); break;
			case WID_PS_TOOL_PICK:   this->tool = PsTool::Pick;   this->SetDirty(); break;
			case WID_PS_TOOL_ERASE:  this->tool = PsTool::Erase;  this->SetDirty(); break;
			case WID_PS_TOOL_SELECT: this->tool = PsTool::Select; this->SetDirty(); break;
			case WID_PS_TOOL_GRADIENT: this->tool = PsTool::Gradient; this->SetDirty(); break;

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
		/* Rechtsklick-Ziehen verschiebt den Ausschnitt (wie die Kartenansicht). */
		if (_right_button_down && (this->panning || FindWindowFromPt(_cursor.pos.x, _cursor.pos.y) == this)) {
			if (this->panning) {
				this->pan_x -= _cursor.pos.x - this->pan_last.x;
				this->pan_y -= _cursor.pos.y - this->pan_last.y;
				/* Auf gueltigen Bereich begrenzen, damit Zoomwechsel sauber rechnen. */
				const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_PS_CANVAS);
				int cell, ox, oy;
				this->CanvasGeometry(nw->current_x, nw->current_y, cell, ox, oy);
				this->pan_x = std::clamp(this->pan_x, 0, std::max(0, (int)this->cur.width * cell - (int)nw->current_x));
				this->pan_y = std::clamp(this->pan_y, 0, std::max(0, (int)this->cur.height * cell - (int)nw->current_y));
				this->SetDirty();
			}
			this->panning = true;
			this->pan_last = _cursor.pos;
		} else {
			this->panning = false;
		}

		if (!_left_button_down) {
			this->stroke_active = false;
			if (this->sel_dragging) {
				this->sel_dragging = false;
				/* Ein blosser Klick (1x1) hebt die Auswahl auf. */
				this->has_sel = !(this->sel_x0 == this->sel_x1 && this->sel_y0 == this->sel_y1);
				this->SetDirty();
			}
			if (this->grad_active) {
				this->grad_active = false;
				this->ApplyGradient();
			}
			return;
		}

		if (this->sel_dragging || this->grad_active) {
			int px, py;
			this->CanvasPixelClamped(_cursor.pos.x - this->left, _cursor.pos.y - this->top, px, py);
			if (this->sel_dragging) {
				this->sel_x0 = std::min(this->sel_ax, px);
				this->sel_x1 = std::max(this->sel_ax, px);
				this->sel_y0 = std::min(this->sel_ay, py);
				this->sel_y1 = std::max(this->sel_ay, py);
			} else {
				this->grad_b = {px, py};
			}
			this->SetDirty();
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

	void OnMouseWheel(int wheel, WidgetID widget) override
	{
		if (widget != WID_PS_CANVAS) return;
		/* Zoom auf den Punkt unter dem Mauszeiger. */
		const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_PS_CANVAS);
		int cx = _cursor.pos.x - this->left - (int)nw->pos_x;
		int cy = _cursor.pos.y - this->top - (int)nw->pos_y;
		this->ChangeZoom(wheel < 0 ? +1 : -1, cx, cy);
	}

	bool OnRightClick([[maybe_unused]] Point pt, WidgetID widget) override
	{
		if (widget == WID_PS_PALETTE) {
			const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_PS_PALETTE);
			int cell = ScaleGUITrad(PS_PAL_CELL);
			uint cx = (pt.x - nw->pos_x) / cell;
			uint cy = (pt.y - nw->pos_y) / cell;
			if (cx < 16 && cy < 16) {
				uint8_t idx = static_cast<uint8_t>(cy * 16 + cx);
				if (idx < PALETTE_ANIM_START || idx >= PALETTE_ANIM_START + PALETTE_ANIM_SIZE) {
					this->colour2 = idx;
					this->SetDirty();
				}
			}
			return true;
		}
		/* Auf der Leinwand kein Tooltip - Rechtsklick dient dem Verschieben. */
		return widget == WID_PS_CANVAS;
	}

	void OnPaint() override
	{
#ifndef __EMSCRIPTEN__
		/* Zwischenablage gibt es nur im Web-Build. */
		this->SetWidgetDisabledState(WID_PS_COPY, true);
		this->SetWidgetDisabledState(WID_PS_PASTE, true);
#endif
		this->SetWidgetLoweredState(WID_PS_FILTER_TRAIN, this->show_type[0]);
		this->SetWidgetLoweredState(WID_PS_FILTER_ROAD, this->show_type[1]);
		this->SetWidgetLoweredState(WID_PS_FILTER_SHIP, this->show_type[2]);
		this->SetWidgetLoweredState(WID_PS_FILTER_AIR, this->show_type[3]);
		this->SetWidgetLoweredState(WID_PS_FILTER_HOUSE, this->show_type[4]);
		this->SetWidgetLoweredState(WID_PS_TOOL_PENCIL, this->tool == PsTool::Pencil);
		this->SetWidgetLoweredState(WID_PS_TOOL_FILL, this->tool == PsTool::Fill);
		this->SetWidgetLoweredState(WID_PS_TOOL_PICK, this->tool == PsTool::Pick);
		this->SetWidgetLoweredState(WID_PS_TOOL_ERASE, this->tool == PsTool::Erase);
		this->SetWidgetLoweredState(WID_PS_TOOL_SELECT, this->tool == PsTool::Select);
		this->SetWidgetLoweredState(WID_PS_TOOL_GRADIENT, this->tool == PsTool::Gradient);
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
			/* Links: Typ-Filter + Fahrzeugliste */
			NWidget(NWID_VERTICAL), SetPIP(0, 2, 0),
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_FILTER_TRAIN), SetStringTip(STR_PIXELSTUDIO_FILTER_TRAIN, STR_PIXELSTUDIO_FILTER_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_FILTER_ROAD), SetStringTip(STR_PIXELSTUDIO_FILTER_ROAD, STR_PIXELSTUDIO_FILTER_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_FILTER_SHIP), SetStringTip(STR_PIXELSTUDIO_FILTER_SHIP, STR_PIXELSTUDIO_FILTER_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_FILTER_AIR), SetStringTip(STR_PIXELSTUDIO_FILTER_AIR, STR_PIXELSTUDIO_FILTER_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_FILTER_HOUSE), SetStringTip(STR_PIXELSTUDIO_FILTER_HOUSE, STR_PIXELSTUDIO_FILTER_TOOLTIP), SetFill(1, 0),
				EndContainer(),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_INSET, Colours::Grey, WID_PS_ENGINE_LIST), SetToolTip(STR_PIXELSTUDIO_LIST_TOOLTIP), SetScrollbar(WID_PS_SCROLLBAR), SetFill(0, 1),
					EndContainer(),
					NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_PS_SCROLLBAR),
				EndContainer(),
			EndContainer(),
			/* Mitte: Ansicht + Leinwand + Vorschau */
			NWidget(NWID_VERTICAL), SetPIP(0, 4, 0),
				NWidget(NWID_HORIZONTAL), SetPIP(0, 4, 0),
					NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_PS_VIEW_PREV), SetMinimalSize(24, 14), SetStringTip(STR_PIXELSTUDIO_PREV, STR_PIXELSTUDIO_VIEW_TOOLTIP),
					NWidget(WWT_TEXT, Colours::Invalid, WID_PS_VIEW_LABEL), SetFill(1, 0), SetAlignment({AlignmentH::Centre, AlignmentV::Middle}),
					NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_PS_VIEW_NEXT), SetMinimalSize(24, 14), SetStringTip(STR_PIXELSTUDIO_NEXT, STR_PIXELSTUDIO_VIEW_TOOLTIP),
					NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_PS_ZOOM_OUT), SetMinimalSize(24, 14), SetStringTip(STR_PIXELSTUDIO_ZOOM_OUT, STR_PIXELSTUDIO_ZOOM_OUT_TOOLTIP),
					NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_PS_ZOOM_IN), SetMinimalSize(24, 14), SetStringTip(STR_PIXELSTUDIO_ZOOM_IN, STR_PIXELSTUDIO_ZOOM_IN_TOOLTIP),
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
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_TOOL_SELECT), SetStringTip(STR_PIXELSTUDIO_TOOL_SELECT, STR_PIXELSTUDIO_TOOL_SELECT_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_TEXTBTN, Colours::Yellow, WID_PS_TOOL_GRADIENT), SetStringTip(STR_PIXELSTUDIO_TOOL_GRADIENT, STR_PIXELSTUDIO_TOOL_GRADIENT_TOOLTIP), SetFill(1, 0),
				EndContainer(),
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_PS_COPY), SetStringTip(STR_PIXELSTUDIO_COPY, STR_PIXELSTUDIO_COPY_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_PS_PASTE), SetStringTip(STR_PIXELSTUDIO_PASTE, STR_PIXELSTUDIO_PASTE_TOOLTIP), SetFill(1, 0),
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

#ifdef __EMSCRIPTEN__
/* Vom JavaScript gerufen: auf Sprite-Groesse skaliertes RGBA-Bild aus der
 * Zwischenablage in den Malpuffer uebernehmen (Palette wird automatisch
 * getroffen, Alpha < 128 wird transparent). */
extern "C" void CDECL em_openttd_ps_paste_data(const uint8_t *rgba, int w, int h)
{
	Window *base = FindWindowById(WindowClass::PixelStudio, 0);
	if (base == nullptr) return;
	PixelStudioWindow *win = static_cast<PixelStudioWindow *>(base);
	if (win->cur.width != w || win->cur.height != h) return;
	win->PushUndo();
	for (int i = 0; i < w * h; i++) {
		uint8_t a = rgba[i * 4 + 3];
		win->cur.pixels[i] = a < 128 ? 0 : NearestPaletteIndex(rgba[i * 4 + 0], rgba[i * 4 + 1], rgba[i * 4 + 2]);
	}
	win->stroke_active = false;
	win->SetDirty();
}
#endif

/** Pixel-Studio oeffnen (Fork-Feature). */
void ShowPixelStudioWindow()
{
	PixelStudioLoadOverrides();
	AllocateWindowDescFront<PixelStudioWindow>(_pixelstudio_desc, 0);
}

/** Pixel-Studio mit vorgewaehlter Firmen-Fahne oeffnen (Firmen-Fenster). */
void ShowPixelStudioFlag()
{
	PixelStudioLoadOverrides();
	CloseWindowById(WindowClass::PixelStudio, 0);
	AllocateWindowDescFront<PixelStudioWindow>(_pixelstudio_desc, 0);
	PixelStudioWindow *w = static_cast<PixelStudioWindow *>(FindWindowById(WindowClass::PixelStudio, 0));
	if (w == nullptr) return;
	for (int i = 0; i < (int)w->entries.size(); i++) {
		if (w->entries[i].is_flag) {
			w->SelectEntry(i);
			break;
		}
	}
}
