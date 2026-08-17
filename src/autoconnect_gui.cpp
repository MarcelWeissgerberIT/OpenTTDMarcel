/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file autoconnect_gui.cpp Auto-Verbindung (Fork-Feature): zwei Städte
 * wählen, Verkehrsmittel und Anzahl angeben — Stationen, Wege, Depot und
 * Fahrzeuge werden automatisch geplant und gebaut.
 *
 * Stufe 1: Flugzeuge (zwei Flughäfen + Flugzeuge mit Pendel-Auftrag).
 * Stufe 1b: Busse (Haltestellen + Straße per A* + Depot + Busse).
 * Stufe 2: Zug (zwei Bahnhöfe + eingleisige Strecke + Depot + ein Zug).
 */

#include "stdafx.h"

#include <queue>
#include <set>
#include "airport.h"
#include "road_map.h"
#include "road_func.h"
#include "direction_func.h"
#include "tile_map.h"
#include "cargotype.h"
#include "road_cmd.h"
#include "rail_cmd.h"
#include "rail_map.h"
#include "train_cmd.h"
#include "bridge.h"
#include "tunnelbridge_cmd.h"
#include "newgrf_station.h"
#include "newgrf_roadstop.h"
#include "vehicle_type.h"
#include "command_func.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "engine_base.h"
#include "error.h"
#include "network/network.h"
#include "order_base.h"
#include "order_cmd.h"
#include "station_base.h"
#include "station_cmd.h"
#include "strings_func.h"
#include "tilearea_type.h"
#include "tilehighlight_func.h"
#include "town.h"
#include "map_func.h"
#include "debug.h"
#include "vehicle_base.h"
#include "vehicle_cmd.h"
#include "viewport_func.h"
#include "window_func.h"
#include "window_gui.h"

#include "widgets/autoconnect_widget.h"

#include "table/sprites.h"
#include "table/strings.h"

#include "safeguards.h"

/** Ergebnis eines Bauversuchs. */
struct AutoConnectResult {
	bool ok = false;
	Money cost = 0;
	StringID error = STR_NULL;
	uint32_t error_detail = 0; ///< Roh-ID der Engine-Fehlermeldung (Diagnose).
};

/**
 * Freie Stelle für einen kleinen Flughafen nahe der Stadt suchen.
 * @param t Stadt, in deren Nähe gebaut werden soll.
 * @return Bauplatz-Tile oder INVALID_TILE.
 */
static TileIndex FindAirportSite(const Town *t)
{
	for (TileIndex tile : SpiralTileSequence(t->xy, 40)) {
		CommandCost res = Command<Commands::BuildAirport>::Do({}, tile, AT_SMALL, 0, NEW_STATION, false);
		if (res.Succeeded()) return tile;
	}
	return INVALID_TILE;
}

/** Bestes verfügbares Passagierflugzeug wählen. */
static EngineID FindBestAircraft()
{
	EngineID best = EngineID::Invalid();
	uint best_capacity = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Aircraft)) {
		if (!e->company_avail.Test(_local_company)) continue;
		uint cap = e->GetDisplayDefaultCapacity();
		if (cap > best_capacity) {
			best_capacity = cap;
			best = e->index;
		}
	}
	return best;
}

/**
 * Flugverbindung bauen: zwei Flughäfen, N Flugzeuge, Pendel-Aufträge.
 */
