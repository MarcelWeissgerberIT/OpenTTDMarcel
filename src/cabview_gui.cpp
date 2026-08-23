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
#include "road_map.h"
#include "station_map.h"
#include "tunnelbridge_map.h"
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

/** So viele Kachelreihen links und rechts der Strecke werden gelesen. */
static const int CAB_SIDE_ROWS = 4;

/** Was auf einer Seitenkachel steht - mit den Daten fuer die echte Grafik. */
struct CabObject {
	CabTile what = CabTile::Ground;   ///< Art des Objekts.
	HouseID house = INVALID_HOUSE_ID; ///< Haus-Sprite, falls Haus.
	uint8_t view = 0;                 ///< Blickrichtung des Haus-Sprites.
	uint8_t tree = 0;                 ///< Baumart, falls Wald.
	PixelColour ground{PC_GRASS_LAND}; ///< Boden dieser Kachel.
};

/** Ein abgetasteter Punkt der Strecke. */
struct CabSample {
	/* Vier Reihen je Seite: mit nur zweien blieb die Landschaft leer,
	 * und genau das liess den Fuehrerstand karg aussehen. */
	CabObject left[CAB_SIDE_ROWS];    ///< Kacheln links der Strecke, von innen nach aussen.
	CabObject right[CAB_SIDE_ROWS];   ///< Kacheln rechts der Strecke.
	CabTile ahead = CabTile::Ground;  ///< Was auf der Strecke selbst liegt.
	PixelColour ground{PC_GRASS_LAND}; ///< Bodenfarbe der Streckenkachel.
	StationID station = StationID::Invalid(); ///< Bahnhof voraus (fuer das Schild).
	bool signal = false;              ///< Signal an dieser Gleiskachel.
	bool signal_red = false;          ///< Signal zeigt Halt.
	float lateral = 0.0f;             ///< Seitlicher Versatz durch Kurven.
	bool tunnel_mouth = false;        ///< Hier faehrt die Strecke in einen Tunnel.
	bool on_bridge = false;           ///< Strecke verlaeuft hier auf einer Bruecke.
	int height = 0;                   ///< Gelaendehoehe (fuer Kuppen und Senken).
	TileIndex tile = INVALID_TILE;    ///< Die Kachel selbst - fuer ortsfeste Bodenmuster.
	bool valid = false;               ///< Kachel liegt noch auf der Karte.
};

/** Liegt auf dieser Kachel ein Gleis, dem wir folgen koennen? */
static bool CabHasRail(TileIndex t)
{
	if (!IsValidTile(t)) return false;
	if (IsTileType(t, TileType::Railway)) return true;
	if (IsTileType(t, TileType::Station)) return HasStationRail(t);
	if (IsTileType(t, TileType::TunnelBridge)) return GetTunnelBridgeTransportType(t) == TransportType::Rail;
	if (IsLevelCrossingTile(t)) return true;
	return false;
}

/** Fuehrt von dieser Kachel eine Strasse in die gegebene Richtung? */
static bool CabRoadLeaves(TileIndex t, DiagDirection d)
{
	if (!IsValidTile(t)) return false;
	if (!MayHaveRoad(t)) return false;
	return GetAnyRoadBits(t, RoadTramType::Road, true).Any(DiagDirToRoadBits(d));
}

/**
 * Der Strecke folgen statt stur geradeaus zu schauen: geradeaus hat
 * Vorrang, sonst wird die Kurve genommen, die weitergeht. Liefert die
 * neue Richtung und -1/0/+1 fuer die Kurvenrichtung.
 */
static DiagDirection CabNextDir(TileIndex tile, DiagDirection dir, VehicleType type, int *turn)
{
	*turn = 0;
	if (type != VehicleType::Train && type != VehicleType::Road) return dir;

	DiagDirection straight = dir;
	DiagDirection left = ChangeDiagDir(dir, DiagDirDiff::Left90);
	DiagDirection right = ChangeDiagDir(dir, DiagDirDiff::Right90);
	const DiagDirection cand[3] = {straight, left, right};
	const int turns[3] = {0, -1, 1};

	for (int i = 0; i < 3; i++) {
		if (type == VehicleType::Road) {
			if (!CabRoadLeaves(tile, cand[i])) continue;
		} else {
			TileIndex next = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDiagDir(cand[i]));
			if (!CabHasRail(next)) continue;
			/* Auf einer geraden Kachel gibt es keine Abzweigung - nur wenn
			 * die Kachel selbst eine Kurve traegt, darf abgebogen werden. */
			if (i > 0 && IsTileType(tile, TileType::Railway) && !IsLevelCrossingTile(tile)) {
				TrackBits tb = GetTrackBits(tile);
				/* Nur Kurvenstuecke erlauben ein Abbiegen - auf einem
				 * geraden Gleis geht es geradeaus weiter. */
				if (!tb.Any(TRACK_BIT_HORZ | TRACK_BIT_VERT)) continue;
			}
		}
		*turn = turns[i];
		return cand[i];
	}
	return dir;
}

