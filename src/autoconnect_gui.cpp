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
 * Stufe 3: Mehrzug-Ring mit einseitigen Pfadsignalen; Schiffe (Häfen + Depot).
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
#include "terraform_cmd.h"
#include "water_cmd.h"
#include "newgrf_station.h"
#include "newgrf_roadstop.h"
#include "vehicle_type.h"
#include "command_func.h"
#include "company_func.h"
#include "company_base.h"
#include "misc_cmd.h"
#include "core/backup_type.hpp"
#include "engine_base.h"
#include "error.h"
#include "network/network.h"
#include "order_base.h"
#include "order_cmd.h"
#include "station_base.h"
#include "station_map.h"
#include "roadstop_base.h"
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

/** Halteauftrag wie im Auftrags-GUI konstruieren (Haltepunkt: Ende). */
static Order MakeStationOrder(StationID station)
{
	Order o;
	o.MakeGoToStation(station);
	o.SetStopLocation(OrderStopLocation::FarEnd);
	return o;
}

/**
 * Nächstgelegene eigene Station mit gewünschter Einrichtung nahe der
 * Stadt suchen (Wiederverwendung statt Neubau).
 */
static Station *FindNearbyOwnStation(const Town *t, StationFacility fac, uint radius)
{
	Station *best = nullptr;
	uint best_dist = radius + 1;
	for (Station *st : Station::Iterate()) {
		if (st->owner != _local_company) continue;
		if (!st->facilities.Test(fac)) continue;
		uint d = DistanceManhattan(st->xy, t->xy);
		if (d < best_dist) {
			best_dist = d;
			best = st;
		}
	}
	return best;
}

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
static AutoConnectResult BuildAirConnection(Town *town_a, Town *town_b, uint count, bool estimate)
{
	AutoConnectResult result;
	DoCommandFlags do_flags = estimate ? DoCommandFlags{} : DoCommandFlags{DoCommandFlag::Execute};

	/* Sind die Städte weit genug auseinander für zwei getrennte Flughäfen? */
	if (DistanceManhattan(town_a->xy, town_b->xy) < 24) {
		result.error = STR_AUTOCONNECT_ERR_TOO_CLOSE;
		return result;
	}

	Backup<CompanyID> cur_company(_current_company, _local_company);

	/* Bestehende eigene Flughäfen in Stadtnähe wiederverwenden. */
	Station *re_a = FindNearbyOwnStation(town_a, StationFacility::Airport, 25);
	Station *re_b = FindNearbyOwnStation(town_b, StationFacility::Airport, 25);
	if (re_a != nullptr && re_a == re_b) re_b = nullptr;

	TileIndex site_a = INVALID_TILE;
	if (re_a == nullptr) {
		/* Flughafen A: Platz suchen und sofort bauen — erst danach für B
		 * suchen, damit die Platzsuche für B den neuen Flughafen A kennt. */
		site_a = FindAirportSite(town_a);
		if (site_a == INVALID_TILE) {
			result.error = STR_AUTOCONNECT_ERR_NO_AIRPORT_SITE;
			cur_company.Restore();
			return result;
		}
		CommandCost cost_a = Command<Commands::BuildAirport>::Do(do_flags, site_a, AT_SMALL, 0, NEW_STATION, false);
		if (cost_a.Failed()) {
			Debug(misc, 0, "AC: airport A failed at 0x{:X} err {}", site_a.base(), cost_a.GetErrorMessage().base());
			/* Stadtbewertung blockiert? Der Spieler hat den Bau angewiesen. */
			cost_a = Command<Commands::BuildAirport>::Do(DoCommandFlags{DoCommandFlag::NoTestTownRating} | do_flags, site_a, AT_SMALL, 0, NEW_STATION, false);
		}
		if (cost_a.Failed()) {
			result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
			result.error_detail = cost_a.GetErrorMessage().base();
			cur_company.Restore();
			return result;
		}
		result.cost += cost_a.GetCost();
	}

	TileIndex site_b = INVALID_TILE;
	if (re_b == nullptr) {
		site_b = FindAirportSite(town_b);
		if (site_b == INVALID_TILE) {
			result.error = STR_AUTOCONNECT_ERR_NO_AIRPORT_SITE;
			cur_company.Restore();
			return result;
		}
		CommandCost cost_b = Command<Commands::BuildAirport>::Do(do_flags, site_b, AT_SMALL, 0, NEW_STATION, false);
		if (cost_b.Failed()) {
			Debug(misc, 0, "AC: airport B failed at 0x{:X} err {}", site_b.base(), cost_b.GetErrorMessage().base());
			cost_b = Command<Commands::BuildAirport>::Do(DoCommandFlags{DoCommandFlag::NoTestTownRating} | do_flags, site_b, AT_SMALL, 0, NEW_STATION, false);
		}
		if (cost_b.Failed()) {
			result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
			result.error_detail = cost_b.GetErrorMessage().base();
			cur_company.Restore();
			return result;
		}
		result.cost += cost_b.GetCost();
	}

	EngineID engine = FindBestAircraft();
	if (engine == EngineID::Invalid()) {
		result.error = STR_AUTOCONNECT_ERR_NO_VEHICLE;
		cur_company.Restore();
		return result;
	}

	if (estimate) {
		result.cost += Engine::Get(engine)->GetCost() * count;
		cur_company.Restore();
		result.ok = true;
		return result;
	}

	Station *st_a = re_a != nullptr ? re_a : Station::GetByTile(site_a);
	Station *st_b = re_b != nullptr ? re_b : Station::GetByTile(site_b);
	Station *hangar_st = st_a->airport.HasHangar() ? st_a : st_b;
	if (!hangar_st->airport.HasHangar()) {
		result.error = STR_AUTOCONNECT_ERR_NO_DEPOT;
		cur_company.Restore();
		return result;
	}
	TileIndex hangar = hangar_st->airport.GetHangarTile(0);

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

		CommandCost co_a = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, MakeStationOrder(st_a->index));
		if (co_a.Failed()) result.error_detail = co_a.GetErrorMessage().base();
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, MakeStationOrder(st_b->index));

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
static EngineID FindBestBus(CargoClasses cargo_class)
{
	EngineID best = EngineID::Invalid();
	uint best_capacity = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (e->VehInfo<RoadVehicleInfo>().roadtype != ROADTYPE_ROAD) continue;
		if (!IsCargoInClass(e->GetDefaultCargoType(), cargo_class)) continue;
		uint cap = e->GetDisplayDefaultCapacity();
		if (cap > best_capacity) {
			best_capacity = cap;
			best = e->index;
		}
	}
	return best;
}

