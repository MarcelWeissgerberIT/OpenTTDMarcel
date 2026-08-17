/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file citizen.cpp Stadtleben 3.0 (Fork-Feature).
 *
 * Buerger sind reine Atmosphaere: sie kommen aus Haeusern, laufen ueber die
 * Stadtstrassen zu einem Ziel (Bahnhof, Besuch, Einkauf) und verschwinden
 * dort wieder in einem Haus. Zivilautos fahren aus einer Parkluecke los und
 * kehren dorthin zurueck. Jede Figur ist anklickbar und zeigt ein kleines
 * Info-Fenster. Nichts davon wird im Spielstand gespeichert - nach dem Laden
 * bevoelkert sich die Stadt binnen Sekunden neu.
 */

#include "stdafx.h"
#include "citizen.h"
#include "landscape.h"
#include "road_map.h"
#include "bridge_map.h"
#include "road_func.h"
#include "town.h"
#include "town_map.h"
#include "station_map.h"
#include "tile_map.h"
#include "map_func.h"
#include "viewport_func.h"
#include "window_gui.h"
#include "window_func.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "timer/timer_game_tick.h"
#include "table/sprites.h"
#include "table/strings.h"

#include <map>
#include <queue>

#include "safeguards.h"

std::vector<Citizen> _citizens;
static uint32_t _citizen_next_id = 1;
static uint32_t _citizen_rng = 0x2A5F1E3Bu;

/* Eigener Zufallsgenerator, damit der Spiel-RNG unberuehrt bleibt. */
static uint32_t CitizenRandom()
{
	_citizen_rng ^= _citizen_rng << 13;
	_citizen_rng ^= _citizen_rng >> 17;
	_citizen_rng ^= _citizen_rng << 5;
	return _citizen_rng;
}

static const char *const _citizen_first_names[] = {
	"Lukas", "Finn", "Jonas", "Paul", "Leon", "Max", "Felix", "Emil",
	"Anton", "Karl", "Otto", "Marcel", "Mia", "Emma", "Lina", "Anna",
	"Lea", "Marie", "Clara", "Ida", "Frieda", "Greta", "Ella", "Sophie",
};

static const char *const _citizen_surnames[] = {
	"Müller", "Schmidt", "Schneider", "Fischer", "Weber", "Meyer",
	"Wagner", "Becker", "Hoffmann", "Koch", "Richter", "Bauer",
	"Weißgerber", "Krüger", "Vogel", "Braun",
};

static const char *const _citizen_interests[] = {
	"Eisenbahnen", "Fußball", "Gärtnern", "Angeln", "Musik",
	"Backen", "Briefmarken", "Wandern", "Malen", "Schach", "Kino", "Tanzen",
};

/* Name/Alter/Interesse deterministisch aus der ID ableiten. */
static uint32_t CitizenHash(uint32_t id, uint32_t salt)
{
	uint32_t h = id * 2654435761u + salt * 40503u;
	h ^= h >> 15;
	h *= 2246822519u;
	h ^= h >> 13;
	return h;
}

static std::string CitizenName(const Citizen &c)
{
	const char *first = _citizen_first_names[CitizenHash(c.id, 1) % lengthof(_citizen_first_names)];
	const char *last = _citizen_surnames[CitizenHash(c.id, 2) % lengthof(_citizen_surnames)];
	return std::string(first) + " " + last;
}

static uint CitizenAge(const Citizen &c)
{
	if (c.kind == CitizenKind::Child) return 4 + CitizenHash(c.id, 3) % 9;
	return 18 + CitizenHash(c.id, 3) % 62;
}

static const char *CitizenInterest(const Citizen &c)
{
	return _citizen_interests[CitizenHash(c.id, 4) % lengthof(_citizen_interests)];
}

/* ---------- Strassennetz ---------- */

static bool IsWalkableRoad(TileIndex tile)
{
	return IsNormalRoadTile(tile) && GetTileSlope(tile) == SLOPE_FLAT && !IsBridgeAbove(tile);
}

static bool RoadConnected(TileIndex from, DiagDirection d, TileIndex &neighbour)
{
	if (!(GetRoadBits(from, RoadTramType::Road) & DiagDirToRoadBits(d)).Any()) return false;
	TileIndex n = AddTileIndexDiffCWrap(from, TileIndexDiffCByDiagDir(d));
	if (n == INVALID_TILE || !IsWalkableRoad(n)) return false;
	if (!(GetRoadBits(n, RoadTramType::Road) & DiagDirToRoadBits(ReverseDiagDir(d))).Any()) return false;
	neighbour = n;
	return true;
}

/**
 * Breitensuche ueber das Strassennetz ab @p start (max. @p limit Kacheln).
 * @return Karte Kachel -> Vorgaenger (start zeigt auf sich selbst).
 */