static AutoConnectResult BuildAirConnection(Town *town_a, Town *town_b, uint count)
{
	AutoConnectResult result;

	/* Sind die Städte weit genug auseinander für zwei getrennte Flughäfen? */
	if (DistanceManhattan(town_a->xy, town_b->xy) < 24) {
		result.error = STR_AUTOCONNECT_ERR_TOO_CLOSE;
		return result;
	}

	Backup<CompanyID> cur_company(_current_company, _local_company);

	/* Flughafen A: Platz suchen und sofort bauen — erst danach für B suchen,
	 * damit die Platzsuche für B den neuen Flughafen A schon kennt. */
	TileIndex site_a = FindAirportSite(town_a);
	if (site_a == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_AIRPORT_SITE;
		cur_company.Restore();
		return result;
	}
	CommandCost cost_a = Command<Commands::BuildAirport>::Do(DoCommandFlag::Execute, site_a, AT_SMALL, 0, NEW_STATION, false);
	if (cost_a.Failed()) {
		result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
		result.error_detail = cost_a.GetErrorMessage().base();
		cur_company.Restore();
		return result;
	}

	TileIndex site_b = FindAirportSite(town_b);
	if (site_b == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_AIRPORT_SITE;
		cur_company.Restore();
		return result;
	}
	CommandCost cost_b = Command<Commands::BuildAirport>::Do(DoCommandFlag::Execute, site_b, AT_SMALL, 0, NEW_STATION, false);
	if (cost_b.Failed()) {
		result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
		cur_company.Restore();
		return result;
	}
	result.cost += cost_a.GetCost() + cost_b.GetCost();

	Station *st_a = Station::GetByTile(site_a);
	Station *st_b = Station::GetByTile(site_b);
	TileIndex hangar = st_a->airport.GetHangarTile(0);

	EngineID engine = FindBestAircraft();
	if (engine == EngineID::Invalid()) {
		result.error = STR_AUTOCONNECT_ERR_NO_VEHICLE;
		cur_company.Restore();
		return result;
	}

	/* Flugzeuge kaufen, Aufträge geben, starten. */
	for (uint i = 0; i < count; i++) {
		auto [cost_v, veh_id, refit_capacity, refit_mail, cargo_capacities] =
				Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, hangar, engine, true, INVALID_CARGO, ClientID::Invalid);
		if (cost_v.Failed()) {
			/* Kein Geld mehr o. Ä.: mit dem bauen, was da ist. */
			if (i == 0) {
				result.error = STR_AUTOCONNECT_ERR_NO_MONEY_VEHICLE;
				cur_company.Restore();
				return result;
			}
			break;
		}
		result.cost += cost_v.GetCost();

		Order order_a;
		order_a.MakeGoToStation(st_a->index);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, order_a);
		Order order_b;
		order_b.MakeGoToStation(st_b->index);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, order_b);

		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_id, false);
	}

	cur_company.Restore();
	result.ok = true;
	return result;
}

/* ------------------------- Busverbindung (Stufe 1b) ------------------- */

/**
 * Gerades Straßenstück in Stadtnähe finden, auf dem eine
 * Durchfahrt-Bushaltestelle gebaut werden kann.
 */
static TileIndex FindBusStopSite(const Town *t, Axis &axis_out)
{
	for (TileIndex tile : SpiralTileSequence(t->xy, 24)) {
		if (!IsNormalRoadTile(tile)) continue;
		RoadBits bits = GetRoadBits(tile, RoadTramType::Road);
		Axis axis;
		if (bits == ROAD_X) {
			axis = Axis::X;
		} else if (bits == ROAD_Y) {
			axis = Axis::Y;
		} else {
			continue;
		}
		CommandCost res = Command<Commands::BuildRoadStop>::Do({}, tile, 1, 1,
				RoadStopType::Bus, true, AxisToDiagDir(axis), ROADTYPE_ROAD,
				ROADSTOP_CLASS_DFLT, 0, NEW_STATION, false);
		if (res.Succeeded()) {
			axis_out = axis;
			return tile;
		}
	}
	return INVALID_TILE;
}

/**
 * A*-Wegsuche für eine Straße von \a from nach \a to.
 * Bestehende Straßen sind billig, Neubau teuer, Wasser unpassierbar.
 * @return Tile-Folge inkl. Start und Ziel, leer wenn kein Weg.
 */
static std::vector<TileIndex> FindRoadPath(TileIndex from, TileIndex to)
{
	struct Node { TileIndex tile; uint cost; uint est; };
	auto cmp = [](const Node &a, const Node &b) { return a.cost + a.est > b.cost + b.est; };
	std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
	std::map<TileIndex, TileIndex> came_from;
	std::map<TileIndex, uint> best;

	auto heuristic = [&](TileIndex t) { return DistanceManhattan(t, to) * 2; };
	open.push({from, 0, heuristic(from)});
	best[from] = 0;
	uint expanded = 0;

	while (!open.empty()) {
		Node n = open.top();
		open.pop();
		if (n.tile == to) break;
		if (n.cost > best[n.tile]) continue;
		if (++expanded > 200000) return {}; // Notbremse

		for (DiagDirection d = DiagDirection::Begin; d < DiagDirection::End; d++) {
			TileIndex next = AddTileIndexDiffCWrap(n.tile, TileIndexDiffCByDiagDir(d));
			if (next == INVALID_TILE) continue;

			uint step;
			if (IsNormalRoadTile(next) || IsRoadDepotTile(next)) {
				step = 2;   /* bestehende Straße mitbenutzen */
			} else if (IsTileType(next, TileType::Clear) || IsTileType(next, TileType::Trees)) {
				step = IsTileFlat(next) ? 8 : 14;
			} else {
				continue;   /* Wasser, Häuser, Industrie, ... */
			}

			uint nc = n.cost + step;
			auto it = best.find(next);
			if (it != best.end() && it->second <= nc) continue;
			best[next] = nc;
			came_from[next] = n.tile;
			open.push({next, nc, heuristic(next)});
		}
	}

	if (came_from.find(to) == came_from.end()) return {};
	std::vector<TileIndex> path;
	for (TileIndex t = to; t != from; t = came_from[t]) path.push_back(t);
	path.push_back(from);
	std::reverse(path.begin(), path.end());
	return path;
}