/** Busverbindung bauen: Straße, zwei Haltestellen, Depot, N Busse. */
static AutoConnectResult BuildRoadConnection(Town *town_a, Town *town_b, uint count, uint8_t cargo_mode, bool estimate)
{
	AutoConnectResult result;
	DoCommandFlags do_flags = estimate ? DoCommandFlags{} : DoCommandFlags{DoCommandFlag::Execute};
	Backup<CompanyID> cur_company(_current_company, _local_company);

	/* Bestehende eigene Haltestellen in Stadtnähe wiederverwenden. */
	Station *re_a = FindNearbyOwnStation(town_a, StationFacility::BusStop, 20);
	Station *re_b = FindNearbyOwnStation(town_b, StationFacility::BusStop, 20);
	if (re_a != nullptr && re_a == re_b) re_b = nullptr;
	bool build_a = re_a == nullptr || re_a->bus_stops == nullptr;
	bool build_b = re_b == nullptr || re_b->bus_stops == nullptr;

	Axis axis_a{}, axis_b{};
	TileIndex stop_a = build_a ? FindBusStopSite(town_a, axis_a) : re_a->bus_stops->xy;
	TileIndex stop_b = build_b ? FindBusStopSite(town_b, axis_b) : re_b->bus_stops->xy;
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
		CommandCost c = Command<Commands::BuildRoad>::Do(do_flags, tile, missing, ROADTYPE_ROAD, DisallowedRoadDirections{}, TownID::Invalid());
		if (c.Succeeded()) result.cost += c.GetCost();
	}

	/* Haltestellen bauen (nur wo keine wiederverwendet wird). */
	for (auto [stop, axis, build] : {std::tuple{stop_a, axis_a, build_a}, std::tuple{stop_b, axis_b, build_b}}) {
		if (!build) continue;
		CommandCost c = Command<Commands::BuildRoadStop>::Do(do_flags, stop, 1, 1,
				RoadStopType::Bus, true, AxisToDiagDir(axis), ROADTYPE_ROAD,
				ROADSTOP_CLASS_DFLT, 0, NEW_STATION, false);
		if (c.Failed()) {
			c = Command<Commands::BuildRoadStop>::Do(DoCommandFlags{DoCommandFlag::NoTestTownRating} | do_flags, stop, 1, 1,
					RoadStopType::Bus, true, AxisToDiagDir(axis), ROADTYPE_ROAD,
					ROADSTOP_CLASS_DFLT, 0, NEW_STATION, false);
		}
		if (c.Failed()) {
			result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
			result.error_detail = c.GetErrorMessage().base();
			cur_company.Restore();
			return result;
		}
		result.cost += c.GetCost();
	}

	/* Fahrzeugwahl je Index: bei "beides" abwechselnd Bus und Postwagen. */
	EngineID e_pax = FindBestBus({CargoClass::Passengers});
	EngineID e_mail = FindBestBus({CargoClass::Mail});
	auto pick_engine = [&](uint i) {
		bool want_mail = cargo_mode == 1 || (cargo_mode == 2 && i % 2 == 1);
		EngineID e = want_mail ? e_mail : e_pax;
		if (e == EngineID::Invalid()) e = want_mail ? e_pax : e_mail;
		return e;
	};
	if (pick_engine(0) == EngineID::Invalid()) {
		result.error = STR_AUTOCONNECT_ERR_NO_VEHICLE;
		cur_company.Restore();
		return result;
	}

	if (estimate) {
		for (uint i = 0; i < count; i++) result.cost += Engine::Get(pick_engine(i))->GetCost();
		cur_company.Restore();
		result.ok = true;
		return result;
	}

	/* Depot neben dem Weg nahe Haltestelle A. Anschluss wird vor dem Bau
	 * getestet, damit kein Depot ohne Ausfahrt entsteht. */
	TileIndex depot = INVALID_TILE;
	for (size_t i = 0; i < std::min<size_t>(path.size(), 20) && depot == INVALID_TILE; i++) {
		if (!IsNormalRoadTile(path[i])) continue; /* Haltestellen usw. überspringen */
		for (DiagDirection d = DiagDirection::Begin; d < DiagDirection::End; d++) {
			TileIndex cand = AddTileIndexDiffCWrap(path[i], TileIndexDiffCByDiagDir(d));
			if (cand == INVALID_TILE || want.count(cand) != 0) continue;
			bool need_bit = !(GetRoadBits(path[i], RoadTramType::Road) & DiagDirToRoadBits(d)).Any();
			if (Command<Commands::BuildRoadDepot>::Do({}, cand, ROADTYPE_ROAD, ReverseDiagDir(d)).Failed()) continue;
			if (need_bit && Command<Commands::BuildRoad>::Do({}, path[i], DiagDirToRoadBits(d), ROADTYPE_ROAD, DisallowedRoadDirections{}, TownID::Invalid()).Failed()) continue;
			CommandCost c = Command<Commands::BuildRoadDepot>::Do(DoCommandFlag::Execute, cand, ROADTYPE_ROAD, ReverseDiagDir(d));
			if (c.Failed()) continue;
			result.cost += c.GetCost();
			if (need_bit) {
				CommandCost c2 = Command<Commands::BuildRoad>::Do(DoCommandFlag::Execute, path[i], DiagDirToRoadBits(d), ROADTYPE_ROAD, DisallowedRoadDirections{}, TownID::Invalid());
				if (c2.Failed()) continue; /* Depot bleibt verwaist - naechster Kandidat */
				result.cost += c2.GetCost();
			}
			depot = cand;
			break;
		}
	}
	if (depot == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_DEPOT;
		cur_company.Restore();
		return result;
	}

	Station *st_a = Station::GetByTile(stop_a);
	Station *st_b = Station::GetByTile(stop_b);

	for (uint i = 0; i < count; i++) {
		auto [cost_v, veh_id, refit_capacity, refit_mail, cargo_capacities] =
				Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, pick_engine(i), true, INVALID_CARGO, ClientID::Invalid);
		if (cost_v.Failed()) {
			if (i == 0) {
				result.error = STR_AUTOCONNECT_ERR_NO_MONEY_VEHICLE;
				cur_company.Restore();
				return result;
			}
			break;
		}
		result.cost += cost_v.GetCost();

		CommandCost co_a = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, MakeStationOrder(st_a->index));
		if (co_a.Failed()) result.error_detail = co_a.GetErrorMessage().base();
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, MakeStationOrder(st_b->index));
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_id, false);
	}

	cur_company.Restore();
	result.ok = true;
	return result;
}

