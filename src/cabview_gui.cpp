/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file cabview_gui.cpp Fuehrerstand (Fork-Feature): Blick nach vorn aus
 * dem Fahrzeug. Kein echtes 3D - die Kacheln vor dem Fahrzeug werden
 * abgetastet und als perspektivische Streifen, Baender und Bloecke
 * gezeichnet, wie in den Lokfuehrer-Spielen der 90er.
 */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "vehicle_base.h"
#include "station_base.h"
#include "order_base.h"
#include "company_func.h"
#include "tile_map.h"
#include "clear_map.h"
#include "tree_map.h"
#include "town_map.h"
#include "station_map.h"
#include "water_map.h"
#include "bridge_map.h"
#include "tunnel_map.h"
#include "house.h"
#include "palette_func.h"
#include "zoom_func.h"
#include "console_func.h"
#include "core/backup_type.hpp"
#include "rail_map.h"
#include "signal_type.h"
#include "table/sprites.h"
#include "direction_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"

#include "widgets/cabview_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/* town_gui.cpp: Haus samt Mehrfach-Kacheln zeichnen (echte Spielgrafik). */
void DrawHouseInGUI(int x, int y, HouseID house_id, int view);
/* tree_gui.cpp: Baum-Sprite fuer die Landschaft. */
PalSpriteID GetCabTreeSprite(uint index);

/** Fahrzeug, aus dem geschaut wird. */
static VehicleID _cab_vehicle = VehicleID::Invalid();

/** So viele Kacheln weit schaut der Fuehrerstand nach vorn. */
static const int CAB_DEPTH = 22;

/** Was auf einer Kachel steht - grob genug fuer die Perspektive. */
enum class CabTile : uint8_t {
	Ground,   ///< Wiese, Feld, Fels.
	Water,    ///< Wasser.
	Trees,    ///< Wald.
	House,    ///< Wohn- oder Geschaeftshaus.
	Industry, ///< Fabrik.
	Station,  ///< Bahnhof, Haltestelle, Flughafen.
	Tunnel,   ///< Tunnel- oder Brueckenkopf.
};

/** Was auf einer Seitenkachel steht - mit den Daten fuer die echte Grafik. */
struct CabObject {
	CabTile what = CabTile::Ground;   ///< Art des Objekts.
	HouseID house = INVALID_HOUSE_ID; ///< Haus-Sprite, falls Haus.
	uint8_t view = 0;                 ///< Blickrichtung des Haus-Sprites.
	uint8_t tree = 0;                 ///< Baumart, falls Wald.
};

/** Ein abgetasteter Punkt der Strecke. */
struct CabSample {
	CabObject left;                   ///< Was links neben der Strecke steht.
	CabObject right;                  ///< Was rechts steht.
	CabTile ahead = CabTile::Ground;  ///< Was auf der Strecke selbst liegt.
	PixelColour ground{PC_GRASS_LAND}; ///< Bodenfarbe der Streckenkachel.
	StationID station = StationID::Invalid(); ///< Bahnhof voraus (fuer das Schild).
	bool signal = false;              ///< Signal an dieser Gleiskachel.
	bool signal_red = false;          ///< Signal zeigt Halt.
	int height = 0;                   ///< Gelaendehoehe (fuer Kuppen und Senken).
	bool valid = false;               ///< Kachel liegt noch auf der Karte.
};

/** Zoomstufe fuer die Entfernung - so werden Sprites perspektivisch klein. */
static ZoomLevel CabZoom(float d, bool flying = false)
{
	/* Aus der Luft ist alles eine Stufe kleiner - man schaut von oben. */
	if (flying) {
		if (d < 3.0f) return ZoomLevel::In2x;
		if (d < 6.0f) return ZoomLevel::Normal;
		return ZoomLevel::Out2x;
	}
	if (d < 3.0f) return ZoomLevel::In4x;
	if (d < 6.0f) return ZoomLevel::In2x;
	if (d < 10.0f) return ZoomLevel::Normal;
	return ZoomLevel::Out2x;
}