/** Bestes verfügbares Passagier-Straßenfahrzeug wählen. */
static EngineID FindBestBus()
{
	EngineID best = EngineID::Invalid();
	uint best_capacity = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (e->VehInfo<RoadVehicleInfo>().roadtype != ROADTYPE_ROAD) continue;
		if (!IsCargoInClass(e->GetDefaultCargoType(), CargoClass::Passengers)) continue;
		uint cap = e->GetDisplayDefaultCapacity();
		if (cap > best_capacity) {
			best_capacity = cap;
			best = e->index;
		}
	}
	return best;
}

/** Busverbindung bauen: Straße, zwei Haltestellen, Depot, N Busse. */
static AutoConnectResult BuildRoadConnection(Town *town_a, Town *town_b, uint count)
{
	AutoConnectResult result;
	Backup<CompanyID> cur_company(_current_company, _local_company);

	Axis axis_a, axis_b;
	TileIndex stop_a = FindBusStopSite(town_a, axis_a);
	TileIndex stop_b = FindBusStopSite(town_b, axis_b);
	if (stop_a == INVALID_TILE || stop_b == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_STOP_SITE;
		cur_company.Restore();
		return result;
	}

	std::vector<TileIndex> path = FindRoadPath(stop_a, stop_b);
	if (path.empty()) {
		result.error = STR_AUTOCONNECT_ERR_NO_ROAD_PATH;
		cur_company.Restore();
		return result;
	}

	/* Straße entlang des Wegs bauen: je Kachel die nötigen Halbstücke. */
	std::map<TileIndex, RoadBits> want;
	for (size_t i = 0; i + 1 < path.size(); i++) {
		DiagDirection d = DiagdirBetweenTiles(path[i], path[i + 1]);
		want[path[i]] |= DiagDirToRoadBits(d);
		want[path[i + 1]] |= DiagDirToRoadBits(ReverseDiagDir(d));
	}
	for (const auto &[tile, bits] : want) {
		RoadBits have = IsNormalRoadTile(tile) ? GetRoadBits(tile, RoadTramType::Road) : RoadBits{};
		RoadBits missing = bits;
		missing.Reset(have);
		if (missing.None()) continue;
		CommandCost c = Command<Commands::BuildRoad>::Do(DoCommandFlag::Execute, tile, missing, ROADTYPE_ROAD, DisallowedRoadDirections{}, TownID::Invalid());
		if (c.Succeeded()) result.cost += c.GetCost();
	}

	/* Haltestellen bauen. */
	for (auto [stop, axis] : {std::pair{stop_a, axis_a}, std::pair{stop_b, axis_b}}) {
		CommandCost c = Command<Commands::BuildRoadStop>::Do(DoCommandFlag::Execute, stop, 1, 1,
				RoadStopType::Bus, true, AxisToDiagDir(axis), ROADTYPE_ROAD,
				ROADSTOP_CLASS_DFLT, 0, NEW_STATION, false);
		if (c.Failed()) {
			result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
			result.error_detail = c.GetErrorMessage().base();
			cur_company.Restore();
			return result;
		}
		result.cost += c.GetCost();
	}

	/* Depot neben dem Weg nahe Haltestelle A. */
	TileIndex depot = INVALID_TILE;
	for (size_t i = 0; i < std::min<size_t>(path.size(), 20) && depot == INVALID_TILE; i++) {
		for (DiagDirection d = DiagDirection::Begin; d < DiagDirection::End; d++) {
			TileIndex cand = AddTileIndexDiffCWrap(path[i], TileIndexDiffCByDiagDir(d));
			if (cand == INVALID_TILE || want.count(cand) != 0) continue;
			CommandCost c = Command<Commands::BuildRoadDepot>::Do(DoCommandFlag::Execute, cand, ROADTYPE_ROAD, ReverseDiagDir(d));
			if (c.Succeeded()) {
				result.cost += c.GetCost();
				/* Anschlussstück vom Weg zum Depot. */
				CommandCost c2 = Command<Commands::BuildRoad>::Do(DoCommandFlag::Execute, path[i], DiagDirToRoadBits(d), ROADTYPE_ROAD, DisallowedRoadDirections{}, TownID::Invalid());
				if (c2.Succeeded()) result.cost += c2.GetCost();
				depot = cand;
				break;
			}
		}
	}
	if (depot == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_DEPOT;
		cur_company.Restore();
		return result;
	}

	Station *st_a = Station::GetByTile(stop_a);
	Station *st_b = Station::GetByTile(stop_b);

	EngineID engine = FindBestBus();
	if (engine == EngineID::Invalid()) {
		result.error = STR_AUTOCONNECT_ERR_NO_VEHICLE;
		cur_company.Restore();
		return result;
	}

	for (uint i = 0; i < count; i++) {
		auto [cost_v, veh_id, refit_capacity, refit_mail, cargo_capacities] =
				Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, engine, true, INVALID_CARGO, ClientID::Invalid);
		if (cost_v.Failed()) {
			if (i == 0) {
				result.error = STR_AUTOCONNECT_ERR_NO_MONEY_VEHICLE;
				cur_company.Restore();
				return result;
			}
			break;
		}
		result.cost += cost_v.GetCost();

		Order order_a;
		order_a.MakeGoToStation(st_a->index);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, order_a);
		Order order_b;
		order_b.MakeGoToStation(st_b->index);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, order_b);
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_id, false);
	}

	cur_company.Restore();
	result.ok = true;
	return result;
}