/* ------------------------- Zugverbindung (Stufe 2) -------------------- */

static const uint RAIL_PLATFORM_LEN = 3; ///< Bahnsteiglänge in Kacheln.

/** Bahnhofs-Bauplatz mit Anschlussrichtungen. */
struct RailSite {
	TileIndex tile = INVALID_TILE; ///< Ursprungskachel des Bahnhofs.
	Axis axis = Axis::Invalid; ///< Bahnsteig-Achse.
	TileIndex exit = INVALID_TILE; ///< Anschluss Richtung Ziel (Hinweg).
	DiagDirection exit_dir = DiagDirection::Invalid; ///< Richtung Bahnsteig-Ende -> exit.
	TileIndex exit2 = INVALID_TILE; ///< Gegenüberliegender Anschluss (Rückweg, nur Ringbetrieb).
	DiagDirection exit2_dir = DiagDirection::Invalid; ///< Richtung Bahnsteig-Ende -> exit2.
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
 * RailSite aus einem bestehenden eigenen Bahnhof ableiten. Liefert
 * false, wenn die nötigen Anschlusskacheln nicht frei sind.
 */
static bool RailSiteFromStation(const Station *st, TileIndex toward, bool need_both, RailSite &site)
{
	TileArea ta = st->train_station;
	if (ta.tile == INVALID_TILE || !IsTileType(ta.tile, TileType::Station)) return false;
	Axis axis = GetRailStationAxis(ta.tile);
	uint len = (axis == Axis::X) ? ta.w : ta.h;
	if (len < 2) return false;

	DiagDirection far_dir = (axis == Axis::X) ? DiagDirection::SW : DiagDirection::SE;
	TileIndex far_end = ta.tile;
	for (uint i = 1; i < len; i++) {
		far_end = AddTileIndexDiffCWrap(far_end, TileIndexDiffCByDiagDir(far_dir));
		if (far_end == INVALID_TILE) return false;
	}

	TileIndex exit_near = AddTileIndexDiffCWrap(ta.tile, TileIndexDiffCByDiagDir(ReverseDiagDir(far_dir)));
	TileIndex exit_far = AddTileIndexDiffCWrap(far_end, TileIndexDiffCByDiagDir(far_dir));
	bool near_ok = exit_near != INVALID_TILE && IsRailBuildableTile(exit_near);
	bool far_ok = exit_far != INVALID_TILE && IsRailBuildableTile(exit_far);
	if (need_both ? (!near_ok || !far_ok) : (!near_ok && !far_ok)) return false;

	bool use_far = far_ok && (!near_ok || DistanceManhattan(exit_far, toward) < DistanceManhattan(exit_near, toward));
	site.tile = ta.tile;
	site.axis = axis;
	if (use_far) {
		site.exit = exit_far;
		site.exit_dir = far_dir;
		site.exit2 = exit_near;
		site.exit2_dir = ReverseDiagDir(far_dir);
	} else {
		site.exit = exit_near;
		site.exit_dir = ReverseDiagDir(far_dir);
		site.exit2 = exit_far;
		site.exit2_dir = far_dir;
	}
	return true;
}

/**
 * Platz für einen Bahnhof (1 Gleis, RAIL_PLATFORM_LEN Kacheln) nahe der
 * Stadt suchen. Das Anschluss-Ende wird Richtung \a toward gewählt.
 * @param need_both Ringbetrieb: beide Bahnsteig-Enden brauchen Anschluss.
 */
static RailSite FindRailStationSite(const Town *t, TileIndex toward, bool need_both, uint8_t numtracks)
{
	RailSite site;
	for (TileIndex tile : SpiralTileSequence(t->xy, 30)) {
		for (Axis axis : {Axis::X, Axis::Y}) {
			CommandCost res = Command<Commands::BuildRailStation>::Do({}, tile, RAILTYPE_RAIL,
					axis, numtracks, RAIL_PLATFORM_LEN, STAT_CLASS_DFLT, 0, NEW_STATION, false);
			if (!res.Succeeded()) continue;

			DiagDirection far_dir = (axis == Axis::X) ? DiagDirection::SW : DiagDirection::SE;
			TileIndex far_end = tile;
			for (uint i = 1; i < RAIL_PLATFORM_LEN; i++) {
				far_end = AddTileIndexDiffCWrap(far_end, TileIndexDiffCByDiagDir(far_dir));
				if (far_end == INVALID_TILE) break;
			}
			if (far_end == INVALID_TILE) continue;

			TileIndex exit_near = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDiagDir(ReverseDiagDir(far_dir)));
			TileIndex exit_far = AddTileIndexDiffCWrap(far_end, TileIndexDiffCByDiagDir(far_dir));
			bool near_ok = exit_near != INVALID_TILE && IsRailBuildableTile(exit_near);
			bool far_ok = exit_far != INVALID_TILE && IsRailBuildableTile(exit_far);
			if (need_both ? (!near_ok || !far_ok) : (!near_ok && !far_ok)) continue;

			/* Primäranschluss Richtung Ziel wählen. */
			bool use_far = far_ok && (!near_ok || DistanceManhattan(exit_far, toward) < DistanceManhattan(exit_near, toward));
			site.tile = tile;
			site.axis = axis;
			if (use_far) {
				site.exit = exit_far;
				site.exit_dir = far_dir;
				site.exit2 = exit_near;
				site.exit2_dir = ReverseDiagDir(far_dir);
			} else {
				site.exit = exit_near;
				site.exit_dir = ReverseDiagDir(far_dir);
				site.exit2 = exit_far;
				site.exit2_dir = far_dir;
			}
			return site;
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
		bool flat_here = IsTileFlat(n.tile);
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
				if (d != n.dir && !flat_here) step += 16; /* Kurve auf Hang: riskant, stark verteuern */
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

	if (goal.first == INVALID_TILE) {
		Debug(misc, 0, "AC: no rail path from 0x{:X} to 0x{:X} ({} expansions)", from.exit.base(), to.exit.base(), expanded);
		return {};
	}
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

/** Waggon der gewünschten Frachtklasse mit größter Kapazität wählen. */
static EngineID FindBestWagon(CargoClasses cargo_class)
{
	EngineID best = EngineID::Invalid();
	uint best_capacity = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		const RailVehicleInfo &rvi = e->VehInfo<RailVehicleInfo>();
		if (rvi.railveh_type != RailVehicleType::Wagon) continue;
		if (!rvi.railtypes.Test(RAILTYPE_RAIL)) continue;
		if (!IsCargoInClass(e->GetDefaultCargoType(), cargo_class)) continue;
		uint cap = e->GetDisplayDefaultCapacity();
		if (cap > best_capacity) {
			best_capacity = cap;
			best = e->index;
		}
	}
	return best;
}

/**
 * Einseitiges Pfadsignal in Fahrtrichtung \a d auf gerader Gleiskachel
 * setzen (Zyklenzahl gemäß Signalseiten-Logik der Engine).
 */
static void BuildLoopSignal(TileIndex tile, DiagDirection d)
{
	Track track = (DiagDirToAxis(d) == Axis::X) ? Track::X : Track::Y;
	uint8_t cycles = (d == DiagDirection::SW || d == DiagDirection::NW) ? 1 : 0;
	Command<Commands::BuildSignal>::Do(DoCommandFlag::Execute, tile, track, SignalType::PathOneWay,
			SignalVariant::Electric, false, false, false, SignalType::Block, SignalType::Block, cycles, 0);
}

/**
 * Eine Gleislinie entlang eines A*-Pfads bauen (inkl. Brücken).
 * @param final_exit_edge Kante der letzten Kachel Richtung Ziel-Bahnsteig.
 * @param signals Einseitige Pfadsignale in Fahrtrichtung setzen (Ringbetrieb).
 * @return false bei Baufehler (result.error ist dann gesetzt).
 */
static bool BuildRailLine(const std::vector<std::pair<TileIndex, DiagDirection>> &path, DiagDirection final_exit_edge, bool signals, DoCommandFlags do_flags, AutoConnectResult &result)
{
	/* Brückenkacheln: aufeinanderfolgende Zustände mit Abstand > 1. */
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
			CommandCost c = Command<Commands::BuildBridge>::Do(do_flags, path[i + 1].first, path[i].first,
					TransportType::Rail, bt < MAX_BRIDGES ? bt : 0, RAILTYPE_RAIL, INVALID_ROADTYPE);
			if (c.Failed()) {
				result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
				result.error_detail = c.GetErrorMessage().base();
				return false;
			}
			result.cost += c.GetCost();
			continue;
		}
		if (bridge_heads.count(i) != 0) continue; /* Rampe liefert das Gleis */
		DiagDirection entry_edge = ReverseDiagDir(path[i].second);
		DiagDirection exit_edge = (i + 1 < path.size()) ? path[i + 1].second : final_exit_edge;
		Track track = TrackFromEdges(entry_edge, exit_edge);
		CommandCost c = Command<Commands::BuildRail>::Do(do_flags, path[i].first, RAILTYPE_RAIL, track, true);
		if (c.Failed() && i > 0 && do_flags.Test(DoCommandFlag::Execute)) {
			/* Unpassender Hang: Kachel auf die Hoehe der vorherigen
			 * Pfadkachel planieren und erneut versuchen. */
			auto [lc, lmoney, ltile] = Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, path[i].first, path[i - 1].first, false, LevelMode::Level);
			if (lc.Succeeded()) result.cost += lc.GetCost();
			c = Command<Commands::BuildRail>::Do(do_flags, path[i].first, RAILTYPE_RAIL, track, true);
		}
		if (c.Failed()) {
			result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
			result.error_detail = c.GetErrorMessage().base();
			return false;
		}
		result.cost += c.GetCost();
	}

	/* Signale in Fahrtrichtung, alle paar Kacheln auf geraden Stücken. */
	if (signals) {
		uint since = 3;
		for (size_t i = 1; i + 1 < path.size(); i++) {
			since++;
			if (since < 4) continue;
			if (bridge_heads.count(i) != 0) continue;
			if (path[i].second != path[i + 1].second) continue; /* nur gerade Kacheln */
			BuildLoopSignal(path[i].first, path[i].second); /* Fehlschlag (z. B. Bahnübergang) ist ok */
			since = 0;
		}
	}
	return true;
}

