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
#include "company_manager_face.h"
#include "core/random_func.hpp"
#include "window_func.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "timer/timer_game_tick.h"
#include "timer/timer_game_calendar.h"
#include "table/sprites.h"
#include "table/strings.h"

#include <map>
#include <set>
#include <unordered_map>
#include <queue>

#include "safeguards.h"

std::vector<Citizen> _citizens;
/* Beschleunigung fuers Zeichnen: Kachel -> Buerger-Indizes, je Tick neu. */
static std::unordered_multimap<uint32_t, uint32_t> _citizens_by_tile;
static std::set<TileIndex> _cars_away;
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

/* Kleidungs-/Autofarbe: eine der 16 Spielfarben, stabil pro Person. */
static PaletteID CitizenPalette(uint32_t id)
{
	return PALETTE_RECOLOUR_START + CitizenHash(id, 8) % 16;
}

/* Portraet deterministisch aus der ID - dieselbe Person, dasselbe Gesicht. */
static CompanyManagerFace CitizenFace(const Citizen &c)
{
	CompanyManagerFace cmf;
	Randomizer r;
	r.SetSeed(CitizenHash(c.id, 6) | 1);
	RandomiseCompanyManagerFace(cmf, r);
	return cmf;
}

/* ---------- Monats- und Jahres-Intentionen ----------
 * Tagesablaeufe waeren bei ~2 s Echtzeit pro Spieltag unsichtbar; darum
 * bekommt jeder Bewohner deterministisch (ID + Kalender) einen
 * Monats-Schwerpunkt und einen Jahres-Vorsatz, die sein Verhalten steuern. */

enum class MonthIntention : uint8_t { Commute, Shopping, Social, Outdoor, Homebody };
enum class YearIntention : uint8_t { Sport, Explore, Travel, Move, Friends };

static MonthIntention GetMonthIntention(const Citizen &c)
{
	uint32_t stamp = (uint32_t)TimerGameCalendar::year.base() * 12u + TimerGameCalendar::month;
	return (MonthIntention)(CitizenHash(c.id, 7000u + stamp) % 5);
}