/* ------------------------- Zugverbindung (Stufe 2) -------------------- */

static const uint RAIL_PLATFORM_LEN = 3; ///< Bahnsteiglänge in Kacheln.

/** Bahnhofs-Bauplatz mit Anschlussrichtung. */
struct RailSite {
	TileIndex tile = INVALID_TILE; ///< Ursprungskachel des Bahnhofs.
	Axis axis = Axis::Invalid; ///< Bahnsteig-Achse.
	TileIndex platform_end = INVALID_TILE; ///< Bahnsteig-Ende auf der Anschlussseite.
	TileIndex exit = INVALID_TILE; ///< Kachel vor dem Bahnsteig-Ende (Streckenbeginn).
	DiagDirection exit_dir = DiagDirection::Invalid; ///< Richtung Bahnsteig-Ende -> exit.
};

/** Gleisstück aus zwei Kachelkanten bestimmen (Gerade oder Kurve). */
static Track TrackFromEdges(DiagDirection a, DiagDirection b)
{
	if (a > b) std::swap(a, b);
	if (a == DiagDirection::NE && b == DiagDirection::SW) return Track::X;
	if (a == DiagDirection::SE && b == DiagDirection::NW) return Track::Y;
	if (a == DiagDirection::NE && b == DiagDirection::NW) return Track::Upper;
	if (a == DiagDirection::SE && b == DiagDirection::SW) return Track::Lower;
	if (a == DiagDirection::SW && b == DiagDirection::NW) return Track::Left;
	return Track::Right; /* NE + SE */
}

/** Ist die Kachel für neuen Gleisbau frei (Wiese/Bäume)? */
static bool IsRailBuildableTile(TileIndex tile)
{
	return IsTileType(tile, TileType::Clear) || IsTileType(tile, TileType::Trees);
}

/**
 * Platz für einen Bahnhof (1 Gleis, RAIL_PLATFORM_LEN Kacheln) nahe der
 * Stadt suchen. Das Anschluss-Ende wird Richtung \a toward gewählt.
 */
static RailSite FindRailStationSite(const Town *t, TileIndex toward)
{
	RailSite site;
	for (TileIndex tile : SpiralTileSequence(t->xy, 30)) {
		for (Axis axis : {Axis::X, Axis::Y}) {
			CommandCost res = Command<Commands::BuildRailStation>::Do({}, tile, RAILTYPE_RAIL,
					axis, 1, RAIL_PLATFORM_LEN, STAT_CLASS_DFLT, 0, NEW_STATION, false);
			if (!res.Succeeded()) continue;

			/* Beide Bahnsteig-Enden prüfen, das Richtung Ziel bevorzugen. */
			DiagDirection far_dir = (axis == Axis::X) ? DiagDirection::SW : DiagDirection::SE;
			DiagDirection near_dir = ReverseDiagDir(far_dir);
			TileIndex far_end = tile;
			for (uint i = 1; i < RAIL_PLATFORM_LEN; i++) {
				far_end = AddTileIndexDiffCWrap(far_end, TileIndexDiffCByDiagDir(far_dir));
				if (far_end == INVALID_TILE) break;
			}
			if (far_end == INVALID_TILE) continue;

			struct { TileIndex end; DiagDirection dir; } cands[] = {
				{tile, near_dir}, {far_end, far_dir},
			};
			/* Näheres Ende zuerst probieren. */
			TileIndex e0 = AddTileIndexDiffCWrap(cands[0].end, TileIndexDiffCByDiagDir(cands[0].dir));
			TileIndex e1 = AddTileIndexDiffCWrap(cands[1].end, TileIndexDiffCByDiagDir(cands[1].dir));
			bool first_is_zero = !(e0 != INVALID_TILE && (e1 == INVALID_TILE || DistanceManhattan(e0, toward) <= DistanceManhattan(e1, toward)));
			for (uint k = 0; k < 2; k++) {
				auto &c = cands[(k + (first_is_zero ? 1 : 0)) % 2];
				TileIndex exit = AddTileIndexDiffCWrap(c.end, TileIndexDiffCByDiagDir(c.dir));
				if (exit == INVALID_TILE || !IsRailBuildableTile(exit)) continue;
				site.tile = tile;
				site.axis = axis;
				site.platform_end = c.end;
				site.exit = exit;
				site.exit_dir = c.dir;
				return site;
			}
		}
	}
	return site;
}