/**
 * Weiche am Bahnsteig-Ende: verbindet das zweite Gleis eines
 * 2-gleisigen Bahnhofs über eine Kurvenkachel mit der Ausfahrkachel,
 * so können wartende Züge einander ausweichen. Bester-Versuch-Bau.
 */
static void BuildStationFan(TileIndex exit_tile, DiagDirection exit_dir, DiagDirection perp, DiagDirection line_edge, AutoConnectResult &result)
{
	if (line_edge == perp) return; /* Strecke biegt zur Fächerseite ab */
	TileIndex fan = AddTileIndexDiffCWrap(exit_tile, TileIndexDiffCByDiagDir(perp));
	if (fan == INVALID_TILE) return;
	/* Nur sinnvoll, wenn dahinter wirklich das zweite Gleis liegt. */
	TileIndex platform2 = AddTileIndexDiffCWrap(fan, TileIndexDiffCByDiagDir(ReverseDiagDir(exit_dir)));
	if (platform2 == INVALID_TILE || !IsTileType(platform2, TileType::Station)) return;
	CommandCost c1 = Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, fan, RAILTYPE_RAIL,
			TrackFromEdges(ReverseDiagDir(exit_dir), ReverseDiagDir(perp)), true);
	if (c1.Failed()) return;
	result.cost += c1.GetCost();
	CommandCost c2 = Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, exit_tile, RAILTYPE_RAIL,
			TrackFromEdges(perp, line_edge), true);
	if (c2.Succeeded()) result.cost += c2.GetCost();
}