static std::map<TileIndex, TileIndex> WalkNetwork(TileIndex start, uint limit)
{
	std::map<TileIndex, TileIndex> prev;
	std::queue<TileIndex> open;
	prev[start] = start;
	open.push(start);
	while (!open.empty() && prev.size() < limit) {
		TileIndex t = open.front();
		open.pop();
		for (DiagDirection d = DiagDirection::Begin; d < DiagDirection::End; d++) {
			TileIndex n;
			if (!RoadConnected(t, d, n)) continue;
			if (prev.count(n) != 0) continue;
			prev[n] = t;
			open.push(n);
		}
	}
	return prev;
}

static std::vector<TileIndex> TracePath(const std::map<TileIndex, TileIndex> &prev, TileIndex goal)
{
	std::vector<TileIndex> path;
	TileIndex t = goal;
	while (true) {
		path.push_back(t);
		TileIndex p = prev.at(t);
		if (p == t) break;
		t = p;
	}
	std::reverse(path.begin(), path.end());
	return path;
}

static bool HasAdjacentTileType(TileIndex tile, TileType type)
{
	for (DiagDirection d = DiagDirection::Begin; d < DiagDirection::End; d++) {
		TileIndex n = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDiagDir(d));
		if (n != INVALID_TILE && IsTileType(n, type)) return true;
	}
	return false;
}

/* Gleiche Bedingung wie die Parkluecken-Deko in road_cmd.cpp. */
static bool IsParkingSpotTile(TileIndex tile)
{
	if (!IsWalkableRoad(tile)) return false;
	if (GetRoadOwner(tile, RoadTramType::Road) != OWNER_TOWN) return false;
	if (GetRoadBits(tile, RoadTramType::Road) != ROAD_X) return false;
	uint32_t h = TileX(tile) * 7919u ^ TileY(tile) * 104729u;
	return (h & 7) == 0;
}

/* ---------- Spawnen ---------- */

static const uint MAX_CITIZENS = 800;
static const uint NETWORK_LIMIT = 90;

/** Zufaellige Strassenkachel mit Haus daneben im Umkreis der Stadt. */
static TileIndex FindSpawnRoad(const Town *t, bool parking)
{
	for (uint attempt = 0; attempt < 24; attempt++) {
		int dx = (int)(CitizenRandom() % 25) - 12;
		int dy = (int)(CitizenRandom() % 25) - 12;
		TileIndex tile = AddTileIndexDiffCWrap(t->xy, TileIndexDiffC{(int16_t)dx, (int16_t)dy});
		if (tile == INVALID_TILE) continue;
		if (parking) {
			if (IsParkingSpotTile(tile) && !IsParkedCarAway(tile)) return tile;
		} else {
			if (IsWalkableRoad(tile) && HasAdjacentTileType(tile, TileType::House)) return tile;
		}
	}
	return INVALID_TILE;
}

static void SpawnCitizen(Town *t)
{
	bool car = (CitizenRandom() % 8) == 0;
	TileIndex start = FindSpawnRoad(t, car);
	if (start == INVALID_TILE) return;

	auto net = WalkNetwork(start, NETWORK_LIMIT);
	if (net.size() < 6) return;

	Citizen c;
	c.id = _citizen_next_id++;
	c.town = t->index;
	c.home = start;
	c.pos = 0;
	c.sub = 8; /* Startet mitten auf der Kachel ("kommt aus dem Haus"). */

	if (car) {
		/* Rundfahrt: zum entferntesten erreichbaren Punkt und zurueck in
		 * dieselbe Parkluecke - so wird das geparkte Auto sichtbar benutzt. */
		TileIndex far = start;
		uint best = 0;
		for (const auto &[tile, from] : net) {
			uint d = DistanceManhattan(start, tile);
			if (d > best) { best = d; far = tile; }
		}
		if (best < 6) return;
		std::vector<TileIndex> out = TracePath(net, far);
		c.path = out;
		for (size_t i = out.size() - 1; i-- > 0; ) c.path.push_back(out[i]);
		c.kind = CitizenKind::Car;
		c.goal = CitizenGoal::Drive;
	} else {
		/* Ziel: Kachel neben Station (Bahnhof/Haltestelle) oder neben einem
		 * anderen Haus - dort verschwindet die Figur wieder ("geht hinein"). */
		TileIndex goal = INVALID_TILE;
		bool to_station = (CitizenRandom() % 5) < 2;
		if (to_station) {
			for (const auto &[tile, from] : net) {
				if (DistanceManhattan(start, tile) >= 3 && HasAdjacentTileType(tile, TileType::Station)) { goal = tile; break; }
			}
		}
		if (goal == INVALID_TILE) {
			to_station = false;
			uint want = 5 + CitizenRandom() % 12;
			for (const auto &[tile, from] : net) {
				if (DistanceManhattan(start, tile) >= want && HasAdjacentTileType(tile, TileType::House)) { goal = tile; break; }
			}
		}
		if (goal == INVALID_TILE) return;
		c.path = TracePath(net, goal);
		uint k = CitizenRandom() % 10;
		c.kind = k < 5 ? CitizenKind::Adult : k < 7 ? CitizenKind::Family : k < 9 ? CitizenKind::Child : CitizenKind::Stroller;
		c.goal = to_station ? CitizenGoal::Station
			: (CitizenRandom() % 3 == 0 ? CitizenGoal::Home : (CitizenRandom() % 2 == 0 ? CitizenGoal::Visit : CitizenGoal::Shopping));
	}

	if (c.path.size() < 2) return;
	_citizens.push_back(std::move(c));
}