/**
 * A*-Wegsuche für eine Bahnstrecke. Zustände sind (Kachel, Einfahrtrichtung),
 * damit Kurven und Bahnübergänge korrekt geplant werden.
 * @return Folge aus (Kachel, Einfahrtrichtung), leer wenn kein Weg.
 */
static std::vector<std::pair<TileIndex, DiagDirection>> FindRailPath(const RailSite &from, const RailSite &to)
{
	using State = std::pair<TileIndex, DiagDirection>;
	struct Node { TileIndex tile; DiagDirection dir; uint cost; uint est; };
	auto cmp = [](const Node &a, const Node &b) { return a.cost + a.est > b.cost + b.est; };
	std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
	std::map<State, State> came_from;
	std::map<State, uint> best;

	auto heuristic = [&](TileIndex t) { return DistanceManhattan(t, to.exit) * 4; };
	State start{from.exit, from.exit_dir};
	open.push({from.exit, from.exit_dir, 0, heuristic(from.exit)});
	best[start] = 0;
	uint expanded = 0;
	State goal{INVALID_TILE, DiagDirection::Invalid};

	while (!open.empty()) {
		Node n = open.top();
		open.pop();
		State cur{n.tile, n.dir};
		if (n.tile == to.exit && n.dir != to.exit_dir) { goal = cur; break; }
		if (n.cost > best[cur]) continue;
		if (++expanded > 400000) return {};

		bool on_crossing = IsNormalRoadTile(n.tile); /* Bahnübergang: nur geradeaus */
		for (DiagDirection d = DiagDirection::Begin; d < DiagDirection::End; d++) {
			if (d == ReverseDiagDir(n.dir)) continue; /* kein Wenden */
			if (on_crossing && d != n.dir) continue;
			TileIndex next = AddTileIndexDiffCWrap(n.tile, TileIndexDiffCByDiagDir(d));
			if (next == INVALID_TILE) continue;

			/* Wasser voraus: Brücke gerade hinüber versuchen. */
			if (IsTileType(next, TileType::Water) && d == n.dir && !IsNormalRoadTile(n.tile)) {
				TileIndex land = next;
				uint span = 0;
				while (land != INVALID_TILE && IsTileType(land, TileType::Water) && ++span <= 12) {
					land = AddTileIndexDiffCWrap(land, TileIndexDiffCByDiagDir(d));
				}
				if (land == INVALID_TILE || span > 12) continue;
				if (!IsRailBuildableTile(land) || land == to.exit) continue;
				uint nc = n.cost + (span + 2) * 12;
				State ns{land, d};
				auto it = best.find(ns);
				if (it != best.end() && it->second <= nc) continue;
				best[ns] = nc;
				came_from[ns] = cur;
				open.push({land, d, nc, heuristic(land)});
				continue;
			}

			uint step;
			if (IsRailBuildableTile(next)) {
				step = IsTileFlat(next) ? 8 : 14;
			} else if (IsNormalRoadTile(next)) {
				/* Bahnübergang nur senkrecht über gerade Straßen. */
				RoadBits bits = GetRoadBits(next, RoadTramType::Road);
				Axis road_axis;
				if (bits == ROAD_X) {
					road_axis = Axis::X;
				} else if (bits == ROAD_Y) {
					road_axis = Axis::Y;
				} else {
					continue;
				}
				if (DiagDirToAxis(d) == road_axis) continue;
				step = 12;
			} else {
				continue;
			}

			uint nc = n.cost + step;
			State ns{next, d};
			auto it = best.find(ns);
			if (it != best.end() && it->second <= nc) continue;
			best[ns] = nc;
			came_from[ns] = cur;
			open.push({next, d, nc, heuristic(next)});
		}
	}

	if (goal.first == INVALID_TILE) return {};
	std::vector<State> path;
	for (State s = goal; !(s == start); s = came_from[s]) path.push_back(s);
	path.push_back(start);
	std::reverse(path.begin(), path.end());
	return path;
}