/**
 * Zugverbindung bauen. Ein Zug: einfache Pendelstrecke. Mehrere Züge:
 * Einbahn-Ring über beide Bahnhöfe (2 Gleise je Bahnhof, Weichen an
 * den Enden) mit einseitigen Pfadsignalen.
 */
static AutoConnectResult BuildRailConnection(Town *town_a, Town *town_b, uint train_count, uint8_t cargo_mode, bool estimate)
{
	static const uint TRAIN_WAGONS = 3; ///< Waggons je Zug.
	AutoConnectResult result;
	DoCommandFlags do_flags = estimate ? DoCommandFlags{} : DoCommandFlags{DoCommandFlag::Execute};
	Backup<CompanyID> cur_company(_current_company, _local_company);
	bool loop = train_count > 1;
	uint8_t numtracks = loop ? 2 : 1;

	/* Bestehende eigene Bahnhöfe in Stadtnähe wiederverwenden. */
	Station *re_a = FindNearbyOwnStation(town_a, StationFacility::Train, 25);
	Station *re_b = FindNearbyOwnStation(town_b, StationFacility::Train, 25);
	if (re_a != nullptr && re_a == re_b) re_b = nullptr;

	RailSite site_a, site_b;
	bool build_st_a = re_a == nullptr || !RailSiteFromStation(re_a, town_b->xy, loop, site_a);
	bool build_st_b = re_b == nullptr || !RailSiteFromStation(re_b, town_a->xy, loop, site_b);
	if (build_st_a) site_a = FindRailStationSite(town_a, town_b->xy, loop, numtracks);
	if (build_st_b) site_b = FindRailStationSite(town_b, town_a->xy, loop, numtracks);
	if (site_a.tile == INVALID_TILE || site_b.tile == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_RAIL_SITE;
		cur_company.Restore();
		return result;
	}

	/* Bahnhöfe zuerst bauen, damit die Wegsuche sie als Hindernis kennt. */
	for (const auto &[site, build] : {std::pair{&site_a, build_st_a}, std::pair{&site_b, build_st_b}}) {
		if (!build) continue;
		CommandCost c = Command<Commands::BuildRailStation>::Do(do_flags, site->tile,
				RAILTYPE_RAIL, site->axis, numtracks, RAIL_PLATFORM_LEN, STAT_CLASS_DFLT, 0, NEW_STATION, false);
		if (c.Failed()) {
			Town *dbg = ClosestTownFromTile(site->tile, _settings_game.economy.dist_local_authority);
			Debug(misc, 0, "AC: rail station refused at 0x{:X} err {} town {} rating {}",
					site->tile.base(), c.GetErrorMessage().base(),
					dbg != nullptr ? dbg->index.base() : 0xFFFF,
					dbg != nullptr ? dbg->ratings[_local_company] : -9999);
			/* Der Spieler hat den Bau angewiesen - Stadtbewertung nicht blockieren lassen. */
			c = Command<Commands::BuildRailStation>::Do(DoCommandFlags{DoCommandFlag::NoTestTownRating} | do_flags, site->tile,
					RAILTYPE_RAIL, site->axis, numtracks, RAIL_PLATFORM_LEN, STAT_CLASS_DFLT, 0, NEW_STATION, false);
		}
		if (c.Failed()) {
			result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
			result.error_detail = c.GetErrorMessage().base();
			cur_company.Restore();
			return result;
		}
		result.cost += c.GetCost();
	}

	/* Hinweg suchen und bauen. */
	auto path = FindRailPath(site_a, site_b);
	if (path.empty()) {
		result.error = STR_AUTOCONNECT_ERR_NO_RAIL_PATH;
		cur_company.Restore();
		return result;
	}
	if (!BuildRailLine(path, ReverseDiagDir(site_b.exit_dir), loop && !estimate, do_flags, result)) {
		cur_company.Restore();
		return result;
	}
	if (loop && !estimate) {
		DiagDirection perp_a = (site_a.axis == Axis::X) ? DiagDirection::SE : DiagDirection::SW;
		DiagDirection perp_b = (site_b.axis == Axis::X) ? DiagDirection::SE : DiagDirection::SW;
		DiagDirection out_edge = (path.size() > 1) ? path[1].second : ReverseDiagDir(site_b.exit_dir);
		BuildStationFan(site_a.exit, site_a.exit_dir, perp_a, out_edge, result);
		BuildStationFan(site_b.exit, site_b.exit_dir, perp_b, ReverseDiagDir(path.back().second), result);
	}

	/* Ringbetrieb: Rückweg vom anderen Bahnsteig-Ende zurück. */
	if (loop) {
		RailSite from2, to2;
		from2.exit = site_b.exit2;
		from2.exit_dir = site_b.exit2_dir;
		to2.exit = site_a.exit2;
		to2.exit_dir = site_a.exit2_dir;
		auto path2 = FindRailPath(from2, to2);
		if (path2.empty()) {
			result.error = STR_AUTOCONNECT_ERR_NO_RAIL_PATH;
			cur_company.Restore();
			return result;
		}
		if (!BuildRailLine(path2, ReverseDiagDir(site_a.exit2_dir), !estimate, do_flags, result)) {
			cur_company.Restore();
			return result;
		}
		if (!estimate) {
			DiagDirection perp_a = (site_a.axis == Axis::X) ? DiagDirection::SE : DiagDirection::SW;
			DiagDirection perp_b = (site_b.axis == Axis::X) ? DiagDirection::SE : DiagDirection::SW;
			DiagDirection out_edge2 = (path2.size() > 1) ? path2[1].second : ReverseDiagDir(site_a.exit2_dir);
			BuildStationFan(site_b.exit2, site_b.exit2_dir, perp_b, out_edge2, result);
			BuildStationFan(site_a.exit2, site_a.exit2_dir, perp_a, ReverseDiagDir(path2.back().second), result);
		}
	}

	EngineID engine = FindBestTrainEngine();
	if (engine == EngineID::Invalid()) {
		result.error = STR_AUTOCONNECT_ERR_NO_VEHICLE;
		cur_company.Restore();
		return result;
	}
	/* Waggonwahl: bei "beides" zwei Passagier- und ein Postwaggon. */
	EngineID w_pax = FindBestWagon({CargoClass::Passengers});
	EngineID w_mail = FindBestWagon({CargoClass::Mail});
	auto pick_wagon = [&](uint j) {
		bool want_mail = cargo_mode == 1 || (cargo_mode == 2 && j == TRAIN_WAGONS - 1);
		EngineID e = want_mail ? w_mail : w_pax;
		if (e == EngineID::Invalid()) e = want_mail ? w_pax : w_mail;
		return e;
	};

	if (estimate) {
		Money per_train = Engine::Get(engine)->GetCost();
		for (uint j = 0; j < TRAIN_WAGONS; j++) {
			if (pick_wagon(j) != EngineID::Invalid()) per_train += Engine::Get(pick_wagon(j))->GetCost();
		}
		result.cost += per_train * train_count;
		cur_company.Restore();
		result.ok = true;
		return result;
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

	for (uint t = 0; t < train_count; t++) {
		auto [cost_v, veh_id, refit_capacity, refit_mail, cargo_capacities] =
				Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, engine, true, INVALID_CARGO, ClientID::Invalid);
		if (cost_v.Failed()) {
			if (t == 0) {
				result.error = STR_AUTOCONNECT_ERR_NO_MONEY_VEHICLE;
				cur_company.Restore();
				return result;
			}
			break;
		}
		result.cost += cost_v.GetCost();

		for (uint i = 0; i < TRAIN_WAGONS; i++) {
			EngineID wagon = pick_wagon(i);
			if (wagon == EngineID::Invalid()) continue;
			auto [cost_w, wagon_id, rw1, rw2, rw3] =
					Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, wagon, true, INVALID_CARGO, ClientID::Invalid);
			if (cost_w.Failed()) break;
			result.cost += cost_w.GetCost();
			Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, wagon_id, veh_id, false);
		}

		CommandCost co_a = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, MakeStationOrder(st_a->index));
		if (co_a.Failed()) result.error_detail = co_a.GetErrorMessage().base();
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, MakeStationOrder(st_b->index));
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_id, false);
	}

	cur_company.Restore();
	result.ok = true;
	return result;
}