static YearIntention GetYearIntention(const Citizen &c)
{
	return (YearIntention)(CitizenHash(c.id, 9000u + (uint32_t)TimerGameCalendar::year.base()) % 5);
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

static const uint MAX_CITIZENS = 2500;
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

/**
 * Plant den naechsten Fussweg ab @p from: Bahnhof, Besuch, Einkauf oder
 * Bummel-Etappe. Setzt Pfad, Ziel und Bummel-Zaehler des Buergers.
 * @return false, wenn ab hier kein Weg gefunden wurde.
 */
static bool PlanOuting(Citizen &c, TileIndex from)
{
	auto net = WalkNetwork(from, NETWORK_LIMIT);
	if (net.size() < 4) return false;

	/* Gewichte nach Monats-Schwerpunkt und Jahres-Vorsatz. */
	MonthIntention mi = GetMonthIntention(c);
	YearIntention yi = GetYearIntention(c);
	uint p_station = 25, p_stroll = 20;
	switch (mi) {
		case MonthIntention::Commute: p_station = 55; break;
		case MonthIntention::Shopping: p_station = 10; p_stroll = 10; break;
		case MonthIntention::Social: p_station = 10; p_stroll = 10; break;
		case MonthIntention::Outdoor: p_stroll = 55; break;
		case MonthIntention::Homebody: p_station = 15; p_stroll = 10; break;
	}
	if (yi == YearIntention::Sport || yi == YearIntention::Explore) p_stroll += 15;
	if (yi == YearIntention::Travel) p_station += 15;

	/* Jahres-Vorsatz Umzug: gelegentlich wirklich das Zuhause wechseln. */
	if (yi == YearIntention::Move && c.kind != CitizenKind::Car && from == c.home && CitizenRandom() % 6 == 0) {
		TileIndex goal = INVALID_TILE;
		uint want = 10 + CitizenRandom() % 15;
		for (const auto &[tile, prev] : net) {
			if (DistanceManhattan(from, tile) >= want && HasAdjacentTileType(tile, TileType::House)) { goal = tile; break; }
		}
		if (goal != INVALID_TILE) {
			c.path = TracePath(net, goal);
			c.pos = 0;
			c.sub = 0;
			c.goal = CitizenGoal::Move;
			return c.path.size() >= 2;
		}
	}

	TileIndex goal = INVALID_TILE;
	uint roll = CitizenRandom() % 100;
	if (roll < p_station) {
		for (const auto &[tile, prev] : net) {
			if (DistanceManhattan(from, tile) >= 3 && HasAdjacentTileType(tile, TileType::Station)) { goal = tile; break; }
		}
		if (goal != INVALID_TILE) c.goal = CitizenGoal::Station;
	}
	if (goal == INVALID_TILE && roll >= 100 - p_stroll) {
		/* Bummeln: beliebige entferntere Kachel, mehrere Etappen. */
		uint want = 6 + CitizenRandom() % 10;
		for (const auto &[tile, prev] : net) {
			if (DistanceManhattan(from, tile) >= want) { goal = tile; break; }
		}
		if (goal != INVALID_TILE) {
			c.goal = CitizenGoal::Stroll;
			if (c.stroll_legs == 0) c.stroll_legs = 2 + CitizenRandom() % 3;
		}
	}
	if (goal == INVALID_TILE) {
		uint want = 5 + CitizenRandom() % 12;
		for (const auto &[tile, prev] : net) {
			if (DistanceManhattan(from, tile) >= want && HasAdjacentTileType(tile, TileType::House)) { goal = tile; break; }
		}
		if (goal != INVALID_TILE) {
			if (mi == MonthIntention::Shopping) c.goal = CitizenGoal::Shopping;
			else if (mi == MonthIntention::Social || yi == YearIntention::Friends) c.goal = CitizenGoal::Visit;
			else c.goal = (CitizenRandom() % 2 == 0) ? CitizenGoal::Visit : CitizenGoal::Shopping;
		}
	}
	if (goal == INVALID_TILE) return false;
	c.path = TracePath(net, goal);
	c.pos = 0;
	c.sub = 0;
	return c.path.size() >= 2;
}

/** Heimweg planen. @return false, wenn das Zuhause nicht erreichbar ist. */
static bool PlanHomeTrip(Citizen &c, TileIndex from)
{
	auto net = WalkNetwork(from, NETWORK_LIMIT);
	if (net.count(c.home) == 0) return false;
	c.path = TracePath(net, c.home);
	c.pos = 0;
	c.sub = 0;
	c.goal = CitizenGoal::Home;
	return c.path.size() >= 2;
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
	c.stroll_legs = 0;

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
		if (!PlanOuting(c, start)) return;
		uint k = CitizenRandom() % 10;
		c.kind = k < 5 ? CitizenKind::Adult : k < 7 ? CitizenKind::Family : k < 9 ? CitizenKind::Child : CitizenKind::Stroller;
	}

	if (c.path.size() < 2) return;
	c.state = CitizenState::Walking;
	c.dwell_until = 0;
	_citizens.push_back(std::move(c));
}

/* ---------- Simulation ---------- */