/** Kachel einordnen und ihre Bodenfarbe bestimmen. */
static CabTile ClassifyTile(TileIndex t, PixelColour *ground = nullptr)
{
	if (ground != nullptr) *ground = PC_GRASS_LAND;
	switch (GetTileType(t)) {
		case TileType::Water:
			if (ground != nullptr) *ground = PC_WATER;
			return CabTile::Water;
		case TileType::Trees:
			if (ground != nullptr) *ground = PC_TREES;
			return CabTile::Trees;
		case TileType::House:
			return CabTile::House;
		case TileType::Industry:
			return CabTile::Industry;
		case TileType::Station:
			return CabTile::Station;
		case TileType::TunnelBridge:
			return CabTile::Tunnel;
		case TileType::Clear:
			if (ground != nullptr) {
				switch (GetClearGround(t)) {
					case ClearGround::Fields: *ground = PC_FIELDS; break;
					case ClearGround::Rough: *ground = PC_ROUGH_LAND; break;
					case ClearGround::Rocks: *ground = PC_BARE_LAND; break;
					case ClearGround::Desert: *ground = PC_BARE_LAND; break;
					default: *ground = PC_GRASS_LAND; break;
				}
			}
			return CabTile::Ground;
		default:
			return CabTile::Ground;
	}
}

/** Kachel samt Sprite-Daten einlesen. */
static CabObject ReadObject(TileIndex t)
{
	CabObject o;
	o.what = ClassifyTile(t);
	if (o.what == CabTile::House) {
		o.house = GetHouseType(t);
		o.view = TileHash2Bit(TileX(t) * TILE_SIZE, TileY(t) * TILE_SIZE);
	} else if (o.what == CabTile::Trees) {
		o.tree = static_cast<uint8_t>(GetTreeType(t));
	}
	return o;
}

struct CabViewWindow : Window {
	uint anim = 0;      ///< Laeuft mit dem Tempo mit (Schwellen, Fahrbahnstreifen).
	int horizon = 0;    ///< Bildschirmzeile des Horizonts.

	CabViewWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->InitNested(number);
		this->FitToScreen();
	}

	/**
	 * Der Fuehrerstand nimmt den ganzen Bildschirm ein. ResizeWindow statt
	 * width/height direkt zu setzen - sonst waechst das Widget-Layout nicht
	 * mit und das Bild bleibt ein schmaler Streifen.
	 */
	void FitToScreen()
	{
		int dx = _screen.width - this->width;
		int dy = _screen.height - this->height;
		if (dx != 0 || dy != 0) ResizeWindow(this, dx, dy, false);
		this->left = 0;
		this->top = 0;
		this->SetDirty();
	}

	const Vehicle *Cab() const
	{
		return Vehicle::GetIfValid(_cab_vehicle);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		const Vehicle *v = this->Cab();
		if (widget != WID_CB_INFO || v == nullptr) return this->Window::GetWidgetString(widget, stringid);
		std::string s = GetString(STR_CABVIEW_INFO, v->index, v->GetDisplaySpeed());
		const Order *o = v->GetNumOrders() > 0 ? v->GetOrder(v->cur_real_order_index) : nullptr;
		if (o != nullptr && o->IsType(OT_GOTO_STATION)) {
			s += GetString(STR_CABVIEW_NEXT_STOP, o->GetDestination().ToStationID());
		}
		return s;
	}

	/** Strecke vor dem Fahrzeug abtasten. */
	std::vector<CabSample> Scan(const Vehicle *v) const
	{
		std::vector<CabSample> out(CAB_DEPTH);
		TileIndex tile = TileVirtXY(v->x_pos, v->y_pos);
		DiagDirection fwd = DirToDiagDir(v->direction);
		DiagDirection lft = ChangeDiagDir(fwd, DiagDirDiff::Left90);
		DiagDirection rgt = ChangeDiagDir(fwd, DiagDirDiff::Right90);
		int base_height = IsValidTile(tile) ? (int)TileHeight(tile) : 0;

		for (int d = 0; d < CAB_DEPTH; d++) {
			tile = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDiagDir(fwd));
			if (tile == INVALID_TILE) break;
			CabSample &s = out[d];
			s.valid = true;
			s.ahead = ClassifyTile(tile, &s.ground);
			s.height = (int)TileHeight(tile) - base_height;
			if (s.ahead == CabTile::Station) s.station = GetStationIndex(tile);
			/* Signale an der Strecke: rot oder gruen, wie im Spiel. */
			if (IsTileType(tile, TileType::Railway) && HasSignals(tile)) {
				s.signal = true;
				s.signal_red = GetSignalStates(tile) == 0;
			}
			TileIndex l = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDiagDir(lft));
			TileIndex r = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDiagDir(rgt));
			if (l != INVALID_TILE) s.left = ReadObject(l);
			if (r != INVALID_TILE) s.right = ReadObject(r);
		}
		return out;
	}

	/** Perspektive: Bildschirmzeile des Bodens in Tiefe d. */
	int GroundY(const Rect &r, float d) const
	{
		float t = 1.0f / (1.0f + d * 0.42f);
		return this->horizon + (int)((r.bottom - this->horizon) * t);
	}

	/** Perspektive: halbe Streckenbreite in Tiefe d. */
	float HalfWidth(const Rect &r, float d) const
	{
		float t = 1.0f / (1.0f + d * 0.42f);
		return (r.right - r.left) * 0.22f * t;
	}

	/**
	 * Ein Objekt neben der Strecke zeichnen - mit der ECHTEN Spielgrafik,
	 * damit man Haeuser und Waelder im Fuehrerstand wiedererkennt. Die
	 * Groesse kommt ueber die Zoomstufe (DrawSprite nimmt sie entgegen,
	 * DrawHouseInGUI ueber _gui_zoom).
	 */
	void DrawSideObject(const Rect &r, const CabObject &o, float d, bool left_side, bool flying = false) const
	{
		if (o.what == CabTile::Ground || o.what == CabTile::Water) return;
		/* Ganz nahe ist man schon vorbei, ganz fern nur noch Pixelmatsch. */
		if (d < 1.0f || d > 13.0f) return;
		float t = 1.0f / (1.0f + d * 0.42f);
		/* Die Sprites sollen auf dem Boden stehen, nicht darueber schweben. */
		int ground = this->GroundY(r, d) + (int)(8 * t);
		int cx = (r.left + r.right) / 2;
		float hw = this->HalfWidth(r, d);
		int x = left_side ? (int)(cx - hw * 2.0f) : (int)(cx + hw * 2.0f);
		ZoomLevel zoom = CabZoom(d, flying);

		switch (o.what) {
			case CabTile::House: {
				/* Das echte Haus-Sprite; _gui_zoom steuert hier die Groesse. */
				AutoRestoreBackup zoom_backup(_gui_zoom, zoom);
				DrawHouseInGUI(x, ground, o.house, o.view);
				break;
			}
			case CabTile::Trees: {
				PalSpriteID tree = GetCabTreeSprite(o.tree);
				DrawSprite(tree.sprite, tree.pal, x, ground, nullptr, zoom);
				break;
			}
			case CabTile::Industry: {
				/* Fabrikhalle mit Schornstein - Sprites waeren hier ganze
				 * Kachel-Layouts, deshalb bleibt es bei der Silhouette. */
				int h = (int)(130 * t);
				int w = std::max(3, (int)(hw * 0.9f));
				if (h < 3) break;
				GfxFillRect(x - w / 2, ground - h, x + w / 2, ground, PC_DARK_GREY);
				GfxFillRect(x - w / 6, ground - h - h / 2, x + w / 12, ground - h, PC_GREY);
				break;
			}
			case CabTile::Station: {
				/* Bahnhofsgebaeude: heller Bau mit rotem Dach. */
				int h = (int)(60 * t);
				int w = std::max(3, (int)(hw * 0.9f));
				if (h < 3) break;
				int base = ground - (int)(8 * t);
				GfxFillRect(x - w / 2, base - h, x + w / 2, base, PC_GREY);
				GfxFillRect(x - w / 2 - w / 8, base - h - std::max(2, h / 5), x + w / 2 + w / 8, base - h, PC_DARK_RED);
				break;
			}
			case CabTile::Tunnel: {
				int h = (int)(110 * t);
				int w = std::max(3, (int)(hw * 1.1f));
				if (h < 3) break;
				GfxFillRect(x - w / 2, ground - h, x + w / 2, ground, PC_VERY_DARK_BROWN);
				break;
			}
			default: break;
		}
	}

	/**
	 * Bahnhofsschild wie im Spiel: heller Kasten mit dem Stationsnamen,
	 * der beim Naeherkommen groesser wird.
	 */
	void DrawStationSign(const Rect &r, StationID id, float d) const
	{
		if (!Station::IsValidID(id) || d > 10.0f) return;
		float t = 1.0f / (1.0f + d * 0.42f);
		int ground = this->GroundY(r, d);
		int cx = (r.left + r.right) / 2;
		float hw = this->HalfWidth(r, d);
		int x = cx + (int)(hw * 1.6f);
		int post = (int)(70 * t);
		if (post < 6) return;
		/* Pfosten und Schild. */
		GfxFillRect(x - 1, ground - post, x + 1, ground, PC_GREY);
		std::string name = GetString(STR_STATION_NAME, id);
		int tw = GetStringBoundingBox(name).width;
		int pad = std::max(2, (int)(6 * t));
		int sw = std::min<int>(tw + pad * 2, (r.right - r.left) / 3);
		int sh = GetCharacterHeight(FontSize::Normal) + pad;
		GfxFillRect(x - sw / 2, ground - post - sh, x + sw / 2, ground - post, PC_WHITE);
		GfxFillRect(x - sw / 2, ground - post - sh, x + sw / 2, ground - post - sh + 1, PC_BLACK);
		DrawString(x - sw / 2 + pad, x + sw / 2 - pad, ground - post - sh + pad / 2, name, TextColour::Black, AlignmentH::Centre);
	}

	/** Signal am Gleisrand - gruen oder rot wie im Spiel. */
	void DrawSignal(const Rect &r, bool red, float d) const
	{
		float t = 1.0f / (1.0f + d * 0.42f);
		int ground = this->GroundY(r, d);
		int cx = (r.left + r.right) / 2;
		float hw = this->HalfWidth(r, d);
		int x = cx + (int)(hw * 1.25f);
		int post = (int)(60 * t);
		int lamp = std::max(2, (int)(14 * t));
		if (post < 5) return;
		GfxFillRect(x - std::max(1, lamp / 6), ground - post, x + std::max(1, lamp / 6), ground, PC_GREY);
		GfxFillRect(x - lamp / 2, ground - post - lamp, x + lamp / 2, ground - post, PC_BLACK);
		GfxFillRect(x - lamp / 2 + 1, ground - post - lamp + 1, x + lamp / 2 - 1, ground - post - 1,
				red ? PC_RED : PC_GREEN);
	}

	/** Wolken fuer den Flug - sie ziehen mit dem Tempo nach unten weg. */
	void DrawClouds(const Rect &r) const
	{
		int w = r.right - r.left;
		int sky = this->horizon - r.top;
		if (sky < 20) return;
		for (int i = 0; i < 5; i++) {
			/* Feste Startpunkte, damit die Wolken nicht flackern. */
			int cw = w / (6 + i);
			int ch = std::max(3, sky / (10 + i));
			int cx = r.left + ((i * 37 + (int)(this->anim / 6)) % (w + cw)) - cw;
			int cy = r.top + sky / 6 + (i * sky) / 9;
			GfxFillRect(cx, cy, cx + cw, cy + ch, PC_WHITE);
			GfxFillRect(cx + cw / 4, cy - ch / 2, cx + cw - cw / 4, cy, PC_WHITE);
		}
	}

	/**
	 * Der Fuehrerstand selbst: Dachbalken, Fensterstreben und ein Pult mit
	 * Tacho-Anzeige - alles gezeichnet, wie es die Lokfuehrer-Spiele der
	 * 90er auch gemacht haben.
	 */
	void DrawCabInterior(const Rect &r, const Vehicle *v) const
	{
		int w = r.right - r.left;
		int h = r.bottom - r.top;
		int strut = std::max(10, w / 22);
		PixelColour body = PC_DARK_GREY;

		/* Dach und seitliche Streben. */
		GfxFillRect(r.left, r.top, r.right, r.top + strut / 2, body);
		GfxFillRect(r.left, r.top, r.left + strut, r.bottom, body);
		GfxFillRect(r.right - strut, r.top, r.right, r.bottom, body);

		/* Pult am unteren Rand, leicht nach vorn gewoelbt. */
		int desk = r.bottom - h / 5;
		GfxFillRect(r.left, desk, r.right, r.bottom, body);
		GfxFillRect(r.left, desk, r.right, desk + std::max(2, h / 160), PC_GREY);

		if (v == nullptr) return;

		/* Tacho: Balken plus grosse Ziffern, gelb auf schwarz. */
		int speed = v->GetDisplaySpeed();
		int max_speed = std::max(1, v->GetDisplayMaxSpeed());
		int gx = r.left + strut + w / 20;
		int gy = desk + h / 40;
		int gw = w / 4;
		int gh = h / 12;
		GfxFillRect(gx, gy, gx + gw, gy + gh, PC_BLACK);
		int fill = std::clamp(speed * gw / max_speed, 0, gw);
		GfxFillRect(gx + 2, gy + gh / 2, gx + 2 + fill, gy + gh - 3, speed * 10 > max_speed * 9 ? PC_RED : PC_GREEN);
		DrawString(gx + 4, gx + gw - 4, gy + 2, GetString(STR_CABVIEW_DASH_SPEED, speed),
				TextColour::Yellow, AlignmentH::Start, false, FontSize::Large);

		/* Rechts das Ziel, damit man weiss, wohin die Reise geht. */
		const Order *o = v->GetNumOrders() > 0 ? v->GetOrder(v->cur_real_order_index) : nullptr;
		if (o != nullptr && o->IsType(OT_GOTO_STATION)) {
			int dx0 = r.right - strut - w / 3;
			GfxFillRect(dx0, gy, r.right - strut - w / 20, gy + gh, PC_BLACK);
			DrawString(dx0 + 4, r.right - strut - w / 20 - 4, gy + gh / 4,
					GetString(STR_CABVIEW_DASH_TARGET, o->GetDestination().ToStationID()),
					TextColour::Orange, AlignmentH::Centre);
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_CB_VIEW) return;
		const Vehicle *v = this->Cab();
		/* Aus dem Cockpit blickt man von oben auf die Welt, im Schiff
		 * dicht ueber dem Wasser - der Horizont wandert entsprechend. */
		int horizon_pct = 42;
		if (v != nullptr && v->type == VehicleType::Aircraft) horizon_pct = 30;
		if (v != nullptr && v->type == VehicleType::Ship) horizon_pct = 46;
		const_cast<CabViewWindow *>(this)->horizon = r.top + (r.bottom - r.top) * horizon_pct / 100;

		/* Himmel mit Bandverlauf, Sonne knapp ueber dem Horizont. */
		static const PixelColour sky[] = {PC_DARK_BLUE, PC_DARK_BLUE, PC_LIGHT_BLUE, PC_LIGHT_BLUE};
		int bands = lengthof(sky);
		for (int i = 0; i < bands; i++) {
			int y0 = r.top + (this->horizon - r.top) * i / bands;
			int y1 = r.top + (this->horizon - r.top) * (i + 1) / bands;
			GfxFillRect(r.left, y0, r.right, y1, sky[i]);
		}
		if (v == nullptr) return;

		std::vector<CabSample> scan = this->Scan(v);
		int cx = (r.left + r.right) / 2;

		/* Boden von hinten nach vorn: jede Kachel ein Streifen. */
		for (int d = CAB_DEPTH - 1; d >= 0; d--) {
			const CabSample &s = scan[d];
			if (!s.valid) continue;
			int y_far = this->GroundY(r, (float)d + 1);
			int y_near = this->GroundY(r, (float)d);
			if (y_near <= y_far) continue;
			/* Entferntes wird blasser - billiger Tiefendunst. */
			PixelColour col = s.ground;
			if (d > 14) col = PC_ROUGH_LAND;
			GfxFillRect(r.left, y_far, r.right, y_near, col);
			/* Jede zweite Kachel etwas dunkler; der Wechsel wandert mit dem
			 * Tempo und macht die Fahrt ueberhaupt erst sichtbar. */
			if (d < 12 && ((d + this->anim / 16) & 1) == 0) {
				GfxFillRect(r.left, y_far, r.right, y_near, PC_BLACK, FillRectMode::Checker);
			}

			bool flying_view = v->type == VehicleType::Aircraft;
			this->DrawSideObject(r, s.left, (float)d, true, flying_view);
			this->DrawSideObject(r, s.right, (float)d, false, flying_view);
			if (s.signal) this->DrawSignal(r, s.signal_red, (float)d);
			if (s.station != StationID::Invalid()) this->DrawStationSign(r, s.station, (float)d);

			/* Fahrweg: Bahndamm, Fahrbahn oder Fahrrinne. Flugzeuge haben
			 * keinen - dort bleibt die Landschaft, ueber die man fliegt. */
			float hw_far = this->HalfWidth(r, (float)d + 1);
			float hw_near = this->HalfWidth(r, (float)d);
			bool rail = v->type == VehicleType::Train;
			bool flying = v->type == VehicleType::Aircraft;
			if (!flying) {
				PixelColour bed = rail ? PC_BARE_LAND : PC_DARK_GREY;
				if (v->type == VehicleType::Ship) bed = PC_WATER;
				/* Trapez als Zeilen fuellen, damit die Kanten zusammenlaufen. */
				for (int y = y_far; y <= y_near; y++) {
					float f = (y_near == y_far) ? 0.0f : (float)(y - y_far) / (float)(y_near - y_far);
					float hw = hw_far + (hw_near - hw_far) * f;
					GfxFillRect(cx - (int)hw, y, cx + (int)hw, y, bed);
				}
			}
			/* Schienen bzw. Mittelstreifen. */
			if (rail) {
				GfxDrawLine(cx - (int)(hw_far * 0.6f), y_far, cx - (int)(hw_near * 0.6f), y_near, PC_GREY, std::max(1, (int)(hw_near / 12)));
				GfxDrawLine(cx + (int)(hw_far * 0.6f), y_far, cx + (int)(hw_near * 0.6f), y_near, PC_GREY, std::max(1, (int)(hw_near / 12)));
				/* Schwellen wandern mit dem Tempo auf den Betrachter zu. */
				int sleeper = y_near - (int)((this->anim % 24) * (y_near - y_far) / 24);
				if (sleeper > y_far && sleeper < y_near) {
					float f = (float)(sleeper - y_far) / (float)std::max(1, y_near - y_far);
					float hw = hw_far + (hw_near - hw_far) * f;
					GfxFillRect(cx - (int)(hw * 0.75f), sleeper, cx + (int)(hw * 0.75f), sleeper + std::max(1, (int)(hw / 16)), PC_DARK_GREY);
				}
			} else if (v->type == VehicleType::Road) {
				int dash = y_near - (int)((this->anim % 20) * (y_near - y_far) / 20);
				if (dash > y_far && dash < y_near) {
					GfxFillRect(cx - std::max(1, (int)(hw_near / 14)), dash, cx + std::max(1, (int)(hw_near / 14)), dash + std::max(1, (int)(hw_near / 8)), PC_WHITE);
				}
			} else if (v->type == VehicleType::Ship) {
				/* Wellenkaemme laufen dem Bug entgegen. */
				int wave = y_near - (int)((this->anim % 28) * (y_near - y_far) / 28);
				if (wave > y_far && wave < y_near) {
					float f = (float)(wave - y_far) / (float)std::max(1, y_near - y_far);
					float hw = hw_far + (hw_near - hw_far) * f;
					GfxFillRect(cx - (int)(hw * 0.7f), wave, cx + (int)(hw * 0.7f), wave + std::max(1, (int)(hw / 18)), PC_LIGHT_BLUE);
				}
			}

			/* Bahnhof voraus: Bahnsteigkante ueber die ganze Breite. */
			if (s.ahead == CabTile::Station) {
				GfxFillRect(cx - (int)(hw_near * 1.8f), y_far, cx - (int)(hw_near * 0.9f), y_near, PC_GREY);
				GfxFillRect(cx + (int)(hw_near * 0.9f), y_far, cx + (int)(hw_near * 1.8f), y_near, PC_GREY);
			}
		}

		if (v->type == VehicleType::Aircraft) this->DrawClouds(r);
		this->DrawCabInterior(r, v);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget == WID_CB_CLOSE) this->Close();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		_cab_vehicle = VehicleID::Invalid();
		if (FindWindowById(WindowClass::MainToolbar, 0) == nullptr) ShowVitalWindows();
		this->Window::Close(data);
	}

	/** Bild und Schwellenlauf mit dem Tempo aktualisieren. */
	const IntervalTimer<TimerWindow> tick = {std::chrono::milliseconds(50), [this](auto) {
		const Vehicle *v = this->Cab();
		if (v == nullptr) {
			this->Close();
			return;
		}
		this->anim += std::max(1, v->GetDisplaySpeed() / 8);
		if (this->width != _screen.width || this->height != _screen.height) this->FitToScreen();
		this->SetDirty();
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_cabview_widgets = {
	NWidget(NWID_VERTICAL),
		NWidget(WWT_PANEL, Colours::DarkGreen, WID_CB_VIEW), SetFill(1, 1), SetResize(1, 1), SetMinimalSize(400, 240), EndContainer(),
		NWidget(WWT_PANEL, Colours::DarkGreen), SetFill(1, 0), SetResize(1, 0),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0), SetPadding(WidgetDimensions::unscaled.framerect),
				NWidget(WWT_TEXT, Colours::Invalid, WID_CB_INFO), SetMinimalSize(360, 12), SetFill(1, 0), SetResize(1, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_CB_CLOSE), SetMinimalSize(130, 12), SetStringTip(STR_CABVIEW_CLOSE, STR_CABVIEW_CLOSE_TOOLTIP),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _cabview_desc(
	WindowPosition::Manual, {}, 0, 0,
	WindowClass::CabView, WindowClass::None,
	{},
	_nested_cabview_widgets
);

/** Fork: Fuehrerstand oeffnen (Blick nach vorn aus dem Fahrzeug). */
void ShowCabView(VehicleID veh)
{
	const Vehicle *v = Vehicle::GetIfValid(veh);
	if (v == nullptr || _game_mode != GameMode::Normal) return;
	CloseWindowById(WindowClass::CabView, 0);
	_cab_vehicle = v->GetMovingFront()->index;
	CloseAllNonVitalWindows();
	IConsoleClose();
	CloseWindowById(WindowClass::MainToolbar, 0);
	CloseWindowById(WindowClass::Statusbar, 0);
	AllocateWindowDescFront<CabViewWindow>(_cabview_desc, 0);
}

/** Fork: Konsolen-Einstieg "cab [index]" - ohne Angabe das erste eigene Fahrzeug. */
bool ShowCabViewConsole(int index)
{
	VehicleID pick = VehicleID::Invalid();
	if (index >= 0) {
		pick = VehicleID(index);
	} else {
		for (const Vehicle *v : Vehicle::Iterate()) {
			if (v->owner == _local_company && v->IsPrimaryVehicle()) { pick = v->index; break; }
		}
	}
	if (!Vehicle::IsValidID(pick)) return false;
	ShowCabView(pick);
	return true;
}