/** Stärkste verfügbare Lokomotive für normale Schienen wählen. */
static EngineID FindBestTrainEngine()
{
	EngineID best = EngineID::Invalid();
	uint best_power = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		const RailVehicleInfo &rvi = e->VehInfo<RailVehicleInfo>();
		if (rvi.railveh_type == RailVehicleType::Wagon) continue;
		if (!rvi.railtypes.Test(RAILTYPE_RAIL)) continue;
		uint power = e->GetPower();
		if (power > best_power) {
			best_power = power;
			best = e->index;
		}
	}
	return best;
}

/** Passagierwaggon mit größter Kapazität wählen. */
static EngineID FindBestPassengerWagon()
{
	EngineID best = EngineID::Invalid();
	uint best_capacity = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		const RailVehicleInfo &rvi = e->VehInfo<RailVehicleInfo>();
		if (rvi.railveh_type != RailVehicleType::Wagon) continue;
		if (!rvi.railtypes.Test(RAILTYPE_RAIL)) continue;
		if (!IsCargoInClass(e->GetDefaultCargoType(), CargoClass::Passengers)) continue;
		uint cap = e->GetDisplayDefaultCapacity();
		if (cap > best_capacity) {
			best_capacity = cap;
			best = e->index;
		}
	}
	return best;
}