bool IsParkedCarAway(TileIndex tile)
{
	return _cars_away.count(tile) != 0;
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
		if (c.state != CitizenState::Walking) continue;
		if (c.kind == CitizenKind::Car || tick % 3 == 0) CitizenStep(c);
	}

	/* Angekommene verweilen am Ziel (unsichtbar im Gebaeude) und planen
	 * spaeter den naechsten Ausflug - niemand verschwindet endgueltig. */
	for (auto it = _citizens.begin(); it != _citizens.end(); ) {
		Citizen &c = *it;
		if (c.state == CitizenState::Walking &&
				(c.pos >= c.path.size() || (c.pos == c.path.size() - 1 && c.sub >= 8))) {
			MarkTileDirtyByTile(c.path[std::min<size_t>(c.pos, c.path.size() - 1)]);
			c.pos = static_cast<uint16_t>(c.path.size() - 1);
			c.state = CitizenState::Dwelling;
			c.dwell_until = tick + 300 + CitizenRandom() % 1700;
			if (c.goal == CitizenGoal::Move) {
				/* Umzug vollzogen: neues Zuhause. */
				c.home = c.path.back();
				c.goal = CitizenGoal::Home;
			}
			if (c.path.back() == c.home && GetMonthIntention(c) == MonthIntention::Homebody) {
				c.dwell_until = tick + 1500 + CitizenRandom() % 3000; /* Bleibt gern daheim. */
			}
			if (c.goal == CitizenGoal::Stroll && c.stroll_legs > 0) {
				c.stroll_legs--;
				c.dwell_until = tick + 40 + CitizenRandom() % 120; /* Nur kurz verschnaufen. */
			}
			++it;
			continue;
		}
		if (c.state == CitizenState::Dwelling && tick >= c.dwell_until) {
			TileIndex from = c.path.empty() ? c.home : c.path.back();
			bool ok;
			if (c.kind == CitizenKind::Car) {
				/* Autos starten immer eine neue Rundfahrt ab ihrer Parkluecke. */
				ok = false;
			} else if (c.stroll_legs > 0) {
				ok = PlanOuting(c, from);
			} else if (from == c.home) {
				ok = PlanOuting(c, from);
			} else {
				ok = PlanHomeTrip(c, from);
			}
			if (c.kind == CitizenKind::Car) {
				/* Rundfahrt neu wuerfeln wie beim Spawn: Pfad hin und zurueck. */
				auto net = WalkNetwork(c.home, NETWORK_LIMIT);
				TileIndex far = c.home;
				uint best = 0;
				for (const auto &[tile, prev] : net) {
					uint d = DistanceManhattan(c.home, tile);
					if (d > best) { best = d; far = tile; }
				}
				ok = best >= 6;
				if (ok) {
					std::vector<TileIndex> out = TracePath(net, far);
					c.path = out;
					for (size_t i = out.size() - 1; i-- > 0; ) c.path.push_back(out[i]);
					c.pos = 0;
					c.sub = 0;
				}
			}
			if (ok) {
				c.state = CitizenState::Walking;
			} else if (from == c.home || c.kind == CitizenKind::Car) {
				/* Zuhause abgeschnitten (Strasse weg): spaeter nochmal probieren. */
				c.dwell_until = tick + 1000;
			} else {
				/* Weder weiter noch heim - dieser Buerger zieht weg. */
				CloseWindowById(WindowClass::Citizen, c.id);
				it = _citizens.erase(it);
				continue;
			}
			++it;
			continue;
		}
		++it;
	}

		/* Zeichen-Indexe neu aufbauen (Kachel -> Figuren, unterwegs befindliche
	 * Autos), damit das Rendern nicht ueber alle Buerger iterieren muss. */
	_citizens_by_tile.clear();
	_cars_away.clear();
	for (uint32_t i = 0; i < _citizens.size(); i++) {
		const Citizen &c = _citizens[i];
		if (c.state != CitizenState::Walking) continue;
		_citizens_by_tile.emplace(c.path[c.pos].base(), i);
		if (c.kind == CitizenKind::Car) _cars_away.insert(c.home);
	}

	/* Nachschub: jeden Tick eine zufaellige Stadt betrachten, damit die
	 * Strassen wirklich belebt wirken. */
	if (_citizens.size() >= MAX_CITIZENS) return;
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
		uint want = ClampU(t->cache.population / 12, 6, 90);
		if (here < want) SpawnCitizen(t);
		if (here + 10 < want) SpawnCitizen(t);
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
	auto range = _citizens_by_tile.equal_range(ti->tile.base());
	for (auto it = range.first; it != range.second; ++it) {
		const Citizen &c = _citizens[it->second];
		int px, py;
		DiagDirection dir;
		CitizenTilePos(c, px, py, dir);
		AddSortableSpriteToDraw(CitizenSprite(c, dir), CitizenPalette(c.id), *ti,
				{{(int8_t)px, (int8_t)py, 0}, {3, 3, 6}, {}});

		/* Sprechblase mit dem aktuellen Vorhaben ueber dem Kopf. */
		if (c.kind != CitizenKind::Car) {
			SpriteID bubble;
			switch (c.goal) {
				case CitizenGoal::Station: bubble = SPR_BUBBLE_STATION; break;
				case CitizenGoal::Visit: bubble = SPR_BUBBLE_VISIT; break;
				case CitizenGoal::Home: bubble = SPR_BUBBLE_HOME; break;
				case CitizenGoal::Shopping: bubble = SPR_BUBBLE_SHOPPING; break;
				case CitizenGoal::Move: bubble = SPR_BUBBLE_MOVE; break;
				default: bubble = SPR_BUBBLE_STROLL; break;
			}
			AddSortableSpriteToDraw(bubble, PAL_NONE, *ti,
					{{(int8_t)px, (int8_t)py, 7}, {3, 3, 3}, {}});
		}
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

		/* Portraet links (Manager-Gesichter des Spiels, stabil pro Person). */
		Rect face_r = {tr.left, tr.top, tr.left + 91, tr.top + 118};
		DrawCompanyManagerFace(CitizenFace(*c), Colours::Grey, face_r);
		tr.left += 100;

		int line = GetCharacterHeight(FontSize::Normal) + 2;
		int y = tr.top;
		DrawString(tr.left, tr.right, y, GetString(STR_CITIZEN_AGE, CitizenAge(*c)));
		y += line;
		DrawString(tr.left, tr.right, y, GetString(STR_CITIZEN_INTEREST, std::string(CitizenInterest(*c))));
		y += line;
		DrawString(tr.left, tr.right, y, GetString(STR_CITIZEN_HOME, c->town));
		y += line;
		StringID goal;
		if (c->state == CitizenState::Dwelling) {
			switch (c->goal) {
				case CitizenGoal::Station: goal = STR_CITIZEN_NOW_STATION; break;
				case CitizenGoal::Visit: goal = STR_CITIZEN_NOW_VISIT; break;
				case CitizenGoal::Shopping: goal = STR_CITIZEN_NOW_SHOPPING; break;
				case CitizenGoal::Drive: goal = STR_CITIZEN_NOW_PARKED; break;
				case CitizenGoal::Stroll: goal = STR_CITIZEN_NOW_BREAK; break;
				default: goal = STR_CITIZEN_NOW_HOME; break;
			}
		} else {
			switch (c->goal) {
				case CitizenGoal::Station: goal = STR_CITIZEN_GOAL_STATION; break;
				case CitizenGoal::Visit: goal = STR_CITIZEN_GOAL_VISIT; break;
				case CitizenGoal::Home: goal = STR_CITIZEN_GOAL_HOME; break;
				case CitizenGoal::Shopping: goal = STR_CITIZEN_GOAL_SHOPPING; break;
				case CitizenGoal::Stroll: goal = STR_CITIZEN_GOAL_STROLL; break;
				case CitizenGoal::Move: goal = STR_CITIZEN_GOAL_MOVE; break;
				default: goal = STR_CITIZEN_GOAL_DRIVE; break;
			}
		}
		DrawString(tr.left, tr.right, y, GetString(goal));
		y += line;
		static const StringID month_str[] = {STR_CITIZEN_MONTH_COMMUTE, STR_CITIZEN_MONTH_SHOPPING, STR_CITIZEN_MONTH_SOCIAL, STR_CITIZEN_MONTH_OUTDOOR, STR_CITIZEN_MONTH_HOMEBODY};
		static const StringID year_str[] = {STR_CITIZEN_YEAR_SPORT, STR_CITIZEN_YEAR_EXPLORE, STR_CITIZEN_YEAR_TRAVEL, STR_CITIZEN_YEAR_MOVE, STR_CITIZEN_YEAR_FRIENDS};
		DrawString(tr.left, tr.right, y, GetString(month_str[(uint)GetMonthIntention(*c)]));
		y += line;
		DrawString(tr.left, tr.right, y, GetString(year_str[(uint)GetYearIntention(*c)]));
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_CZ_PANEL) return;
		size.width = std::max<uint>(size.width, 300);
		size.height = std::max<uint>(size.height, std::max(119 + 8, 6 * (GetCharacterHeight(FontSize::Normal) + 2) + 8));
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
		if (c.state != CitizenState::Walking) continue; /* Unsichtbare nicht anklickbar. */
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