/* ------------------------- Schiffsverbindung (Stufe 3) ---------------- */

/** Küstenplatz für einen Hafen nahe der Stadt suchen. */
static TileIndex FindDockSite(const Town *t)
{
	for (TileIndex tile : SpiralTileSequence(t->xy, 40)) {
		CommandCost res = Command<Commands::BuildDock>::Do({}, tile, NEW_STATION, false);
		if (res.Succeeded()) return tile;
	}
	return INVALID_TILE;
}

/** Bestes verfügbares Schiff der Frachtklasse wählen. */
static EngineID FindBestShip(CargoClasses cargo_class)
{
	EngineID best = EngineID::Invalid();
	uint best_capacity = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Ship)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (!IsCargoInClass(e->GetDefaultCargoType(), cargo_class)) continue;
		uint cap = e->GetDisplayDefaultCapacity();
		if (cap > best_capacity) {
			best_capacity = cap;
			best = e->index;
		}
	}
	return best;
}

/**
 * Schiffsverbindung bauen: zwei Häfen, Schiffsdepot, N Schiffe.
 * Den Wasserweg finden die Schiffe selbst.
 */
static AutoConnectResult BuildShipConnection(Town *town_a, Town *town_b, uint count, uint8_t cargo_mode, bool estimate)
{
	AutoConnectResult result;
	DoCommandFlags do_flags = estimate ? DoCommandFlags{} : DoCommandFlags{DoCommandFlag::Execute};
	Backup<CompanyID> cur_company(_current_company, _local_company);

	TileIndex dock_a = FindDockSite(town_a);
	if (dock_a != INVALID_TILE) {
		CommandCost c = Command<Commands::BuildDock>::Do(do_flags, dock_a, NEW_STATION, false);
		if (c.Failed()) dock_a = INVALID_TILE; else result.cost += c.GetCost();
	}
	TileIndex dock_b = dock_a == INVALID_TILE ? INVALID_TILE : FindDockSite(town_b);
	if (dock_b != INVALID_TILE) {
		CommandCost c = Command<Commands::BuildDock>::Do(do_flags, dock_b, NEW_STATION, false);
		if (c.Failed()) dock_b = INVALID_TILE; else result.cost += c.GetCost();
	}
	if (dock_a == INVALID_TILE || dock_b == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_DOCK_SITE;
		cur_company.Restore();
		return result;
	}

	EngineID s_pax = FindBestShip({CargoClass::Passengers});
	EngineID s_mail = FindBestShip({CargoClass::Mail});
	auto pick_ship = [&](uint i) {
		bool want_mail = cargo_mode == 1 || (cargo_mode == 2 && i % 2 == 1);
		EngineID e = want_mail ? s_mail : s_pax;
		if (e == EngineID::Invalid()) e = want_mail ? s_pax : s_mail;
		return e;
	};
	if (pick_ship(0) == EngineID::Invalid()) {
		result.error = STR_AUTOCONNECT_ERR_NO_VEHICLE;
		cur_company.Restore();
		return result;
	}

	/* Schiffsdepot auf Wasser nahe Hafen A. */
	TileIndex depot = INVALID_TILE;
	for (TileIndex tile : SpiralTileSequence(dock_a, 12)) {
		for (Axis axis : {Axis::X, Axis::Y}) {
			CommandCost c = Command<Commands::BuildShipDepot>::Do(do_flags, tile, axis);
			if (c.Succeeded()) {
				result.cost += c.GetCost();
				depot = tile;
				break;
			}
		}
		if (depot != INVALID_TILE) break;
	}
	if (depot == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_SHIP_DEPOT;
		cur_company.Restore();
		return result;
	}

	if (estimate) {
		for (uint i = 0; i < count; i++) result.cost += Engine::Get(pick_ship(i))->GetCost();
		cur_company.Restore();
		result.ok = true;
		return result;
	}

	Station *st_a = Station::GetByTile(dock_a);
	Station *st_b = Station::GetByTile(dock_b);

	for (uint i = 0; i < count; i++) {
		auto [cost_v, veh_id, refit_capacity, refit_mail, cargo_capacities] =
				Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, pick_ship(i), true, INVALID_CARGO, ClientID::Invalid);
		if (cost_v.Failed()) {
			if (i == 0) {
				result.error = STR_AUTOCONNECT_ERR_NO_MONEY_VEHICLE;
				cur_company.Restore();
				return result;
			}
			break;
		}
		result.cost += cost_v.GetCost();

		CommandCost co_a = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, MakeStationOrder(st_a->index));
		if (co_a.Failed()) result.error_detail = co_a.GetErrorMessage().base();
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, MakeStationOrder(st_b->index));
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_id, false);
	}

	cur_company.Restore();
	result.ok = true;
	return result;
}