/* ---------- Simulation ---------- */

bool IsParkedCarAway(TileIndex tile)
{
	for (const Citizen &c : _citizens) {
		if (c.kind == CitizenKind::Car && c.home == tile) return true;
	}
	return false;
}

static void CitizenStep(Citizen &c)
{
	MarkTileDirtyByTile(c.path[c.pos]);
	if (c.sub < 15) {
		c.sub++;
		return;
	}
	c.sub = 0;
	c.pos++;
	if (c.pos < c.path.size()) MarkTileDirtyByTile(c.path[c.pos]);
}

void RunCitizensTick()
{
	uint64_t tick = TimerGameTick::counter;

	/* Bewegung: Fussgaenger jeden 3. Tick, Autos jeden Tick. */
	for (Citizen &c : _citizens) {
		if (c.kind == CitizenKind::Car || tick % 3 == 0) CitizenStep(c);
	}

	/* Angekommene entfernen (Fenster dazu schliessen). */
	for (auto it = _citizens.begin(); it != _citizens.end(); ) {
		if (it->pos >= it->path.size() || (it->pos == it->path.size() - 1 && it->sub >= 8 && it->kind != CitizenKind::Car)) {
			CloseWindowById(WindowClass::Citizen, it->id);
			it = _citizens.erase(it);
		} else {
			++it;
		}
	}

		/* Nachschub: alle 6 Ticks eine zufaellige Stadt betrachten, damit sich
	 * die Welt nach Spielstart/Laden zuegig fuellt. */
	if (tick % 2 != 0 || _citizens.size() >= MAX_CITIZENS) return;
	uint town_count = 0;
	for ([[maybe_unused]] const Town *t : Town::Iterate()) town_count++;
	if (town_count == 0) return;
	uint pick = CitizenRandom() % town_count;
	for (Town *t : Town::Iterate()) {
		if (pick-- != 0) continue;
		uint here = 0;
		for (const Citizen &c : _citizens) {
			if (c.town == t->index) here++;
		}
		uint want = ClampU(t->cache.population / 25, 4, 60);
		if (here < want) SpawnCitizen(t);
		break;
	}
}

void ClearCitizens()
{
	_citizens.clear();
	_citizen_rng ^= (uint32_t)TimerGameTick::counter | 1;
}

/* ---------- Darstellung ---------- */

/** Position der Figur innerhalb ihrer aktuellen Kachel (0..15). */
static void CitizenTilePos(const Citizen &c, int &px, int &py, DiagDirection &dir)
{
	TileIndex cur = c.path[c.pos];
	TileIndex next = ((size_t)c.pos + 1 < c.path.size()) ? c.path[c.pos + 1] : cur;
	dir = DiagDirection::NE;
	if (next != cur) {
		if (TileX(next) < TileX(cur)) dir = DiagDirection::NE;
		else if (TileX(next) > TileX(cur)) dir = DiagDirection::SW;
		else if (TileY(next) < TileY(cur)) dir = DiagDirection::NW;
		else dir = DiagDirection::SE;
	}
	bool is_car = c.kind == CitizenKind::Car;
	int along = c.sub;
	int side_ped = (CitizenHash(c.id, 5) & 1) != 0 ? 3 : 12;
	switch (dir) {
		case DiagDirection::NE: px = 15 - along; py = is_car ? 10 : side_ped; break;
		case DiagDirection::SW: px = along;      py = is_car ? 5  : side_ped; break;
		case DiagDirection::NW: py = 15 - along; px = is_car ? 5  : side_ped; break;
		default:                py = along;      px = is_car ? 10 : side_ped; break;
	}
	px = Clamp(px, 0, 13);
	py = Clamp(py, 0, 13);
}