/** Zugverbindung bauen: Bahnhöfe, Strecke, Depot, ein Zug mit N Waggons. */
static AutoConnectResult BuildRailConnection(Town *town_a, Town *town_b, uint wagon_count)
{
	AutoConnectResult result;
	Backup<CompanyID> cur_company(_current_company, _local_company);

	RailSite site_a = FindRailStationSite(town_a, town_b->xy);
	RailSite site_b = FindRailStationSite(town_b, town_a->xy);
	if (site_a.tile == INVALID_TILE || site_b.tile == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_RAIL_SITE;
		cur_company.Restore();
		return result;
	}

	auto path = FindRailPath(site_a, site_b);
	if (path.empty()) {
		result.error = STR_AUTOCONNECT_ERR_NO_RAIL_PATH;
		cur_company.Restore();
		return result;
	}

	/* Bahnhöfe bauen. */
	for (const RailSite *s : {&site_a, &site_b}) {
		CommandCost c = Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, s->tile,
				RAILTYPE_RAIL, s->axis, 1, RAIL_PLATFORM_LEN, STAT_CLASS_DFLT, 0, NEW_STATION, false);
		if (c.Failed()) {
			result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
			result.error_detail = c.GetErrorMessage().base();
			cur_company.Restore();
			return result;
		}
		result.cost += c.GetCost();
	}

	/* Brückenkacheln bestimmen: aufeinanderfolgende Zustände mit Abstand > 1. */
	std::set<size_t> bridge_heads;
	for (size_t i = 0; i + 1 < path.size(); i++) {
		if (DistanceManhattan(path[i].first, path[i + 1].first) > 1) {
			bridge_heads.insert(i);
			bridge_heads.insert(i + 1);
		}
	}

	/* Gleise legen: je Kachel Einfahrts- und Ausfahrtskante verbinden. */
	for (size_t i = 0; i < path.size(); i++) {
		/* Brücke statt Gleis? */
		if (i + 1 < path.size() && DistanceManhattan(path[i].first, path[i + 1].first) > 1) {
			uint len = DistanceManhattan(path[i].first, path[i + 1].first) - 1;
			BridgeType bt = 0;
			for (; bt < MAX_BRIDGES; bt++) {
				if (CheckBridgeAvailability(bt, len).Succeeded()) break;
			}
			CommandCost c = Command<Commands::BuildBridge>::Do(DoCommandFlag::Execute, path[i + 1].first, path[i].first,
					TransportType::Rail, bt < MAX_BRIDGES ? bt : 0, RAILTYPE_RAIL, INVALID_ROADTYPE);
			if (c.Failed()) {
				result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
				result.error_detail = c.GetErrorMessage().base();
				cur_company.Restore();
				return result;
			}
			result.cost += c.GetCost();
			continue;
		}
		if (bridge_heads.count(i) != 0) continue; /* Rampe liefert das Gleis */
		DiagDirection entry_edge = ReverseDiagDir(path[i].second);
		DiagDirection exit_edge = (i + 1 < path.size()) ? path[i + 1].second : ReverseDiagDir(site_b.exit_dir);
		Track track = TrackFromEdges(entry_edge, exit_edge);
		CommandCost c = Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, path[i].first, RAILTYPE_RAIL, track, true);
		if (c.Failed()) {
			result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
			result.error_detail = c.GetErrorMessage().base();
			cur_company.Restore();
			return result;
		}
		result.cost += c.GetCost();
	}

	/* Depot an einer geraden Streckenkachel nahe Bahnhof A. */
	TileIndex depot = INVALID_TILE;
	for (size_t i = 1; i < std::min<size_t>(path.size(), 25) && depot == INVALID_TILE; i++) {
		DiagDirection entry_edge = ReverseDiagDir(path[i].second);
		DiagDirection exit_edge = (i + 1 < path.size()) ? path[i + 1].second : ReverseDiagDir(site_b.exit_dir);
		if (exit_edge != path[i].second) continue; /* nur gerade Kacheln */
		for (DiagDirection d = DiagDirection::Begin; d < DiagDirection::End; d++) {
			if (d == entry_edge || d == exit_edge) continue;
			TileIndex cand = AddTileIndexDiffCWrap(path[i].first, TileIndexDiffCByDiagDir(d));
			if (cand == INVALID_TILE || !IsRailBuildableTile(cand)) continue;
			CommandCost c = Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, cand, RAILTYPE_RAIL, ReverseDiagDir(d));
			if (c.Failed()) continue;
			result.cost += c.GetCost();
			/* Anschlusskurven vom Depot auf beide Streckenrichtungen. */
			CommandCost c1 = Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, path[i].first, RAILTYPE_RAIL, TrackFromEdges(d, entry_edge), true);
			CommandCost c2 = Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, path[i].first, RAILTYPE_RAIL, TrackFromEdges(d, exit_edge), true);
			if (c1.Succeeded()) result.cost += c1.GetCost();
			if (c2.Succeeded()) result.cost += c2.GetCost();
			if (c1.Failed() && c2.Failed()) continue; /* Depot bleibt, aber unbrauchbar - nächster Versuch */
			depot = cand;
			break;
		}
	}
	if (depot == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_DEPOT;
		cur_company.Restore();
		return result;
	}

	Station *st_a = Station::GetByTile(site_a.tile);
	Station *st_b = Station::GetByTile(site_b.tile);

	EngineID engine = FindBestTrainEngine();
	if (engine == EngineID::Invalid()) {
		result.error = STR_AUTOCONNECT_ERR_NO_VEHICLE;
		cur_company.Restore();
		return result;
	}

	auto [cost_v, veh_id, refit_capacity, refit_mail, cargo_capacities] =
			Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, engine, true, INVALID_CARGO, ClientID::Invalid);
	if (cost_v.Failed()) {
		result.error = STR_AUTOCONNECT_ERR_NO_MONEY_VEHICLE;
		cur_company.Restore();
		return result;
	}
	result.cost += cost_v.GetCost();

	EngineID wagon = FindBestPassengerWagon();
	if (wagon != EngineID::Invalid()) {
		for (uint i = 0; i < wagon_count; i++) {
			auto [cost_w, wagon_id, rw1, rw2, rw3] =
					Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, wagon, true, INVALID_CARGO, ClientID::Invalid);
			if (cost_w.Failed()) break;
			result.cost += cost_w.GetCost();
			Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, wagon_id, veh_id, false);
		}
	}

	Order order_a;
	order_a.MakeGoToStation(st_a->index);
	Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, order_a);
	Order order_b;
	order_b.MakeGoToStation(st_b->index);
	Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, order_b);
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_id, false);

	cur_company.Restore();
	result.ok = true;
	return result;
}

/** Fenster der Auto-Verbindung. */
struct AutoConnectWindow : Window {
	TownID town_a = TownID::Invalid();
	TownID town_b = TownID::Invalid();
	uint count = 2;
	uint8_t mode = 0; ///< 0 = Flugzeuge, 1 = Busse, 2 = Zug.
	WidgetID picking_for = 0; ///< Widget, für das gerade eine Stadt gewählt wird (0 = keins).
	std::string status;

	AutoConnectWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->InitNested(number);
		this->status = GetString(STR_AUTOCONNECT_STATUS_PICK);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		ResetObjectToPlace();
		this->Window::Close();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_AC_TOWN_A:
				return this->town_a == TownID::Invalid() ? GetString(STR_AUTOCONNECT_PICK_TOWN_A) : GetString(STR_AUTOCONNECT_TOWN_NAME, this->town_a);
			case WID_AC_TOWN_B:
				return this->town_b == TownID::Invalid() ? GetString(STR_AUTOCONNECT_PICK_TOWN_B) : GetString(STR_AUTOCONNECT_TOWN_NAME, this->town_b);
			case WID_AC_COUNT: {
				static const StringID counts[] = {STR_AUTOCONNECT_COUNT, STR_AUTOCONNECT_COUNT_BUS, STR_AUTOCONNECT_COUNT_TRAIN};
				return GetString(counts[this->mode], this->count);
			}
			case WID_AC_MODE: {
				static const StringID modes[] = {STR_AUTOCONNECT_MODE_AIR, STR_AUTOCONNECT_MODE_BUS, STR_AUTOCONNECT_MODE_TRAIN};
				return GetString(modes[this->mode]);
			}
			case WID_AC_STATUS:
				return this->status;
			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_AC_TOWN_A:
			case WID_AC_TOWN_B:
				this->picking_for = widget;
				SetObjectToPlace(SPR_CURSOR_TOWN, PAL_NONE, HT_RECT, this->window_class, this->window_number);
				this->status = GetString(STR_AUTOCONNECT_STATUS_CLICK_MAP);
				this->SetDirty();
				break;