/** Fenster der Auto-Verbindung. */
struct AutoConnectWindow : Window {
	TownID town_a = TownID::Invalid();
	TownID town_b = TownID::Invalid();
	uint count = 2;
	uint8_t mode = 0; ///< 0 = Flugzeuge, 1 = Busse, 2 = Zug, 3 = Schiffe.
	uint8_t cargo = 0; ///< 0 = Passagiere, 1 = Post, 2 = beides.
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
				static const StringID counts[] = {STR_AUTOCONNECT_COUNT, STR_AUTOCONNECT_COUNT_BUS, STR_AUTOCONNECT_COUNT_TRAIN, STR_AUTOCONNECT_COUNT_SHIP};
				return GetString(counts[this->mode], this->count);
			}
			case WID_AC_MODE: {
				static const StringID modes[] = {STR_AUTOCONNECT_MODE_AIR, STR_AUTOCONNECT_MODE_BUS, STR_AUTOCONNECT_MODE_TRAIN, STR_AUTOCONNECT_MODE_SHIP};
				return GetString(modes[this->mode]);
			}
			case WID_AC_CARGO: {
				static const StringID cargos[] = {STR_AUTOCONNECT_CARGO_PAX, STR_AUTOCONNECT_CARGO_MAIL, STR_AUTOCONNECT_CARGO_BOTH};
				return GetString(cargos[this->cargo]);
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
				this->mode = (this->mode + 1) % 4;
				this->SetDirty();
				break;

			case WID_AC_CARGO:
				this->cargo = (this->cargo + 1) % 3;
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

			case WID_AC_ESTIMATE:
			case WID_AC_BUILD: {
				bool estimate = widget == WID_AC_ESTIMATE;
				if (_networking) {
					ShowErrorMessage(GetEncodedString(STR_AUTOCONNECT_ERR_SINGLEPLAYER), {}, WarningLevel::Info);
					break;
				}
				if (this->town_a == TownID::Invalid() || this->town_b == TownID::Invalid() || this->town_a == this->town_b) {
					this->status = GetString(STR_AUTOCONNECT_STATUS_NEED_TOWNS);
					this->SetDirty();
					break;
				}
				auto run = [&](bool est) {
					switch (this->mode) {
						case 1: return BuildRoadConnection(Town::Get(this->town_a), Town::Get(this->town_b), this->count, this->cargo, est);
						case 2: return BuildRailConnection(Town::Get(this->town_a), Town::Get(this->town_b), this->count, this->cargo, est);
						case 3: return BuildShipConnection(Town::Get(this->town_a), Town::Get(this->town_b), this->count, this->cargo, est);
						default: return BuildAirConnection(Town::Get(this->town_a), Town::Get(this->town_b), this->count, est);
					}
				};

				if (!estimate) {
					/* Vorab schaetzen: nicht anfangen, wenn das Geld (inkl.
					 * Kreditrahmen) nicht reicht - halbe Bauten nutzen niemandem. */
					AutoConnectResult est_res = run(true);
					if (est_res.ok) {
						Money needed = est_res.cost + est_res.cost / 4; /* +25 % Puffer */
						const Company *c = Company::Get(_local_company);
						Money avail = c->money + (c->GetMaxLoan() - c->current_loan);
						if (avail < needed) {
							this->status = GetString(STR_AUTOCONNECT_ERR_TOO_EXPENSIVE, needed);
							this->SetDirty();
							break;
						}
						if (c->money < needed) {
							Backup<CompanyID> cur_company(_current_company, _local_company);
							Command<Commands::IncreaseLoan>::Do(DoCommandFlag::Execute, LoanCommand::Amount, needed - c->money);
							cur_company.Restore();
						}
					}
				}

				AutoConnectResult res = run(estimate);
				if (res.ok) {
					this->status = GetString(estimate ? STR_AUTOCONNECT_STATUS_ESTIMATE : STR_AUTOCONNECT_STATUS_DONE, res.cost);
					if (res.error_detail != 0) this->status += fmt::format(" [#{}]", res.error_detail);
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
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_CARGO), SetFill(1, 0), SetMinimalSize(220, 14), SetToolTip(STR_AUTOCONNECT_CARGO_TOOLTIP),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_COUNT_DOWN), SetMinimalSize(20, 14), SetStringTip(STR_AUTOCONNECT_MINUS),
				NWidget(WWT_TEXT, Colours::Invalid, WID_AC_COUNT), SetFill(1, 0), SetAlignment({AlignmentH::Centre, AlignmentV::Middle}),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_COUNT_UP), SetMinimalSize(20, 14), SetStringTip(STR_AUTOCONNECT_PLUS),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_ESTIMATE), SetFill(1, 0), SetMinimalSize(106, 16), SetStringTip(STR_AUTOCONNECT_ESTIMATE, STR_AUTOCONNECT_ESTIMATE_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Green, WID_AC_BUILD), SetFill(1, 0), SetMinimalSize(106, 16), SetStringTip(STR_AUTOCONNECT_BUILD, STR_AUTOCONNECT_BUILD_TOOLTIP),
			EndContainer(),
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