static SpriteID CitizenSprite(const Citizen &c, DiagDirection dir)
{
	switch (c.kind) {
		case CitizenKind::Car:
			return (dir == DiagDirection::NW || dir == DiagDirection::SE) ? SPR_CIV_CAR_Y : SPR_PARKED_CAR;
		case CitizenKind::Family: return SPR_CITIZEN_FAMILY;
		case CitizenKind::Child: return SPR_CITIZEN_CHILD;
		case CitizenKind::Stroller: return SPR_CITIZEN_STROLLER;
		default: return ((c.sub >> 2) & 1) != 0 ? SPR_CITIZEN_WALK_1 : SPR_CITIZEN_WALK_2;
	}
}

void DrawCitizensOnTile(const TileInfo *ti)
{
	for (const Citizen &c : _citizens) {
		if (c.path[c.pos] != ti->tile) continue;
		int px, py;
		DiagDirection dir;
		CitizenTilePos(c, px, py, dir);
		AddSortableSpriteToDraw(CitizenSprite(c, dir), PAL_NONE, *ti,
				{{(int8_t)px, (int8_t)py, 0}, {3, 3, 6}, {}});
	}
}

/* ---------- Klick + Fenster ---------- */

static Citizen *FindCitizenById(uint32_t id)
{
	for (Citizen &c : _citizens) {
		if (c.id == id) return &c;
	}
	return nullptr;
}

/** Widgets des Buerger-Fensters. */
enum CitizenWidgets : WidgetID {
	WID_CZ_CAPTION, ///< Titel mit Namen.
	WID_CZ_PANEL,   ///< Textzeilen.
};

struct CitizenWindow : Window {
	uint32_t citizen_id;

	CitizenWindow(WindowDesc &desc, WindowNumber number) : Window(desc), citizen_id((uint32_t)number)
	{
		this->InitNested(number);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_CZ_CAPTION) {
			const Citizen *c = FindCitizenById(this->citizen_id);
			if (c != nullptr) return GetString(STR_CITIZEN_CAPTION, CitizenName(*c));
		}
		return this->Window::GetWidgetString(widget, stringid);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_CZ_PANEL) return;
		const Citizen *c = FindCitizenById(this->citizen_id);
		if (c == nullptr) return;
		Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);
		int line = GetCharacterHeight(FontSize::Normal) + 2;
		int y = tr.top;
		DrawString(tr.left, tr.right, y, GetString(STR_CITIZEN_AGE, CitizenAge(*c)));
		y += line;
		DrawString(tr.left, tr.right, y, GetString(STR_CITIZEN_INTEREST, std::string(CitizenInterest(*c))));
		y += line;
		DrawString(tr.left, tr.right, y, GetString(STR_CITIZEN_HOME, c->town));
		y += line;
		StringID goal;
		switch (c->goal) {
			case CitizenGoal::Station: goal = STR_CITIZEN_GOAL_STATION; break;
			case CitizenGoal::Visit: goal = STR_CITIZEN_GOAL_VISIT; break;
			case CitizenGoal::Home: goal = STR_CITIZEN_GOAL_HOME; break;
			case CitizenGoal::Shopping: goal = STR_CITIZEN_GOAL_SHOPPING; break;
			default: goal = STR_CITIZEN_GOAL_DRIVE; break;
		}
		DrawString(tr.left, tr.right, y, GetString(goal));
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_CZ_PANEL) return;
		size.width = std::max<uint>(size.width, 190);
		size.height = std::max<uint>(size.height, 4 * (GetCharacterHeight(FontSize::Normal) + 2) + 8);
	}

	/* Die Figur laeuft weiter - Fenster regelmaessig nachzeichnen. */
	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_citizen_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Brown),
		NWidget(WWT_CAPTION, Colours::Brown, WID_CZ_CAPTION), SetStringTip(STR_CITIZEN_CAPTION),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Brown, WID_CZ_PANEL), SetMinimalSize(190, 60),
	EndContainer(),
};

static WindowDesc _citizen_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::Citizen, WindowClass::None,
	{},
	_nested_citizen_widgets
);

static void ShowCitizenWindow(uint32_t id)
{
	AllocateWindowDescFront<CitizenWindow>(_citizen_desc, id);
}

bool CheckClickOnCitizen(int world_x, int world_y)
{
	const Citizen *best = nullptr;
	uint best_d = 11; /* Fangradius in Weltkoordinaten. */
	for (const Citizen &c : _citizens) {
		TileIndex tile = c.path[c.pos];
		int px, py;
		DiagDirection dir;
		CitizenTilePos(c, px, py, dir);
		uint d = abs((int)(TileX(tile) * TILE_SIZE + px) - world_x) + abs((int)(TileY(tile) * TILE_SIZE + py) - world_y);
		if (d < best_d) { best_d = d; best = &c; }
	}
	if (best == nullptr) return false;
	ShowCitizenWindow(best->id);
	return true;
}