/** Zoomstufe fuer die Entfernung - so werden Sprites perspektivisch klein. */
static ZoomLevel CabZoom(float d, bool flying = false)
{
	/* Aus der Luft ist alles eine Stufe kleiner - man schaut von oben. */
	if (flying) {
		if (d < 3.0f) return ZoomLevel::In2x;
		if (d < 6.0f) return ZoomLevel::Normal;
		return ZoomLevel::Out2x;
	}
	/* Fork: Die Stufen liegen bewusst niedrig. Ein Haus in vierfacher
	 * Groesse fuellt den halben Bildschirm, verdeckt die Strecke und
	 * zeigt sein Dach von oben - aus dem Fahrersitz sieht man das nicht. */
	if (d < 1.6f) return ZoomLevel::In4x;
	if (d < 4.5f) return ZoomLevel::In2x;
	if (d < 8.5f) return ZoomLevel::Normal;
	if (d < 14.0f) return ZoomLevel::Out2x;
	return ZoomLevel::Out4x;
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

/**
 * Fork: Das Bodenbild einer Kachel - die ECHTE Spielgrafik. Vorher lag
 * hier nur eine Farbflaeche, und genau das liess den Fuehrerstand wie
 * ein Modell statt wie OpenTTD aussehen.
 */
static SpriteID CabGroundSprite(CabTile what, PixelColour ground)
{
	if (what == CabTile::Water) return SPR_FLAT_WATER_TILE;
	if (ground.p == PC_ROUGH_LAND.p) return SPR_FLAT_ROUGH_LAND;
	if (ground.p == PC_BARE_LAND.p) return SPR_FLAT_BARE_LAND;
	if (ground.p == PC_FIELDS.p) return SPR_FLAT_BARE_LAND;
	return SPR_FLAT_GRASS_TILE;
}

/** Kachel samt Sprite-Daten einlesen. */
static CabObject ReadObject(TileIndex t)
{
	CabObject o;
	o.what = ClassifyTile(t, &o.ground);
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
	float sub = 0.0f;   ///< Wie weit das Fahrzeug in der aktuellen Kachel steht (0..1).

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
		int base_height = IsValidTile(tile) ? (int)TileHeight(tile) : 0;
		float lateral = 0.0f;
		float drift = 0.0f;

		for (int d = 0; d < CAB_DEPTH; d++) {
			/* Der Strecke folgen: die naechste Kachel ergibt sich aus dem
			 * tatsaechlichen Gleis- bzw. Strassenverlauf. */
			int turn = 0;
			DiagDirection next_dir = CabNextDir(tile, fwd, v->type, &turn);
			tile = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDiagDir(next_dir));
			if (tile == INVALID_TILE) break;
			fwd = next_dir;
			/* Kurven schieben den Fluchtpunkt zur Seite - wie in den
			 * Pseudo-3D-Rennspielen; drift sorgt fuer weiche Boegen. */
			drift += turn * 0.5f;
			lateral += drift;
			DiagDirection lft = ChangeDiagDir(fwd, DiagDirDiff::Left90);
			DiagDirection rgt = ChangeDiagDir(fwd, DiagDirDiff::Right90);
			CabSample &s = out[d];
			s.valid = true;
			s.tile = tile;
			s.lateral = lateral;
			s.ahead = ClassifyTile(tile, &s.ground);
			s.height = (int)TileHeight(tile) - base_height;
			if (IsTileType(tile, TileType::TunnelBridge)) {
				if (IsTunnelTile(tile)) {
					s.tunnel_mouth = true;
				} else {
					s.on_bridge = true;
				}
			}
			if (s.ahead == CabTile::Station) s.station = GetStationIndex(tile);
			/* Signale an der Strecke: rot oder gruen, wie im Spiel. */
			if (IsTileType(tile, TileType::Railway) && HasSignals(tile)) {
				s.signal = true;
				s.signal_red = GetSignalStates(tile) == 0;
			}
			/* Reihenweise nach aussen tasten - so fuellt sich die
			 * Landschaft bis zum Bildrand mit echten Kacheln. */
			TileIndex lt = tile;
			TileIndex rt = tile;
			for (int row = 0; row < CAB_SIDE_ROWS; row++) {
				if (lt != INVALID_TILE) {
					lt = AddTileIndexDiffCWrap(lt, TileIndexDiffCByDiagDir(lft));
					if (lt != INVALID_TILE) s.left[row] = ReadObject(lt);
				}
				if (rt != INVALID_TILE) {
					rt = AddTileIndexDiffCWrap(rt, TileIndexDiffCByDiagDir(rgt));
					if (rt != INVALID_TILE) s.right[row] = ReadObject(rt);
				}
			}
		}
		return out;
	}

	/**
	 * Tiefe einer Kachel aus Sicht der Kamera. Der Bruchteil sub sorgt
	 * dafuer, dass die Landschaft zwischen zwei Kacheln WEITERLAEUFT -
	 * ohne ihn springt das Bild nur beim Kachelwechsel und ruckelt.
	 */
	float Depth(float d) const
	{
		return d - this->sub;
	}

	/**
	 * Perspektive: Bildschirmzeile des Bodens in Tiefe d. Der Hoehenversatz
	 * macht Kuppen und Senken der Strecke sichtbar - bergauf wandert der
	 * Boden nach oben, bergab nach unten.
	 */
	int GroundY(const Rect &r, float d, int height = 0) const
	{
		float t = 1.0f / (1.0f + d * 0.42f);
		int y = this->horizon + (int)((r.bottom - this->horizon) * t);
		return y - (int)(height * 26 * t);
	}

	/** Mitte der Strecke in Tiefe d - wandert in Kurven zur Seite. */
	int CenterX(const Rect &r, float d, float lateral) const
	{
		float t = 1.0f / (1.0f + d * 0.42f);
		int cx = (r.left + r.right) / 2;
		return cx + (int)(lateral * (r.right - r.left) * 0.10f * t);
	}

	/**
	 * Perspektive: halbe Streckenbreite in Tiefe d. Der Wert bestimmt
	 * zugleich das Sichtfeld - je schmaler die Trasse, desto mehr
	 * Umgebung passt links und rechts ins Bild.
	 */
	float HalfWidth(const Rect &r, float d) const
	{
		float t = 1.0f / (1.0f + d * 0.42f);
		return (r.right - r.left) * 0.165f * t;
	}

	/**
	 * Bodenmuster einer Tiefe. Echte Kachelgrafiken taugen hier nicht:
	 * OpenTTD-Rauten sind immer 2:1, eine Kachel in der Ferne ist aber
	 * flach und breit - gezeichnet stapeln sie sich zu einer Wand.
	 * Stattdessen streuen wir Halme, Furchen und Wellen perspektivisch
	 * korrekt: Groesse und Dichte richten sich nach der Entfernung, die
	 * Positionen haengen an der Kachel und wandern deshalb mit der
	 * Fahrt mit, statt am Bildschirm zu kleben.
	 */
	void DrawGroundTexture(const Rect &r, const CabSample &s, int d, float dn) const
	{
		if (!s.valid || s.tile == INVALID_TILE) return;
		float t = 1.0f / (1.0f + dn * 0.42f);
		if (t < 0.06f) return; /* Zu weit weg - dort traegt die Flaeche. */
		int specks = (dn < 4.0f) ? 54 : (dn < 9.0f ? 36 : 20);

		for (int k = 0; k < specks; k++) {
			/* Deterministisch aus Kachel und Zaehler: kein Flackern. */
			uint32_t h = (uint32_t)(TileX(s.tile) * 73856093u ^ TileY(s.tile) * 19349663u ^ (uint32_t)k * 83492791u);
			float frac = ((h >> 8) & 0xFF) / 255.0f;          /* Tiefe in der Kachel */
			float lat = (((h >> 16) & 0xFF) / 255.0f - 0.5f) * 26.4f; /* bis zu drei Kacheln seitlich */
			float dd = dn + frac;
			int y = this->GroundY(r, dd, s.height);
			if (y <= this->horizon) continue;
			int x = this->CenterX(r, dd, s.lateral + lat);
			if (x < r.left || x > r.right) continue;

			float tt = 1.0f / (1.0f + dd * 0.42f);
			int sz = std::max(1, (int)(10 * tt));

			/* Was auf der Kachel liegt, bestimmt das Muster. */
			int row = std::min(CAB_SIDE_ROWS - 1, (int)(std::fabs(lat) / 2.2f));
			const CabObject *side = (lat < 0.0f) ? &s.left[row] : &s.right[row];
			PixelColour base = side->ground;
			if (side->what == CabTile::Water) {
				/* Kurze, versetzte Wellenkaemme - lange Striche sehen aus
				 * wie ein Streifenmuster statt wie Wasser. */
				GfxFillRect(x - sz, y, x + sz, y + std::max(1, sz / 4), PC_LIGHT_BLUE);
			} else if (base.p == PC_FIELDS.p) {
				/* Ackerfurchen, dunkler als der Boden. */
				GfxFillRect(x - sz, y, x + sz, y + std::max(1, sz / 3), PC_BARE_LAND);
			} else if (base.p == PC_ROUGH_LAND.p || base.p == PC_BARE_LAND.p) {
				/* Steppe: Steine und trockene Bueschel. */
				GfxFillRect(x - sz / 2, y - sz / 2, x + sz / 2, y, PC_GREY);
			} else {
				/* Wiese: die Form wechselt mit der Kachel - mal ein flacher
				 * Fleck, mal ein Bueschel, mal ein einzelner Halm. Immer
				 * dieselbe Form ergaebe ein Symbolmuster statt Bewuchs. */
				PixelColour tuft = ((h >> 3) & 1) ? PC_TREES : PC_RAINFOREST;
				int form = (h >> 5) & 3;
				int gw = std::max(1, sz / 2);
				if (form == 0) {
					GfxFillRect(x - gw, y - std::max(1, sz / 4), x + gw, y, tuft);
				} else if (form == 1) {
					GfxFillRect(x - std::max(1, sz / 5), y - sz, x + std::max(1, sz / 5), y, tuft);
				} else if (form == 2) {
					GfxFillRect(x - gw, y - std::max(1, sz / 5), x + gw, y, tuft);
					GfxFillRect(x - std::max(1, sz / 6), y - sz, x + std::max(1, sz / 6), y, tuft);
				} else {
					GfxFillRect(x - std::max(1, sz / 3), y - std::max(1, sz / 2), x + std::max(1, sz / 3), y, tuft);
				}
			}
		}
		(void)d;
	}

	/**
	 * Ein Stueck Bahnsteig laengs der Strecke. Ober- und Vorderseite
	 * werden zeilenweise gefuellt, damit sich die Kachelstuecke zu einer
	 * durchgehenden Kante fuegen statt als Klotz zu stehen.
	 */
	void DrawPlatform(const Rect &r, float d, int height, float lateral, bool left_side, float spread) const
	{
		float dn = d;
		float df = d + 1.0f;
		int y_near = this->GroundY(r, dn, height);
		int y_far = this->GroundY(r, df, height);
		if (y_near <= y_far) return;
		float t_near = 1.0f / (1.0f + dn * 0.42f);
		int lip = std::max(1, (int)(11 * t_near)); /* Hoehe der Bahnsteigkante */
		int side = left_side ? -1 : 1;

		int inner_far = 0, inner_near = 0, top_far = y_far, top_near = y_near;
		for (int y = y_far; y <= y_near; y++) {
			float f = (float)(y - y_far) / (float)std::max(1, y_near - y_far);
			float dd = df + (dn - df) * f;
			float hw = this->HalfWidth(r, dd);
			int cx = this->CenterX(r, dd, lateral);
			int inner = cx + side * (int)(hw * (spread - 0.9f));
			int outer = cx + side * (int)(hw * (spread + 1.1f));
			int top = y - (int)(lip * (0.4f + 0.6f * f));
			GfxFillRect(std::min(inner, outer), top, std::max(inner, outer), y, PC_GREY);
			if (y == y_far) { inner_far = inner; top_far = top; }
			inner_near = inner;
			top_near = top;
		}
		/* Die helle Bahnsteigkante gehoert EINMAL an den Rand. Pro Zeile
		 * gezeichnet ergaebe sie eine weisse Flaeche - genau das war sie
		 * vorher. */
		GfxDrawLine(inner_far, top_far, inner_near, top_near, PC_WHITE, std::max(1, (int)(3 * t_near)));
	}

	/**
	 * Ein Objekt neben der Strecke zeichnen - mit der ECHTEN Spielgrafik,
	 * damit man Haeuser und Waelder im Fuehrerstand wiedererkennt. Die
	 * Groesse kommt ueber die Zoomstufe (DrawSprite nimmt sie entgegen,
	 * DrawHouseInGUI ueber _gui_zoom).
	 */
	void DrawSideObject(const Rect &r, const CabObject &o, float d, bool left_side, bool flying, float spread, int height, float lateral) const
	{
		if (o.what == CabTile::Ground || o.what == CabTile::Water) return;
		/* Ganz nahe ist man schon vorbei, ganz fern nur noch Pixelmatsch. */
		if (d < 1.0f || d > 13.0f) return;
		float t = 1.0f / (1.0f + d * 0.42f);
		/* Die Sprites sollen auf dem Boden stehen, nicht darueber schweben. */
		int ground = this->GroundY(r, d, height) + (int)(8 * t);
		int cx = this->CenterX(r, d, lateral);
		float hw = this->HalfWidth(r, d);
		int x = left_side ? (int)(cx - hw * spread) : (int)(cx + hw * spread);
		ZoomLevel zoom = CabZoom(d, flying);

		switch (o.what) {
			case CabTile::House: {
				/* Das echte Haus-Sprite; _gui_zoom steuert hier die Groesse.
				 *
				 * ACHTUNG: DrawHouseInGUI setzt an (x,y) die OBERE Ecke der
				 * Bodenraute an, nicht den Fuss des Hauses. Ohne Korrektur
				 * sitzt das Gebaeude eine halbe Kachel zu tief und schleppt
				 * seine Bodenkachel als Platte durchs Bild. Wir heben es um
				 * die halbe Rautenhoehe an. */
				ZoomLevel hz = zoom;
				const HouseSpec *hs = HouseSpec::Get(o.house);
				/* Grosse Haeuser bestehen aus bis zu vier Kacheln und
				 * wuerden sonst alles ueberdecken - eine Stufe kleiner. */
				if (hs != nullptr && hs->building_flags.Any({BuildingFlag::Size2x2, BuildingFlag::Size2x1, BuildingFlag::Size1x2})) {
					hz = (ZoomLevel)std::min<int>((int)ZoomLevel::Out4x, (int)zoom + 1);
				}
				AutoRestoreBackup zoom_backup(_gui_zoom, hz);
				int half_tile = ScaleByZoom(TILE_PIXELS / 2, hz) / ZOOM_BASE;
				DrawHouseInGUI(x, ground - half_tile, o.house, o.view);
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
				/* Bahnsteig statt Kasten: ein Bahnhof besteht aus mehreren
				 * Kacheln, ein Klotz je Kachel ergab eine Reihe schwebender
				 * Platten. Jetzt zeichnet jede Kachel ein flaches Stueck
				 * Bahnsteig, das sich mit den Nachbarn zu einer Kante
				 * zusammensetzt. */
				this->DrawPlatform(r, d, height, lateral, left_side, spread);
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
	 * Wegrand-Details, die das Tempo spuerbar machen: Oberleitungsmasten
	 * an Gleisen, sonst Leitpfosten an der Strasse.
	 */
	void DrawWayside(const Rect &r, const Vehicle *v, int d, float depth, int height, float lateral) const
	{
		if (v->type != VehicleType::Train && v->type != VehicleType::Road) return;
		if (d % 2 != 0 || d > 12) return;
		float dd = depth;
		float t = 1.0f / (1.0f + dd * 0.42f);
		int ground = this->GroundY(r, dd, height);
		int cx = this->CenterX(r, dd, lateral);
		float hw = this->HalfWidth(r, dd);
		int post = (int)((v->type == VehicleType::Train ? 90 : 34) * t);
		if (post < 3) return;
		int w = std::max(1, (int)(4 * t));

		for (int side = 0; side < 2; side++) {
			int x = side == 0 ? cx - (int)(hw * 1.35f) : cx + (int)(hw * 1.35f);
			if (v->type == VehicleType::Train) {
				/* Mast mit kurzem Ausleger zum Gleis hin. */
				GfxFillRect(x - w, ground - post, x + w, ground, PC_DARK_GREY);
				int reach = (int)(hw * 0.35f);
				int dir = side == 0 ? 1 : -1;
				GfxFillRect(std::min(x, x + dir * reach), ground - post,
						std::max(x, x + dir * reach), ground - post + std::max(1, w / 2), PC_DARK_GREY);
			} else {
				/* Leitpfosten mit dunklem Kopf. */
				GfxFillRect(x - w, ground - post, x + w, ground, PC_WHITE);
				GfxFillRect(x - w, ground - post, x + w, ground - post + std::max(1, post / 4), PC_BLACK);
			}
		}
	}

	/** Tunnelportal: dunkle Oeffnung mit Rahmen, die auf einen zukommt. */
	void DrawTunnelMouth(const Rect &r, float d, int height, float lateral) const
	{
		float t = 1.0f / (1.0f + d * 0.42f);
		int ground = this->GroundY(r, d, height);
		int cx = this->CenterX(r, d, lateral);
		float hw = this->HalfWidth(r, d);
		int h = (int)(150 * t);
		int w = (int)(hw * 1.5f);
		if (h < 4 || w < 3) return;
		/* Portalmauer und dunkle Roehre. */
		GfxFillRect(cx - w - w / 4, ground - h - h / 6, cx + w + w / 4, ground, PC_GREY);
		GfxFillRect(cx - w, ground - h, cx + w, ground, PC_BLACK);
		GfxFillRect(cx - w - w / 4, ground - h - h / 6, cx + w + w / 4, ground - h - h / 6 + std::max(1, h / 20), PC_DARK_GREY);
	}

	/** Bruecke: Gelaender links und rechts der Fahrbahn. */
	void DrawBridgeRails(const Rect &r, float d, int height, float lateral) const
	{
		float t = 1.0f / (1.0f + d * 0.42f);
		int ground = this->GroundY(r, d, height);
		int cx = this->CenterX(r, d, lateral);
		float hw = this->HalfWidth(r, d);
		int h = (int)(28 * t);
		if (h < 2) return;
		int w = std::max(1, (int)(3 * t));
		for (int side = 0; side < 2; side++) {
			int x = side == 0 ? cx - (int)(hw * 1.1f) : cx + (int)(hw * 1.1f);
			GfxFillRect(x - w, ground - h, x + w, ground, PC_GREY);
			GfxFillRect(x - w * 2, ground - h, x + w * 2, ground - h + std::max(1, h / 4), PC_WHITE);
		}
	}

	/**
	 * Bahnhofsschild wie im Spiel: heller Kasten mit dem Stationsnamen,
	 * der beim Naeherkommen groesser wird.
	 */
	void DrawStationSign(const Rect &r, StationID id, float d, int height, float lateral) const
	{
		if (!Station::IsValidID(id) || d > 10.0f) return;
		float t = 1.0f / (1.0f + d * 0.42f);
		int ground = this->GroundY(r, d, height);
		int cx = this->CenterX(r, d, lateral);
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
	void DrawSignal(const Rect &r, bool red, float d, int height, float lateral) const
	{
		float t = 1.0f / (1.0f + d * 0.42f);
		int ground = this->GroundY(r, d, height);
		int cx = this->CenterX(r, d, lateral);
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
		for (int i = 0; i < 6; i++) {
			/* Feste Startpunkte, damit die Wolken nicht flackern. */
			int cw = w / (7 + i);
			int ch = std::max(3, sky / (12 + i));
			int cx = r.left + ((i * 211 + (int)(this->anim / 10)) % (w + cw * 2)) - cw;
			int cy = r.top + sky / 8 + (i * sky) / 11;
			if (cy + ch > this->horizon - sky / 10) continue;
			/* Weicher Rand: Kern voll, Saum als Schachbrett. */
			GfxFillRect(cx, cy, cx + cw, cy + ch, PC_WHITE);
			GfxFillRect(cx + cw / 5, cy - ch / 2, cx + cw - cw / 5, cy, PC_WHITE);
			GfxFillRect(cx - cw / 6, cy, cx + cw + cw / 6, cy + ch + ch / 2, PC_WHITE, FillRectMode::Checker);
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

		/* Scheibenecken abschraegen: eine rechteckige Oeffnung sieht aus
		 * wie ein Bilderrahmen, eine abgerundete wie eine Windschutz-
		 * scheibe. Zeilenweise, damit die Schraege sauber laeuft. */
		int corner = std::max(5, w / 20);
		for (int i = 0; i < corner; i++) {
			int cut = corner - i;
			int y = r.top + strut / 2 + i;
			GfxFillRect(r.left + strut, y, r.left + strut + cut, y, body);
			GfxFillRect(r.right - strut - cut, y, r.right - strut, y, body);
		}

		/* Pult am unteren Rand, leicht nach vorn gewoelbt. */
		int desk = r.bottom - h / 5;
		GfxFillRect(r.left, desk, r.right, r.bottom, body);
		/* Kante: heller Grat oben, dunkler Schatten darunter - das gibt
		 * dem Pult Tiefe statt einer flachen Flaeche. */
		GfxFillRect(r.left, desk, r.right, desk + std::max(2, h / 160), PC_GREY);
		GfxFillRect(r.left, desk + std::max(2, h / 160), r.right, desk + std::max(4, h / 70), PC_BLACK);
		/* Griffmulden im Pult, damit es nicht leer wirkt. */
		int slot_h = std::max(2, h / 90);
		for (int k = 1; k <= 3; k++) {
			int sx = r.left + strut + w * k / 6;
			GfxFillRect(sx, r.bottom - h / 14, sx + w / 40, r.bottom - h / 14 + slot_h, PC_BLACK);
		}

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

		/* Himmel: von oben dunkel nach unten hell. Die Palette hat nur
		 * wenige Blautoene - die Zwischenstufen entstehen durch ein
		 * Schachbrett aus zwei Nachbartoenen, sonst stehen dort Balken. */
		{
			int sky_h = std::max(1, this->horizon - r.top);
			static const PixelColour sky[] = {PC_DARK_BLUE, PC_DARK_BLUE, PC_LIGHT_BLUE, PC_LIGHT_BLUE, PC_LIGHT_BLUE};
			int bands = lengthof(sky) - 1;
			for (int i = 0; i < bands; i++) {
				int y0 = r.top + sky_h * i / bands;
				int y1 = r.top + sky_h * (i + 1) / bands;
				GfxFillRect(r.left, y0, r.right, y1, sky[i]);
				/* Untere Haelfte des Bandes zum naechsten Ton hin auflösen. */
				int mid = (y0 + y1) / 2;
				GfxFillRect(r.left, mid, r.right, y1, sky[i + 1], FillRectMode::Checker);
			}
			/* Dunstband direkt ueber dem Horizont - laesst die Ferne
			 * zurueckweichen, statt hart abzuschneiden. */
			int haze = std::max(2, sky_h / 12);
			GfxFillRect(r.left, this->horizon - haze, r.right, this->horizon, PC_WHITE, FillRectMode::Checker);

			/* Sonne: fester Platz, damit sie nicht mitwandert. */
			int sun_r = std::max(3, sky_h / 14);
			int sun_x = r.left + (r.right - r.left) * 3 / 4;
			int sun_y = r.top + sky_h / 3;
			for (int dy = -sun_r; dy <= sun_r; dy++) {
				int dx = (int)(std::sqrt((float)(sun_r * sun_r - dy * dy)));
				GfxFillRect(sun_x - dx, sun_y + dy, sun_x + dx, sun_y + dy, PC_YELLOW);
			}
			for (int dy = -sun_r / 2; dy <= sun_r / 2; dy++) {
				int rr = sun_r / 2;
				int dx = (int)(std::sqrt((float)std::max(0, rr * rr - dy * dy)));
				GfxFillRect(sun_x - dx, sun_y + dy, sun_x + dx, sun_y + dy, PC_WHITE);
			}

		}
		if (v == nullptr) return;

		/* Zwischenposition in der Kachel bestimmen (Weltkoordinaten sind
		 * 1/16 Kachel): daraus fliesst die Landschaft gleichmaessig. */
		{
			DiagDirection fwd = DirToDiagDir(v->direction);
			int fx = v->x_pos & 0x0F;
			int fy = v->y_pos & 0x0F;
			float s = 0.0f;
			switch (fwd) {
				case DiagDirection::NE: s = (15 - fx) / 16.0f; break; /* -x */
				case DiagDirection::SW: s = fx / 16.0f; break;        /* +x */
				case DiagDirection::NW: s = (15 - fy) / 16.0f; break; /* -y */
				default:                s = fy / 16.0f; break;        /* +y */
			}
			const_cast<CabViewWindow *>(this)->sub = s;
		}

		std::vector<CabSample> scan = this->Scan(v);

		/* Boden von hinten nach vorn: jede Kachel ein Streifen. */
		for (int d = CAB_DEPTH - 1; d >= 0; d--) {
			const CabSample &s = scan[d];
			if (!s.valid) continue;
			const CabSample &next = (d + 1 < CAB_DEPTH) ? scan[d + 1] : s;
			float dn = this->Depth((float)d);
			float df = this->Depth((float)d + 1);
			int y_far = this->GroundY(r, df, next.height);
			int y_near = this->GroundY(r, dn, s.height);
			if (y_near <= y_far) continue;
			/* In der Kurve wandert die Streckenmitte zur Seite. */
			int cx = this->CenterX(r, dn, s.lateral);
			int cx_far = this->CenterX(r, df, next.lateral);
			PixelColour col = s.ground;
			PixelColour col_l = s.left[0].ground;
			PixelColour col_r = s.right[0].ground;
			if (d > 14) { col = PC_ROUGH_LAND; col_l = PC_ROUGH_LAND; col_r = PC_ROUGH_LAND; }
			/* Die Baender laufen perspektivisch zusammen - zeilenweise
			 * gezeichnet, sonst stehen dort harte Kloetze statt Landschaft. */
			float band_near = this->HalfWidth(r, dn) * 3.0f;
			float band_far = this->HalfWidth(r, df) * 3.0f;
			/* Die oberen Zeilen bekommen die Farbe der dahinterliegenden
			 * Kachel als Schachbrett daruebergelegt: so verlaufen die
			 * Farben ineinander statt als harte Streifenkanten zu stehen. */
			PixelColour far_col = (d > 14) ? PC_ROUGH_LAND : next.ground;
			PixelColour far_l = (d > 14) ? PC_ROUGH_LAND : next.left[0].ground;
			PixelColour far_r = (d > 14) ? PC_ROUGH_LAND : next.right[0].ground;
			int blend = std::max(1, (y_near - y_far) / 3);
			for (int y = y_far; y <= y_near; y++) {
				float f = (y_near == y_far) ? 1.0f : (float)(y - y_far) / (float)(y_near - y_far);
				int mx = cx_far + (int)((cx - cx_far) * f);
				int bw = (int)(band_far + (band_near - band_far) * f);
				GfxFillRect(r.left, y, mx - bw, y, col_l);
				GfxFillRect(mx - bw, y, mx + bw, y, col);
				GfxFillRect(mx + bw, y, r.right, y, col_r);
				if (y < y_far + blend) {
					GfxFillRect(r.left, y, mx - bw, y, far_l, FillRectMode::Checker);
					GfxFillRect(mx - bw, y, mx + bw, y, far_col, FillRectMode::Checker);
					GfxFillRect(mx + bw, y, r.right, y, far_r, FillRectMode::Checker);
				}
			}

			bool flying_view = v->type == VehicleType::Aircraft;
			/* Struktur auf die Flaeche: Halme, Furchen, Wellen. */
			if (!flying_view) this->DrawGroundTexture(r, s, d, dn);
			/* Aeussere Reihen zuerst, damit die naeheren sie ueberdecken. */
			for (int row = CAB_SIDE_ROWS - 1; row >= 0; row--) {
				float spread = 2.0f + row * 1.5f;
				this->DrawSideObject(r, s.left[row], dn, true, flying_view, spread, s.height, s.lateral);
				this->DrawSideObject(r, s.right[row], dn, false, flying_view, spread, s.height, s.lateral);
			}
			this->DrawWayside(r, v, d, dn, s.height, s.lateral);
			if (s.on_bridge) this->DrawBridgeRails(r, dn, s.height, s.lateral);
			if (s.tunnel_mouth) this->DrawTunnelMouth(r, dn, s.height, s.lateral);
			if (s.signal) this->DrawSignal(r, s.signal_red, dn, s.height, s.lateral);
			if (s.station != StationID::Invalid()) this->DrawStationSign(r, s.station, dn, s.height, s.lateral);

			/* Fahrweg: Bahndamm, Fahrbahn oder Fahrrinne. Flugzeuge haben
			 * keinen - dort bleibt die Landschaft, ueber die man fliegt. */
			float hw_far = this->HalfWidth(r, df);
			float hw_near = this->HalfWidth(r, dn);
			bool rail = v->type == VehicleType::Train;
			bool flying = v->type == VehicleType::Aircraft;
			if (!flying) {
				PixelColour bed = rail ? PC_BARE_LAND : PC_DARK_GREY;
				if (v->type == VehicleType::Ship) bed = PC_WATER;
				/* Trapez als Zeilen fuellen, damit die Kanten zusammenlaufen. */
				for (int y = y_far; y <= y_near; y++) {
					float f = (y_near == y_far) ? 0.0f : (float)(y - y_far) / (float)(y_near - y_far);
					float hw = hw_far + (hw_near - hw_far) * f;
					int mx = cx_far + (int)((cx - cx_far) * f);
					GfxFillRect(mx - (int)hw, y, mx + (int)hw, y, bed);
				}
			}
			/* Schienen bzw. Mittelstreifen. */
			if (rail) {
				GfxDrawLine(cx_far - (int)(hw_far * 0.6f), y_far, cx - (int)(hw_near * 0.6f), y_near, PC_GREY, std::max(1, (int)(hw_near / 12)));
				GfxDrawLine(cx_far + (int)(hw_far * 0.6f), y_far, cx + (int)(hw_near * 0.6f), y_near, PC_GREY, std::max(1, (int)(hw_near / 12)));
				/* Schwellen wandern mit dem Tempo auf den Betrachter zu -
				 * mehrere je Kachel, sonst fehlt der Bewegungseindruck. */
				int span = std::max(1, y_near - y_far);
				for (int k = 0; k < 4; k++) {
					int off = (int)(((this->anim % 24) + k * 6) % 24);
					int sleeper = y_near - off * span / 24;
					if (sleeper <= y_far || sleeper >= y_near) continue;
					float f = (float)(sleeper - y_far) / (float)span;
					float hw = hw_far + (hw_near - hw_far) * f;
					int mx = cx_far + (int)((cx - cx_far) * f);
					GfxFillRect(mx - (int)(hw * 0.78f), sleeper, mx + (int)(hw * 0.78f),
							sleeper + std::max(1, (int)(hw / 18)), PC_DARK_GREY);
				}
			} else if (v->type == VehicleType::Road) {
				/* Fahrbahnrand: heller Saum links und rechts. */
				for (int y = y_far; y <= y_near; y++) {
					float f = (y_near == y_far) ? 0.0f : (float)(y - y_far) / (float)(y_near - y_far);
					float hw = hw_far + (hw_near - hw_far) * f;
					int mx = cx_far + (int)((cx - cx_far) * f);
					int lw = std::max(1, (int)(hw / 22));
					GfxFillRect(mx - (int)hw, y, mx - (int)hw + lw, y, PC_GREY);
					GfxFillRect(mx + (int)hw - lw, y, mx + (int)hw, y, PC_GREY);
				}
				/* Mittelstreifen: mehrere Striche je Kachel, sonst wirkt die
				 * Fahrbahn leer und man sieht keine Bewegung. */
				int span = std::max(1, y_near - y_far);
				for (int k = 0; k < 3; k++) {
					int off = (int)(((this->anim % 20) + k * 20 / 3) % 20);
					int dash = y_near - off * span / 20;
					if (dash <= y_far || dash >= y_near) continue;
					float f = (float)(dash - y_far) / (float)span;
					float hw = hw_far + (hw_near - hw_far) * f;
					int mx = cx_far + (int)((cx - cx_far) * f);
					int dw = std::max(1, (int)(hw / 16));
					int dh = std::max(1, (int)(hw / 7));
					GfxFillRect(mx - dw, dash, mx + dw, std::min(dash + dh, y_near), PC_WHITE);
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

		this->DrawClouds(r);
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

	/**
	 * Bild bei JEDEM Frame neu zeichnen - ein fester Timer waere gegenueber
	 * der Bildrate versetzt und laesst die Fahrt stocken.
	 */
	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		const Vehicle *v = this->Cab();
		if (v == nullptr) {
			this->Close();
			return;
		}
		/* Schwellen und Streifen laufen mit dem Tempo, unabhaengig von der
		 * Bildrate (delta_ms), damit die Bewegung gleichmaessig bleibt. */
		this->anim += std::max(1u, (uint)(v->GetDisplaySpeed() * std::max(1u, delta_ms) / 300));
		if (this->width != _screen.width || this->height != _screen.height) this->FitToScreen();
		this->SetDirty();
	}
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