			case WID_AC_MODE:
				this->mode = (this->mode + 1) % 3;
				this->SetDirty();
				break;

			case WID_AC_COUNT_DOWN:
				if (this->count > 1) this->count--;
				this->SetDirty();
				break;

			case WID_AC_COUNT_UP:
				if (this->count < 10) this->count++;
				this->SetDirty();
				break;

			case WID_AC_BUILD: {
				if (_networking) {
					ShowErrorMessage(GetEncodedString(STR_AUTOCONNECT_ERR_SINGLEPLAYER), {}, WarningLevel::Info);
					break;
				}
				if (this->town_a == TownID::Invalid() || this->town_b == TownID::Invalid() || this->town_a == this->town_b) {
					this->status = GetString(STR_AUTOCONNECT_STATUS_NEED_TOWNS);
					this->SetDirty();
					break;
				}
				AutoConnectResult res;
				switch (this->mode) {
					case 1: res = BuildRoadConnection(Town::Get(this->town_a), Town::Get(this->town_b), this->count); break;
					case 2: res = BuildRailConnection(Town::Get(this->town_a), Town::Get(this->town_b), this->count); break;
					default: res = BuildAirConnection(Town::Get(this->town_a), Town::Get(this->town_b), this->count); break;
				}
				if (res.ok) {
					this->status = GetString(STR_AUTOCONNECT_STATUS_DONE, res.cost);
				} else {
					this->status = GetString(res.error);
					if (res.error_detail != 0) this->status += fmt::format(" [#{}]", res.error_detail);
				}
				this->SetDirty();
				break;
			}
		}
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		Town *t = CalcClosestTownFromTile(tile);
		if (t == nullptr) return;
		if (this->picking_for == WID_AC_TOWN_A) this->town_a = t->index;
		if (this->picking_for == WID_AC_TOWN_B) this->town_b = t->index;
		this->picking_for = 0;
		this->status = GetString(STR_AUTOCONNECT_STATUS_PICK);
		ResetObjectToPlace();
		this->SetDirty();
	}

	void OnPlaceObjectAbort() override
	{
		this->picking_for = 0;
		this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_autoconnect_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_AUTOCONNECT_CAPTION),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_TOWN_A), SetFill(1, 0), SetMinimalSize(220, 14), SetToolTip(STR_AUTOCONNECT_PICK_TOOLTIP),
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_TOWN_B), SetFill(1, 0), SetMinimalSize(220, 14), SetToolTip(STR_AUTOCONNECT_PICK_TOOLTIP),
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_MODE), SetFill(1, 0), SetMinimalSize(220, 14), SetToolTip(STR_AUTOCONNECT_MODE_TOOLTIP),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_COUNT_DOWN), SetMinimalSize(20, 14), SetStringTip(STR_AUTOCONNECT_MINUS),
				NWidget(WWT_TEXT, Colours::Invalid, WID_AC_COUNT), SetFill(1, 0), SetAlignment({AlignmentH::Centre, AlignmentV::Middle}),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_COUNT_UP), SetMinimalSize(20, 14), SetStringTip(STR_AUTOCONNECT_PLUS),
			EndContainer(),
			NWidget(WWT_PUSHTXTBTN, Colours::Green, WID_AC_BUILD), SetFill(1, 0), SetMinimalSize(220, 16), SetStringTip(STR_AUTOCONNECT_BUILD, STR_AUTOCONNECT_BUILD_TOOLTIP),
			NWidget(WWT_TEXT, Colours::Invalid, WID_AC_STATUS), SetFill(1, 0), SetMinimalSize(220, 28),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _autoconnect_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::AutoConnect, WindowClass::None,
	{},
	_nested_autoconnect_widgets
);

/** Fenster der Auto-Verbindung öffnen (Fork-Feature). */
void ShowAutoConnectWindow()
{
	AllocateWindowDescFront<AutoConnectWindow>(_autoconnect_desc, 0);
}
