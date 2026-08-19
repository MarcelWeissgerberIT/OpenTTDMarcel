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
#include "depot_base.h"
#include "zoom_func.h"
#include "landscape_cmd.h"
#include "industry.h"
#include "water_map.h"
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

#include "autoreplace_func.h"
#include "autoreplace_cmd.h"
#include "group.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"

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
static Station *FindNearbyOwnStation(TileIndex center, StationFacility fac, uint radius)
{
	Station *best = nullptr;
	uint best_dist = radius + 1;
	for (Station *st : Station::Iterate()) {
		if (st->owner != _local_company) continue;
		if (!st->facilities.Test(fac)) continue;
		uint d = DistanceManhattan(st->xy, center);
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

/* ============ Fork: Einstellungen, Staffelstart, Bau-Rollback ============ */

static bool _ac_allow_terraform = true; ///< Flaechen fuer Stationen/Flughaefen planieren.
static bool _ac_big_airports = true;    ///< Groessten verfuegbaren Flughafen bevorzugen.

/** Gestaffelter Start: Fahrzeug i faehrt i*4 Tage nach dem ersten los. */
struct AcDelayedStart {
	VehicleID vehicle;
	int days;
};
static std::vector<AcDelayedStart> _ac_delayed_starts;

static void AcStartStaggered(VehicleID veh, uint index)
{
	if (index == 0) {
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh, false);
	} else {
		_ac_delayed_starts.push_back({veh, (int)index * 4});
	}
}

static const IntervalTimer<TimerGameCalendar> _ac_stagger_timer = {{TimerGameCalendar::Trigger::Day, TimerGameCalendar::Priority::None}, [](auto) {
	for (auto it = _ac_delayed_starts.begin(); it != _ac_delayed_starts.end();) {
		const Vehicle *v = Vehicle::GetIfValid(it->vehicle);
		if (v == nullptr) {
			it = _ac_delayed_starts.erase(it);
			continue;
		}
		if (--it->days > 0) {
			++it;
			continue;
		}
		if (v->vehstatus.Test(VehState::Stopped)) {
			Backup<CompanyID> cur_company(_current_company, v->owner);
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, it->vehicle, false);
			cur_company.Restore();
		}
		it = _ac_delayed_starts.erase(it);
	}
}};

/* Bau-Log: Bei einem Abbruch mitten im Bau werden alle bereits gebauten
 * Kacheln wieder abgerissen, statt halbe Ruinen stehen zu lassen. */
static std::vector<TileIndex> _ac_build_log;
static bool _ac_log_active = false;

static void AcLogBegin() { _ac_build_log.clear(); _ac_log_active = true; }
static void AcLogEnd() { _ac_log_active = false; _ac_build_log.clear(); }
static void AcLogTile(TileIndex t) { if (_ac_log_active) _ac_build_log.push_back(t); }

static void AcRollback()
{
	if (!_ac_log_active) return;
	_ac_log_active = false;
	for (auto it = _ac_build_log.rbegin(); it != _ac_build_log.rend(); ++it) {
		Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute, *it);
	}
	_ac_build_log.clear();
}

/**
 * Flughafentyp-Kandidaten, groesster zuerst (je nach Einstellung und
 * Jahr/Verfuegbarkeit); AT_SMALL ist immer der letzte Rueckfall.
 */
static std::vector<uint8_t> AcAirportCandidates()
{
	std::vector<uint8_t> out;
	if (_ac_big_airports) {
		for (uint8_t at : {(uint8_t)AT_INTERNATIONAL, (uint8_t)AT_METROPOLITAN, (uint8_t)AT_LARGE, (uint8_t)AT_COMMUTER}) {
			if (AirportSpec::Get(at)->IsAvailable()) out.push_back(at);
		}
	}
	if (AirportSpec::Get(AT_SMALL)->IsAvailable()) out.push_back(AT_SMALL);
	return out;
}

/**
 * Freie Stelle für einen Flughafen des Typs nahe der Stadt suchen.
 * @return Bauplatz-Tile oder INVALID_TILE.
 */
static TileIndex FindAirportSite(const Town *t, uint8_t type)
{
	for (TileIndex tile : SpiralTileSequence(t->xy, 40)) {
		CommandCost res = Command<Commands::BuildAirport>::Do({}, tile, type, 0, NEW_STATION, false);
		if (res.Succeeded()) return tile;
	}
	return INVALID_TILE;
}

/**
 * Notfall-Flughafenbau mit Landanpassung: Fläche in Typ-Groesse
 * nahe der Stadt planieren und dort bauen. Nur im Execute-Modus.
 * @return Bauplatz oder INVALID_TILE.
 */
static TileIndex BuildAirportWithTerraform(const Town *t, uint8_t type, Money reserve, AutoConnectResult &result)
{
	const AirportSpec *as = AirportSpec::Get(type);
	uint attempts = 0;
	for (TileIndex tile : SpiralTileSequence(t->xy, 40)) {
		TileIndex end = AddTileIndexDiffCWrap(tile, TileIndexDiffC{(int16_t)(as->size_x - 1), (int16_t)(as->size_y - 1)});
		if (end == INVALID_TILE) continue;
		/* Erst testen, ob die Fläche überhaupt planierbar ist - und ob nach
		 * der Planierung die Reserve (Flugzeuge) noch bezahlbar waere. Ohne
		 * diese Bremse verbrennt die Suche das halbe Konto in Planierungen
		 * und meldet am Ende trotzdem einen Fehlschlag. */
		auto [tc, tmoney, ttile] = Command<Commands::LevelLand>::Do({}, end, tile, false, LevelMode::Level);
		if (tc.Failed()) continue;
		if (Company::Get(_local_company)->money < tc.GetCost() + reserve) return INVALID_TILE;
		if (++attempts > 25) break; /* Kosten begrenzen */
		auto [lc, lmoney, ltile] = Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, end, tile, false, LevelMode::Level);
		if (lc.Failed()) continue;
		result.cost += lc.GetCost();
		CommandCost c = Command<Commands::BuildAirport>::Do(DoCommandFlags{DoCommandFlag::NoTestTownRating, DoCommandFlag::Execute}, tile, type, 0, NEW_STATION, false);
		if (c.Succeeded()) {
			result.cost += c.GetCost();
			return tile;
		}
	}
	return INVALID_TILE;
}

/**
 * Flughafen nahe einer Stadt bauen: probiert die Typen von gross nach
 * klein, erst auf passendem Gelaende, dann (falls erlaubt) mit
 * Planierung. Gebaut wird sofort; Rueckgabe ist die Bauplatz-Kachel.
 */
static TileIndex AcBuildAirportNear(const Town *t, bool estimate, Money reserve, AutoConnectResult &result, bool &blocked_by_money)
{
	DoCommandFlags do_flags = estimate ? DoCommandFlags{} : DoCommandFlags{DoCommandFlag::Execute};
	for (uint8_t at : AcAirportCandidates()) {
		TileIndex site = FindAirportSite(t, at);
		if (site == INVALID_TILE) continue;
		/* Kostenvoranschlag: Nach dem Flughafen muss die Reserve (Flugzeuge,
		 * zweiter Flughafen) noch bezahlbar sein - sonst kleineren Typ nehmen. */
		if (!estimate) {
			CommandCost probe = Command<Commands::BuildAirport>::Do({}, site, at, 0, NEW_STATION, false);
			if (probe.Succeeded() && Company::Get(_local_company)->money < probe.GetCost() + reserve) {
				Debug(misc, 0, "AC: airport type {} too expensive with reserve", at);
				blocked_by_money = true;
				continue;
			}
		}
		CommandCost c = Command<Commands::BuildAirport>::Do(do_flags, site, at, 0, NEW_STATION, false);
		if (c.Failed()) {
			/* Stadtbewertung blockiert? Der Spieler hat den Bau angewiesen. */
			c = Command<Commands::BuildAirport>::Do(DoCommandFlags{DoCommandFlag::NoTestTownRating} | do_flags, site, at, 0, NEW_STATION, false);
		}
		if (c.Failed()) {
			Debug(misc, 0, "AC: airport type {} failed at 0x{:X} err {}", at, site.base(), c.GetErrorMessage().base());
			continue; /* z. B. zu teuer -> kleineren Typ probieren */
		}
		result.cost += c.GetCost();
		AcLogTile(site);
		return site;
	}
	if (!estimate && _ac_allow_terraform) {
		for (uint8_t at : AcAirportCandidates()) {
			TileIndex site = BuildAirportWithTerraform(t, at, reserve, result);
			if (site == INVALID_TILE) continue;
			if (Company::Get(_local_company)->money < reserve) {
				blocked_by_money = true;
				/* Nach dem Bau bliebe kein Geld fuer die Flugzeuge:
				 * wieder abreissen und einen kleineren Typ versuchen. */
				Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute, site);
				continue;
			}
			AcLogTile(site);
			return site;
		}
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

	/* Sind die Städte weit genug auseinander für zwei getrennte Flughäfen? */
	if (DistanceManhattan(town_a->xy, town_b->xy) < 12) {
		result.error = STR_AUTOCONNECT_ERR_TOO_CLOSE;
		return result;
	}

	Backup<CompanyID> cur_company(_current_company, _local_company);

	/* Flugzeugkosten vorab kennen: Diese Reserve bleibt beim Flughafenbau
	 * unangetastet, sonst stehen teure Flughaefen ohne ein einziges
	 * Flugzeug in der Landschaft. */
	EngineID engine = FindBestAircraft();
	if (engine == EngineID::Invalid()) {
		result.error = STR_AUTOCONNECT_ERR_NO_VEHICLE;
		cur_company.Restore();
		return result;
	}
	Money aircraft_cost = Engine::Get(engine)->GetCost() * count;

	/* Bestehende eigene Flughäfen in Stadtnähe wiederverwenden. */
	Station *re_a = FindNearbyOwnStation(town_a->xy, StationFacility::Airport, 25);
	Station *re_b = FindNearbyOwnStation(town_b->xy, StationFacility::Airport, 25);
	if (re_a != nullptr && re_a == re_b) re_b = nullptr;

	if (!estimate) AcLogBegin();

	TileIndex site_a = INVALID_TILE;
	if (re_a == nullptr) {
		/* Flughafen A: groessten passenden Typ bauen — erst danach für B
		 * suchen, damit die Platzsuche für B den neuen Flughafen A kennt.
		 * Reserve: Flugzeuge + Platz fuer den zweiten (kleinen) Flughafen. */
		bool blocked_a = false;
		site_a = AcBuildAirportNear(town_a, estimate, aircraft_cost + (re_b == nullptr ? 25000 : 0), result, blocked_a);
		if (site_a == INVALID_TILE) {
			result.error = blocked_a ? STR_AUTOCONNECT_ERR_NO_MONEY_TOTAL : STR_AUTOCONNECT_ERR_NO_AIRPORT_SITE;
			AcRollback();
			cur_company.Restore();
			return result;
		}
	}

	TileIndex site_b = INVALID_TILE;
	if (re_b == nullptr) {
		bool blocked_b = false;
		site_b = AcBuildAirportNear(town_b, estimate, aircraft_cost, result, blocked_b);
		if (site_b == INVALID_TILE) {
			result.error = blocked_b ? STR_AUTOCONNECT_ERR_NO_MONEY_TOTAL : STR_AUTOCONNECT_ERR_NO_AIRPORT_SITE;
			AcRollback();
			cur_company.Restore();
			return result;
		}
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
		AcRollback();
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
				AcRollback();
				cur_company.Restore();
				return result;
			}
			break;
		}
		result.cost += cost_v.GetCost();

		CommandCost co_a = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, MakeStationOrder(st_a->index));
		if (co_a.Failed()) result.error_detail = co_a.GetErrorMessage().base();
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, MakeStationOrder(st_b->index));

		AcStartStaggered(veh_id, i);
	}

	AcLogEnd();
	cur_company.Restore();
	result.ok = true;
	return result;
}

/* ------------------------- Busverbindung (Stufe 1b) ------------------- */

/**
 * Gerades Straßenstück in Stadtnähe finden, auf dem eine
 * Durchfahrt-Bushaltestelle gebaut werden kann.
 */
static TileIndex FindBusStopSite(const Town *t, Axis &axis_out, const std::vector<TileIndex> &avoid = {})
{
	for (TileIndex tile : SpiralTileSequence(t->xy, 24)) {
		if (!IsNormalRoadTile(tile)) continue;
		bool too_close = false;
		for (TileIndex a : avoid) {
			if (DistanceManhattan(tile, a) < 5) { too_close = true; break; }
		}
		if (too_close) continue;
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

/**
 * Busverbindung bauen: Straße, Haltestellen, Depot, N Busse.
 * @param stops_per_town Haltestellen je Stadt; 0 = automatisch nach Größe.
 */
static AutoConnectResult BuildRoadConnection(Town *town_a, Town *town_b, uint count, uint8_t cargo_mode, bool estimate, uint stops_per_town = 0)
{
	AutoConnectResult result;
	DoCommandFlags do_flags = estimate ? DoCommandFlags{} : DoCommandFlags{DoCommandFlag::Execute};
	Backup<CompanyID> cur_company(_current_company, _local_company);

	/* Bestehende eigene Haltestellen in Stadtnähe wiederverwenden. */
	Station *re_a = FindNearbyOwnStation(town_a->xy, StationFacility::BusStop, 20);
	Station *re_b = FindNearbyOwnStation(town_b->xy, StationFacility::BusStop, 20);
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

	/* Zusaetzliche Haltestellen in beiden Staedten, ueber die Stadt
	 * verteilt (Mindestabstand zueinander). Bester-Versuch: was nicht
	 * gebaut werden kann, wird uebersprungen. */
	std::vector<TileIndex> stops_town_a{stop_a}, stops_town_b{stop_b};
	for (auto [t, stops] : {std::pair{town_a, &stops_town_a}, std::pair{town_b, &stops_town_b}}) {
		uint want_stops = stops_per_town != 0 ? stops_per_town : ClampU(t->cache.population / 1200, 1, 3);
		while (stops->size() < want_stops) {
			Axis ax{};
			TileIndex extra = FindBusStopSite(t, ax, *stops);
			if (extra == INVALID_TILE) break;
			CommandCost c = Command<Commands::BuildRoadStop>::Do(DoCommandFlags{DoCommandFlag::NoTestTownRating} | do_flags, extra, 1, 1,
					RoadStopType::Bus, true, AxisToDiagDir(ax), ROADTYPE_ROAD,
					ROADSTOP_CLASS_DFLT, 0, NEW_STATION, false);
			if (c.Failed()) break;
			result.cost += c.GetCost();
			stops->push_back(extra);
		}
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

		/* Rundkurs ueber alle Halte: erst Stadt A, dann Stadt B. */
		uint order_no = 0;
		for (TileIndex st : stops_town_a) {
			CommandCost co = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, order_no++, MakeStationOrder(Station::GetByTile(st)->index));
			if (co.Failed() && order_no == 1) result.error_detail = co.GetErrorMessage().base();
		}
		for (TileIndex st : stops_town_b) {
			Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, order_no++, MakeStationOrder(Station::GetByTile(st)->index));
		}
		AcStartStaggered(veh_id, i);
	}
	(void)st_a; (void)st_b;

	cur_company.Restore();
	result.ok = true;
	return result;
}

/* ------------------------- Zugverbindung (Stufe 2) -------------------- */

static const uint RAIL_PLATFORM_LEN = 5; ///< Bahnsteiglänge in Kacheln (Lok + 3 Waggons passen ganz hinein).

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
 * Taugt die Kachel als Streckenausfahrt? Sie braucht neben der
 * Bahnhofsseite mindestens einen weiteren bebaubaren Nachbarn, sonst
 * beginnt die Wegsuche in einer Sackgasse.
 */
static bool IsUsableExit(TileIndex exit, DiagDirection exit_dir)
{
	if (exit == INVALID_TILE || !IsRailBuildableTile(exit)) return false;
	for (DiagDirection d = DiagDirection::Begin; d < DiagDirection::End; d++) {
		if (d == ReverseDiagDir(exit_dir)) continue; /* zurück zum Bahnhof */
		TileIndex n = AddTileIndexDiffCWrap(exit, TileIndexDiffCByDiagDir(d));
		if (n != INVALID_TILE && (IsRailBuildableTile(n) || IsTileType(n, TileType::Water))) return true;
	}
	return false;
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
	bool near_ok = IsUsableExit(exit_near, ReverseDiagDir(far_dir));
	bool far_ok = IsUsableExit(exit_far, far_dir);
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
static RailSite FindRailStationSite(TileIndex center, TileIndex toward, bool need_both, uint8_t numtracks, bool terraform = false, AutoConnectResult *result = nullptr)
{
	RailSite site;
	uint level_attempts = 0;
	for (TileIndex tile : SpiralTileSequence(center, 30)) {
		for (Axis axis : {Axis::X, Axis::Y}) {
			CommandCost res = Command<Commands::BuildRailStation>::Do({}, tile, RAILTYPE_RAIL,
					axis, numtracks, RAIL_PLATFORM_LEN, STAT_CLASS_DFLT, 0, NEW_STATION, false);
			if (!res.Succeeded() && terraform && level_attempts < 40) {
				/* Bahnhofsflaeche samt beider Ausfahrkacheln planieren
				 * und erneut versuchen (Marcel: nicht mitten im Bau
				 * aufgeben, sondern das Gelaende passend machen). */
				TileIndexDiffC pre = (axis == Axis::X) ? TileIndexDiffC{-1, 0} : TileIndexDiffC{0, -1};
				TileIndexDiffC post = (axis == Axis::X)
						? TileIndexDiffC{(int16_t)RAIL_PLATFORM_LEN, (int16_t)(numtracks - 1)}
						: TileIndexDiffC{(int16_t)(numtracks - 1), (int16_t)RAIL_PLATFORM_LEN};
				TileIndex a0 = AddTileIndexDiffCWrap(tile, pre);
				TileIndex a1 = AddTileIndexDiffCWrap(tile, post);
				if (a0 == INVALID_TILE || a1 == INVALID_TILE) continue;
				auto [tc, tmoney, ttile] = Command<Commands::LevelLand>::Do({}, a1, a0, false, LevelMode::Level);
				if (tc.Failed()) continue;
				level_attempts++;
				auto [lc, lmoney, ltile] = Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, a1, a0, false, LevelMode::Level);
				if (lc.Failed()) continue;
				if (result != nullptr) result->cost += lc.GetCost();
				res = Command<Commands::BuildRailStation>::Do({}, tile, RAILTYPE_RAIL,
						axis, numtracks, RAIL_PLATFORM_LEN, STAT_CLASS_DFLT, 0, NEW_STATION, false);
			}
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
			bool near_ok = IsUsableExit(exit_near, ReverseDiagDir(far_dir));
			bool far_ok = IsUsableExit(exit_far, far_dir);
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
	/* Zustand: Kachel, Einfahrtrichtung und die Richtung davor. Letztere
	 * erlaubt es, alternierende Richtungswechsel (= glatte 45-Grad-
	 * Diagonale im Spiel) billig zu machen, echte Kurven aber teuer. */
	using State = std::tuple<TileIndex, DiagDirection, DiagDirection>;
	struct Node { TileIndex tile; DiagDirection dir; DiagDirection prev; uint cost; uint est; };
	auto cmp = [](const Node &a, const Node &b) { return a.cost + a.est > b.cost + b.est; };
	std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
	std::map<State, State> came_from;
	std::map<State, uint> best;

	auto heuristic = [&](TileIndex t) { return DistanceManhattan(t, to.exit) * 4; };
	State start{from.exit, from.exit_dir, from.exit_dir};
	open.push({from.exit, from.exit_dir, from.exit_dir, 0, heuristic(from.exit)});
	best[start] = 0;
	uint expanded = 0;
	State goal{INVALID_TILE, DiagDirection::Invalid, DiagDirection::Invalid};

	while (!open.empty()) {
		Node n = open.top();
		open.pop();
		State cur{n.tile, n.dir, n.prev};
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
				State ns{land, d, d};
				auto it = best.find(ns);
				if (it != best.end() && it->second <= nc) continue;
				best[ns] = nc;
				came_from[ns] = cur;
				open.push({land, d, d, nc, heuristic(land)});
				continue;
			}

			uint step;
			if (IsRailBuildableTile(next)) {
				step = IsTileFlat(next) ? 8 : 14;
				if (d != n.dir) {
					/* Alternierender Wechsel setzt eine Diagonale fort (billig),
					 * alles andere ist eine echte Kurve (teuer). */
					step += (d == n.prev) ? 1 : 8;
					if (!flat_here) step += 16; /* Richtungswechsel auf Hang: riskant */
				}
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
			State ns{next, d, n.dir};
			auto it = best.find(ns);
			if (it != best.end() && it->second <= nc) continue;
			best[ns] = nc;
			came_from[ns] = cur;
			open.push({next, d, n.dir, nc, heuristic(next)});
		}
	}

	if (std::get<0>(goal) == INVALID_TILE) {
		Debug(misc, 0, "AC: no rail path from 0x{:X} to 0x{:X} ({} expansions)", from.exit.base(), to.exit.base(), expanded);
		return {};
	}
	std::vector<std::pair<TileIndex, DiagDirection>> path;
	for (State s = goal; !(s == start); s = came_from[s]) path.push_back({std::get<0>(s), std::get<1>(s)});
	path.push_back({std::get<0>(start), std::get<1>(start)});
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
 * Einseitiges Pfadsignal in Fahrtrichtung setzen — auch auf Kurven- und
 * Diagonalkacheln. Zyklenzahl je (Herkunftskante, Gleisstück) gemäß der
 * Trackdir-Tabelle der Script-API.
 */
static void BuildLoopSignal(TileIndex tile, DiagDirection entry_edge, DiagDirection exit_edge)
{
	Track track = TrackFromEdges(entry_edge, exit_edge);
	/* cycles[front][track]: front = Kante zur Herkunftskachel. -1 = ungueltig. */
	auto cyc = [](DiagDirection front, Track t) -> int {
		switch (front) {
			case DiagDirection::NW: return t == Track::Upper ? 0 : t == Track::Y ? 0 : t == Track::Left ? 1 : -1;
			case DiagDirection::NE: return t == Track::Right ? 1 : t == Track::X ? 1 : t == Track::Upper ? 1 : -1;
			case DiagDirection::SW: return t == Track::Lower ? 0 : t == Track::X ? 0 : t == Track::Left ? 0 : -1;
			case DiagDirection::SE: return t == Track::Right ? 0 : t == Track::Y ? 1 : t == Track::Lower ? 1 : -1;
			default: return -1;
		}
	};
	int cycles = cyc(entry_edge, track);
	if (cycles < 0) return;
	Command<Commands::BuildSignal>::Do(DoCommandFlag::Execute, tile, track, SignalType::PathOneWay,
			SignalVariant::Electric, false, false, false, SignalType::Block, SignalType::Block, static_cast<uint8_t>(cycles), 0);
}

/**
 * Eine Gleislinie entlang eines A*-Pfads bauen (inkl. Brücken).
 * @param final_exit_edge Kante der letzten Kachel Richtung Ziel-Bahnsteig.
 * @param signals Einseitige Pfadsignale in Fahrtrichtung setzen (Ringbetrieb).
 * @return false bei Baufehler (result.error ist dann gesetzt).
 */
static bool BuildRailLine(const std::vector<std::pair<TileIndex, DiagDirection>> &path, DiagDirection final_exit_edge, [[maybe_unused]] bool signals, DoCommandFlags do_flags, AutoConnectResult &result)
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
			if (c.Failed() && i > 0 && do_flags.Test(DoCommandFlag::Execute)) {
				/* Rampen-Hang unpassend: beide Brueckenkoepfe planieren. */
				auto [l1, m1, t1] = Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, path[i].first, path[i - 1].first, false, LevelMode::Level);
				if (l1.Succeeded()) result.cost += l1.GetCost();
				auto [l2, m2, t2] = Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, path[i + 1].first, path[i].first, false, LevelMode::Level);
				if (l2.Succeeded()) result.cost += l2.GetCost();
				c = Command<Commands::BuildBridge>::Do(do_flags, path[i + 1].first, path[i].first,
						TransportType::Rail, bt < MAX_BRIDGES ? bt : 0, RAILTYPE_RAIL, INVALID_ROADTYPE);
				Debug(misc, 0, "AC: bridge retry at 0x{:X} -> {}", path[i].first.base(), c.Succeeded() ? 1 : 0);
			}
			if (c.Failed()) {
				result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
				result.error_detail = c.GetErrorMessage().base();
				return false;
			}
			result.cost += c.GetCost();
			if (do_flags.Test(DoCommandFlag::Execute)) {
				AcLogTile(path[i].first);
				AcLogTile(path[i + 1].first);
			}
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
			CommandCost c2 = Command<Commands::BuildRail>::Do(do_flags, path[i].first, RAILTYPE_RAIL, track, true);
			Debug(misc, 0, "AC: rail retry at 0x{:X} err {} level {} retry {}",
					path[i].first.base(), c.GetErrorMessage().base(),
					lc.Succeeded() ? 1 : 0, c2.Succeeded() ? 1 : 0);
			c = c2;
		}
		if (c.Failed()) {
			result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
			result.error_detail = c.GetErrorMessage().base();
			return false;
		}
		result.cost += c.GetCost();
		if (do_flags.Test(DoCommandFlag::Execute)) AcLogTile(path[i].first);
	}

	return true;
}

/** Einbahn-Signale entlang eines gebauten Pfads setzen (alle 4 Kacheln). */
static void SignalisePath(const std::vector<std::pair<TileIndex, DiagDirection>> &path, AutoConnectResult &result)
{
	uint since = 3;
	for (size_t i = 1; i + 1 < path.size(); i++) {
		since++;
		if (since < 4) continue;
		if (DistanceManhattan(path[i].first, path[i + 1].first) > 1) continue; /* Brücke */
		if (i > 0 && DistanceManhattan(path[i - 1].first, path[i].first) > 1) continue; /* Brückenkopf */
		BuildLoopSignal(path[i].first, ReverseDiagDir(path[i].second), path[i + 1].second); /* Fehlschlag ist ok */
		since = 0;
	}
	(void)result;
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
static AutoConnectResult BuildRailConnection(TileIndex center_a, TileIndex center_b, uint train_count, uint8_t cargo_mode, CargoType freight, bool estimate)
{
	static const uint TRAIN_WAGONS = 3; ///< Waggons je Zug.
	AutoConnectResult result;
	DoCommandFlags do_flags = estimate ? DoCommandFlags{} : DoCommandFlags{DoCommandFlag::Execute};
	Backup<CompanyID> cur_company(_current_company, _local_company);
	bool loop = train_count > 1;
	uint8_t numtracks = loop ? 2 : 1;

	/* Geld-Vorabpruefung: gar nicht erst bauen, wenn Strecke UND Zuege
	 * zusammen absehbar nicht bezahlbar sind. Vorher stand sonst eine
	 * teure Strecke ohne einen einzigen Zug in der Landschaft. */
	if (!estimate) {
		EngineID pre_engine = FindBestTrainEngine();
		if (pre_engine == EngineID::Invalid()) {
			result.error = STR_AUTOCONNECT_ERR_NO_VEHICLE;
			cur_company.Restore();
			return result;
		}
		Money vehicles = Engine::Get(pre_engine)->GetCost() * 2 * train_count; /* Lok + grob Waggons */
		Money infra = (Money)DistanceManhattan(center_a, center_b) * 1000 * numtracks + 30000;
		if (Company::Get(_local_company)->money < vehicles + infra) {
			result.error = STR_AUTOCONNECT_ERR_NO_MONEY_TOTAL;
			cur_company.Restore();
			return result;
		}
	}

	/* Bestehende eigene Bahnhöfe in der Nähe wiederverwenden. */
	Station *re_a = FindNearbyOwnStation(center_a, StationFacility::Train, 25);
	Station *re_b = FindNearbyOwnStation(center_b, StationFacility::Train, 25);
	if (re_a != nullptr && re_a == re_b) re_b = nullptr;

	RailSite site_a, site_b;
	bool build_st_a = re_a == nullptr || !RailSiteFromStation(re_a, center_b, loop, site_a);
	bool build_st_b = re_b == nullptr || !RailSiteFromStation(re_b, center_a, loop, site_b);
	if (build_st_a) site_a = FindRailStationSite(center_a, center_b, loop, numtracks);
	if (build_st_b) site_b = FindRailStationSite(center_b, center_a, loop, numtracks);
	/* Kein ebener Platz? Mit Planierung erneut suchen (nur beim echten Bau). */
	if (!estimate && _ac_allow_terraform) {
		if (build_st_a && site_a.tile == INVALID_TILE) site_a = FindRailStationSite(center_a, center_b, loop, numtracks, true, &result);
		if (build_st_b && site_b.tile == INVALID_TILE) site_b = FindRailStationSite(center_b, center_a, loop, numtracks, true, &result);
	}
	if (site_a.tile == INVALID_TILE || site_b.tile == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_RAIL_SITE;
		cur_company.Restore();
		return result;
	}

	/* Ab hier entsteht Sichtbares: Bau-Log fuer sauberen Rueckbau fuehren. */
	if (!estimate) AcLogBegin();

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
			AcRollback();
			cur_company.Restore();
			return result;
		}
		result.cost += c.GetCost();
		/* Neue Bahnhofsflaeche ins Bau-Log (fuer den Rueckbau-Fall). */
		if (!estimate) {
			uint w = (site->axis == Axis::X) ? RAIL_PLATFORM_LEN : numtracks;
			uint h = (site->axis == Axis::X) ? numtracks : RAIL_PLATFORM_LEN;
			for (TileIndex t : TileArea(site->tile, static_cast<uint8_t>(w), static_cast<uint8_t>(h))) AcLogTile(t);
		}
	}

	/* Hinweg suchen und bauen. */
	auto path = FindRailPath(site_a, site_b);
	if (path.empty()) {
		result.error = STR_AUTOCONNECT_ERR_NO_RAIL_PATH;
		AcRollback();
		cur_company.Restore();
		return result;
	}
	/* Nur-Umweg-Routen (z. B. einmal um die Fabrik herum) nicht bauen -
	 * das Ergebnis aergert mehr, als es nutzt. */
	if (path.size() > 30 + 3 * DistanceManhattan(center_a, center_b)) {
		result.error = STR_AUTOCONNECT_ERR_DETOUR;
		AcRollback();
		cur_company.Restore();
		return result;
	}
	if (!BuildRailLine(path, ReverseDiagDir(site_b.exit_dir), loop && !estimate, do_flags, result)) {
		AcRollback();
		cur_company.Restore();
		return result;
	}
	std::vector<std::pair<TileIndex, DiagDirection>> path2;
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
		path2 = FindRailPath(from2, to2);
		if (path2.empty()) {
			result.error = STR_AUTOCONNECT_ERR_NO_RAIL_PATH;
			AcRollback();
			cur_company.Restore();
			return result;
		}
		if (!BuildRailLine(path2, ReverseDiagDir(site_a.exit2_dir), !estimate, do_flags, result)) {
			AcRollback();
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
		AcRollback();
		cur_company.Restore();
		return result;
	}
	/* Waggonwahl: Güterzug nimmt refit-fähige Waggons der Frachtart,
	 * sonst bei "beides" zwei Passagier- und ein Postwaggon. */
	EngineID w_pax = FindBestWagon({CargoClass::Passengers});
	EngineID w_mail = FindBestWagon({CargoClass::Mail});
	EngineID w_freight = EngineID::Invalid();
	if (freight != INVALID_CARGO) {
		uint best_cap = 0;
		for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
			if (!e->company_avail.Test(_local_company)) continue;
			const RailVehicleInfo &rvi = e->VehInfo<RailVehicleInfo>();
			if (rvi.railveh_type != RailVehicleType::Wagon) continue;
			if (!rvi.railtypes.Test(RAILTYPE_RAIL)) continue;
			if (e->GetDefaultCargoType() != freight && !e->info.refit_mask.Test(freight)) continue;
			uint cap = e->GetDisplayDefaultCapacity();
			if (cap >= best_cap) {
				best_cap = cap;
				w_freight = e->index;
			}
		}
	}
	auto pick_wagon = [&](uint j) {
		if (freight != INVALID_CARGO) return w_freight;
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

	/* Depot an einer Streckenkachel (gerade oder diagonal). */
	TileIndex depot = INVALID_TILE;
	for (size_t i = 1; i + 1 < path.size() && depot == INVALID_TILE; i++) {
		DiagDirection entry_edge = ReverseDiagDir(path[i].second);
		DiagDirection exit_edge = (i + 1 < path.size()) ? path[i + 1].second : ReverseDiagDir(site_b.exit_dir);
		if (entry_edge == exit_edge) continue; /* degeneriert */
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
			/* BEIDE Anschlusskurven muessen stehen, sonst haengt das Depot nur
			 * einseitig an der Strecke und der Zug findet kein Ziel ("lost").
			 * Dann Depot wieder abreissen und die naechste Stelle versuchen. */
			if (c1.Failed() || c2.Failed()) {
				Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute, cand);
				continue;
			}
			result.cost += c1.GetCost() + c2.GetCost();
			depot = cand;
			AcLogTile(cand);
			break;
		}
	}
	if (depot == INVALID_TILE) {
		result.error = STR_AUTOCONNECT_ERR_NO_DEPOT;
		AcRollback();
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
				AcRollback();
				cur_company.Restore();
				return result;
			}
			break;
		}
		result.cost += cost_v.GetCost();

		uint wagons_built = 0;
		for (uint i = 0; i < TRAIN_WAGONS; i++) {
			EngineID wagon = pick_wagon(i);
			if (wagon == EngineID::Invalid()) continue;
			auto [cost_w, wagon_id, rw1, rw2, rw3] =
					Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, wagon, true, freight, ClientID::Invalid);
			if (cost_w.Failed()) break;
			result.cost += cost_w.GetCost();
			Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, wagon_id, veh_id, false);
			wagons_built++;
		}
		if (wagons_built == 0) {
			/* Lok ohne Waggons transportiert nichts: verkaufen und sauber melden. */
			Command<Commands::SellVehicle>::Do(DoCommandFlag::Execute, veh_id, true, false, ClientID::Invalid);
			if (t == 0) {
				result.error = STR_AUTOCONNECT_ERR_NO_MONEY_VEHICLE;
				AcRollback();
				cur_company.Restore();
				return result;
			}
			break;
		}

		Order oa = MakeStationOrder(st_a->index);
		if (freight != INVALID_CARGO) oa.SetLoadType(OrderLoadType::FullLoadAny); /* an der Quelle volladen */
		CommandCost co_a = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, oa);
		if (co_a.Failed()) result.error_detail = co_a.GetErrorMessage().base();
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, MakeStationOrder(st_b->index));
		AcStartStaggered(veh_id, t);
	}

	/* Signale erst jetzt: eine abgebrochene Baustelle hinterlässt so
	 * nie sinnlos besignalte Gleise ohne Züge. */
	if (loop) {
		SignalisePath(path, result);
		if (!path2.empty()) SignalisePath(path2, result);
	}

	AcLogEnd();
	cur_company.Restore();
	result.ok = true;
	return result;
}

/**
 * Lohnendste unbediente Industrie-Verbindung suchen.
 * @return true bei Fund; Quelle, Ziel und Frachtart in den Out-Parametern.
 */
static bool FindFreightSuggestion(IndustryID &out_a, IndustryID &out_b, CargoType &out_cargo)
{
	uint best_score = 0;
	for (const Industry *src : Industry::Iterate()) {
		for (const auto &p : src->produced) {
			if (p.cargo == INVALID_CARGO || p.rate == 0) continue;
			/* Schon bedient? Eigene Station in Reichweite der Quelle. */
			if (FindNearbyOwnStation(src->location.tile, StationFacility::Train, 8) != nullptr) continue;
			for (const Industry *dst : Industry::Iterate()) {
				if (dst == src) continue;
				for (const auto &a : dst->accepted) {
					if (a.cargo != p.cargo) continue;
					uint dist = DistanceManhattan(src->location.tile, dst->location.tile);
					if (dist < 16 || dist > 140) continue;
					/* Hohe Produktion gut, mittlere Distanz ideal. */
					uint score = uint(p.rate) * 1000 / (30 + (dist > 60 ? dist - 60 : 60 - dist));
					if (score > best_score) {
						best_score = score;
						out_a = src->index;
						out_b = dst->index;
						out_cargo = p.cargo;
					}
				}
			}
		}
	}
	return best_score > 0;
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
		AcStartStaggered(veh_id, i);
	}

	cur_company.Restore();
	result.ok = true;
	return result;
}

/**
 * Bestehende Verbindung zwischen zwei Städten finden: ein eigenes
 * Fahrzeug des Typs, dessen Aufträge Stationen nahe beider Städte
 * anfahren.
 */
static const Vehicle *FindExistingLink(const Town *a, const Town *b, VehicleType type)
{
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->owner != _local_company || !v->IsPrimaryVehicle() || v->type != type) continue;
		bool near_a = false, near_b = false;
		for (const Order &o : v->Orders()) {
			if (!o.IsType(OT_GOTO_STATION)) continue;
			const Station *st = Station::GetIfValid(o.GetDestination().ToStationID());
			if (st == nullptr) continue;
			if (DistanceManhattan(st->xy, a->xy) < 25) near_a = true;
			if (DistanceManhattan(st->xy, b->xy) < 25) near_b = true;
		}
		if (near_a && near_b) return v;
	}
	return nullptr;
}

/** Passendes Depot (bzw. Hangar) zum Klonen eines Fahrzeugs finden. */
static TileIndex FindCloneDepot(const Vehicle *v)
{
	if (v->type == VehicleType::Aircraft) {
		for (const Order &o : v->Orders()) {
			if (!o.IsType(OT_GOTO_STATION)) continue;
			const Station *st = Station::GetIfValid(o.GetDestination().ToStationID());
			if (st != nullptr && st->facilities.Test(StationFacility::Airport) && st->airport.HasHangar()) {
				return st->airport.GetHangarTile(0);
			}
		}
		return INVALID_TILE;
	}
	TileIndex best = INVALID_TILE;
	uint best_dist = UINT32_MAX;
	for (const Depot *d : Depot::Iterate()) {
		if (d->xy == INVALID_TILE || GetTileOwner(d->xy) != _local_company) continue;
		bool fits = (v->type == VehicleType::Train && IsRailDepotTile(d->xy)) ||
				(v->type == VehicleType::Road && IsRoadDepotTile(d->xy)) ||
				(v->type == VehicleType::Ship && IsShipDepotTile(d->xy));
		if (!fits) continue;
		uint dist = DistanceManhattan(d->xy, v->tile);
		if (dist < best_dist) {
			best_dist = dist;
			best = d->xy;
		}
	}
	return best;
}

/**
 * Netz-Diagnose: eigene Fahrzeuge und Stationen auf typische Probleme
 * prüfen. Liefert eine kompakte Statuszeile; das erste Problemfahrzeug
 * wird ins Blickfeld gescrollt.
 */
static std::string RunNetworkCheck()
{
	uint total = 0, no_orders = 0, stopped = 0, lost = 0, unused_st = 0;
	TileIndex focus = INVALID_TILE;
	std::set<StationID> served;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->owner != _local_company || !v->IsPrimaryVehicle()) continue;
		total++;
		bool bad = false;
		if (v->GetNumOrders() < 2) { no_orders++; bad = true; }
		if (v->vehstatus.Test(VehState::Stopped)) { stopped++; bad = true; }
		if (v->vehicle_flags.Test(VehicleFlag::PathfinderLost)) { lost++; bad = true; }
		if (bad && focus == INVALID_TILE) focus = v->tile;
		for (const Order &o : v->Orders()) {
			if (o.IsType(OT_GOTO_STATION)) served.insert(o.GetDestination().ToStationID());
		}
	}
	for (const Station *st : Station::Iterate()) {
		if (st->owner != _local_company) continue;
		if (served.count(st->index) == 0) {
			unused_st++;
			if (focus == INVALID_TILE) focus = st->xy;
		}
	}
	if (no_orders + stopped + lost + unused_st == 0) return GetString(STR_AUTOCONNECT_CHECK_OK, total);
	if (focus != INVALID_TILE) ScrollMainWindowToTile(focus);
	return GetString(STR_AUTOCONNECT_CHECK_RESULT, no_orders, stopped, lost, unused_st);
}

/** Fenster der Auto-Verbindung. */
struct AutoConnectWindow : Window {
	TownID town_a = TownID::Invalid();
	TownID town_b = TownID::Invalid();
	uint count = 2;
	uint8_t mode = 0; ///< 0 = Flugzeuge, 1 = Busse, 2 = Zug, 3 = Schiffe.
	uint8_t cargo = 0; ///< 0 = Passagiere, 1 = Post, 2 = beides.
	uint8_t stops = 0; ///< Bus-Haltestellen je Stadt; 0 = automatisch.
	IndustryID sug_a = IndustryID::Invalid(); ///< Güter-Vorschlag: Quelle.
	IndustryID sug_b = IndustryID::Invalid(); ///< Güter-Vorschlag: Ziel.
	CargoType sug_cargo = INVALID_CARGO; ///< Güter-Vorschlag: Frachtart.
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
			case WID_AC_STOPS:
				return this->stops == 0 ? GetString(STR_AUTOCONNECT_STOPS_AUTO) : GetString(STR_AUTOCONNECT_STOPS_N, this->stops);
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

			case WID_AC_STOPS:
				this->stops = (this->stops + 1) % 5; /* 0 = automatisch, dann 1-4. */
				this->SetDirty();
				break;

			case WID_AC_CHECK:
				this->status = RunNetworkCheck();
				this->SetDirty();
				break;

			case WID_AC_SUGGEST: {
				if (_networking) break;
				if (this->sug_cargo != INVALID_CARGO) {
					/* Zweiter Klick: Vorschlag als Güterzug bauen. */
					Industry *ia = Industry::GetIfValid(this->sug_a);
					Industry *ib = Industry::GetIfValid(this->sug_b);
					if (ia != nullptr && ib != nullptr) {
						AutoConnectResult res = BuildRailConnection(ia->location.tile, ib->location.tile, 1, 0, this->sug_cargo, false);
						if (res.ok) {
							this->status = GetString(STR_AUTOCONNECT_STATUS_DONE, res.cost);
						} else {
							this->status = GetString(res.error);
							if (res.error_detail != 0) this->status += ": " + GetString(StringID(res.error_detail));
						}
					}
					this->sug_cargo = INVALID_CARGO;
					this->sug_a = IndustryID::Invalid();
					this->sug_b = IndustryID::Invalid();
				} else if (FindFreightSuggestion(this->sug_a, this->sug_b, this->sug_cargo)) {
					ScrollMainWindowToTile(Industry::Get(this->sug_a)->location.tile);
					this->status = GetString(STR_AUTOCONNECT_SUGGEST_RESULT, this->sug_a, this->sug_b);
				} else {
					this->status = GetString(STR_AUTOCONNECT_SUGGEST_NONE);
				}
				this->SetDirty();
				break;
			}

			case WID_AC_TERRAFORM:
				_ac_allow_terraform = !_ac_allow_terraform;
				this->SetDirty();
				break;

			case WID_AC_BIGAIR:
				_ac_big_airports = !_ac_big_airports;
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
						case 1: return BuildRoadConnection(Town::Get(this->town_a), Town::Get(this->town_b), this->count, this->cargo, est, this->stops);
						case 2: return BuildRailConnection(Town::Get(this->town_a)->xy, Town::Get(this->town_b)->xy, this->count, this->cargo, INVALID_CARGO, est);
						case 3: return BuildShipConnection(Town::Get(this->town_a), Town::Get(this->town_b), this->count, this->cargo, est);
						default: return BuildAirConnection(Town::Get(this->town_a), Town::Get(this->town_b), this->count, est);
					}
				};

				if (!estimate) {
					/* Gibt es die Verbindung schon? Dann nur Fahrzeuge ergaenzen. */
					static const VehicleType kinds[] = {VehicleType::Aircraft, VehicleType::Road, VehicleType::Train, VehicleType::Ship};
					const Vehicle *link = FindExistingLink(Town::Get(this->town_a), Town::Get(this->town_b), kinds[this->mode]);
					if (link != nullptr) {
						TileIndex depot = FindCloneDepot(link);
						if (depot != INVALID_TILE) {
							Backup<CompanyID> cur_company(_current_company, _local_company);
							Money spent = 0;
							uint added = 0;
							for (uint i = 0; i < this->count; i++) {
								auto [cc, new_id] = Command<Commands::CloneVehicle>::Do(DoCommandFlag::Execute, depot, link->index, true);
								if (cc.Failed()) break;
								spent += cc.GetCost();
								added++;
								Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, new_id, false);
							}
							cur_company.Restore();
							this->status = GetString(STR_AUTOCONNECT_STATUS_EXTENDED, added, spent);
							this->SetDirty();
							break;
						}
					}

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
					if (res.error_detail != 0) this->status += ": " + GetString(StringID(res.error_detail));
				} else {
					this->status = GetString(res.error);
					if (res.error_detail != 0) this->status += ": " + GetString(StringID(res.error_detail));
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

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_AC_TERRAFORM, _ac_allow_terraform);
		this->SetWidgetLoweredState(WID_AC_BIGAIR, _ac_big_airports);
		this->DrawWidgets();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		/* Statuszeile mehrzeilig, damit auch lange Fehlermeldungen lesbar sind. */
		if (widget != WID_AC_STATUS) return;
		DrawStringMultiLine(r, this->status, TextColour::Black);
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
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_STOPS), SetFill(1, 0), SetMinimalSize(220, 14), SetToolTip(STR_AUTOCONNECT_STOPS_TOOLTIP),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_COUNT_DOWN), SetMinimalSize(20, 14), SetStringTip(STR_AUTOCONNECT_MINUS),
				NWidget(WWT_TEXT, Colours::Invalid, WID_AC_COUNT), SetFill(1, 0), SetAlignment({AlignmentH::Centre, AlignmentV::Middle}),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_COUNT_UP), SetMinimalSize(20, 14), SetStringTip(STR_AUTOCONNECT_PLUS),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_AC_TERRAFORM), SetFill(1, 0), SetMinimalSize(106, 14), SetStringTip(STR_AUTOCONNECT_TERRAFORM, STR_AUTOCONNECT_TERRAFORM_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_AC_BIGAIR), SetFill(1, 0), SetMinimalSize(106, 14), SetStringTip(STR_AUTOCONNECT_BIGAIR, STR_AUTOCONNECT_BIGAIR_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_ESTIMATE), SetFill(1, 0), SetMinimalSize(106, 16), SetStringTip(STR_AUTOCONNECT_ESTIMATE, STR_AUTOCONNECT_ESTIMATE_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Green, WID_AC_BUILD), SetFill(1, 0), SetMinimalSize(106, 16), SetStringTip(STR_AUTOCONNECT_BUILD, STR_AUTOCONNECT_BUILD_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_CHECK), SetFill(1, 0), SetMinimalSize(106, 14), SetStringTip(STR_AUTOCONNECT_CHECK, STR_AUTOCONNECT_CHECK_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AC_SUGGEST), SetFill(1, 0), SetMinimalSize(106, 14), SetStringTip(STR_AUTOCONNECT_SUGGEST, STR_AUTOCONNECT_SUGGEST_TOOLTIP),
			EndContainer(),
			NWidget(WWT_EMPTY, Colours::Invalid, WID_AC_STATUS), SetFill(1, 0), SetMinimalSize(220, 44),
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


/**
 * Fork: Diagnose-Einstieg fuer den Konsolenbefehl "autoconnect".
 * Fuehrt den Auto-Bau direkt aus und liefert eine lesbare Zusammenfassung.
 * @param mode "air", "bus", "rail" oder "ship".
 * @param a_idx/b_idx Stadt-Indizes in Iterationsreihenfolge; bei auto_pick
 *                    wird das naechstgelegene Stadtpaar (15-45 Kacheln) gewaehlt.
 */
std::string AutoConnectDebugBuild(std::string_view mode, uint a_idx, uint b_idx, uint count, bool auto_pick)
{
	std::vector<Town *> towns;
	for (Town *t : Town::Iterate()) towns.push_back(t);
	if (towns.size() < 2) return "Zu wenige Staedte auf der Karte.";

	Town *ta = nullptr, *tb = nullptr;
	if (auto_pick) {
		uint best = UINT_MAX;
		for (size_t i = 0; i < towns.size(); i++) {
			for (size_t j = i + 1; j < towns.size(); j++) {
				uint d = DistanceManhattan(towns[i]->xy, towns[j]->xy);
				if (d >= 15 && d <= 45 && d < best) { best = d; ta = towns[i]; tb = towns[j]; }
			}
		}
		if (ta == nullptr) { ta = towns[0]; tb = towns[1]; }
	} else {
		if (a_idx >= towns.size() || b_idx >= towns.size() || a_idx == b_idx) return "Ungueltige Stadt-Indizes.";
		ta = towns[a_idx];
		tb = towns[b_idx];
	}

	Backup<CompanyID> cur_company(_current_company, _local_company);
	AutoConnectResult res;
	if (mode == "air") res = BuildAirConnection(ta, tb, count, false);
	else if (mode == "bus") res = BuildRoadConnection(ta, tb, count, 0, false);
	else if (mode == "rail") res = BuildRailConnection(ta->xy, tb->xy, count, 0, INVALID_CARGO, false);
	else if (mode == "ship") res = BuildShipConnection(ta, tb, count, 0, false);
	else { cur_company.Restore(); return "Unbekannter Modus (air/bus/rail/ship)."; }
	cur_company.Restore();

	std::string out = fmt::format("AC-Diagnose {}: {} -> {} (Distanz {}): ", mode,
			ta->GetCachedName(), tb->GetCachedName(), DistanceManhattan(ta->xy, tb->xy));
	if (res.ok) {
		out += fmt::format("OK, Kosten {}", (int64_t)res.cost);
		if (res.error_detail != 0) out += fmt::format(" (Teilproblem: {})", GetString(StringID(res.error_detail)));
	} else {
		out += fmt::format("FEHLGESCHLAGEN: {}", GetString(res.error));
		if (res.error_detail != 0) out += fmt::format(" - Grund: {}", GetString(StringID(res.error_detail)));
	}
	Debug(misc, 0, "{}", out);
	return out;
}


/* =================== Industrie-Abnehmer-Dialog (Fork) =================== */

/** Bester LKW für eine Frachtart (Standardfracht oder umruestbar). */
static EngineID FindBestTruck(CargoType cargo)
{
	EngineID best = EngineID::Invalid();
	uint best_capacity = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (e->VehInfo<RoadVehicleInfo>().roadtype != ROADTYPE_ROAD) continue;
		if (e->GetDefaultCargoType() != cargo && !e->info.refit_mask.Test(cargo)) continue;
		uint cap = e->GetDisplayDefaultCapacity();
		if (cap > best_capacity) {
			best_capacity = cap;
			best = e->index;
		}
	}
	return best;
}

/**
 * Laderampe (Durchfahrt-LKW-Stop) nahe einer Industrie: bestehende
 * gerade Strasse nutzen oder eine Strassenkachel neu anlegen.
 */
static TileIndex FindIndustryStopSite(const Industry *ind, Axis &axis_out, bool build_road)
{
	for (TileIndex tile : SpiralTileSequence(ind->location.tile, 16)) {
		if (IsNormalRoadTile(tile)) {
			RoadBits bits = GetRoadBits(tile, RoadTramType::Road);
			Axis axis;
			if (bits == ROAD_X) axis = Axis::X;
			else if (bits == ROAD_Y) axis = Axis::Y;
			else continue;
			if (Command<Commands::BuildRoadStop>::Do({}, tile, 1, 1, RoadStopType::Truck, true,
					AxisToDiagDir(axis), ROADTYPE_ROAD, ROADSTOP_CLASS_DFLT, 0, NEW_STATION, false).Succeeded()) {
				axis_out = axis;
				return tile;
			}
		}
	}
	if (!build_road) return INVALID_TILE;
	for (TileIndex tile : SpiralTileSequence(ind->location.tile, 12)) {
		if (IsNormalRoadTile(tile) || IsTileType(tile, TileType::Station)) continue;
		for (Axis axis : {Axis::X, Axis::Y}) {
			RoadBits want = axis == Axis::X ? ROAD_X : ROAD_Y;
			if (Command<Commands::BuildRoad>::Do({}, tile, want, ROADTYPE_ROAD, DisallowedRoadDirections{}, TownID::Invalid()).Failed()) continue;
			Command<Commands::BuildRoad>::Do(DoCommandFlag::Execute, tile, want, ROADTYPE_ROAD, DisallowedRoadDirections{}, TownID::Invalid());
			if (Command<Commands::BuildRoadStop>::Do({}, tile, 1, 1, RoadStopType::Truck, true,
					AxisToDiagDir(axis), ROADTYPE_ROAD, ROADSTOP_CLASS_DFLT, 0, NEW_STATION, false).Succeeded()) {
				axis_out = axis;
				return tile;
			}
		}
	}
	return INVALID_TILE;
}

/** Gueter-LKW-Verbindung zwischen zwei Industrien bauen. */
static AutoConnectResult BuildTruckConnection(const Industry *ind_a, const Industry *ind_b, CargoType cargo, uint count)
{
	AutoConnectResult result;
	Backup<CompanyID> cur_company(_current_company, _local_company);

	EngineID engine = FindBestTruck(cargo);
	if (engine == EngineID::Invalid()) {
		result.error = STR_AUTOCONNECT_ERR_NO_VEHICLE;
		cur_company.Restore();
		return result;
	}

	Axis axis_a{}, axis_b{};
	TileIndex stop_a = FindIndustryStopSite(ind_a, axis_a, true);
	TileIndex stop_b = FindIndustryStopSite(ind_b, axis_b, true);
	if (stop_a == INVALID_TILE || stop_b == INVALID_TILE || stop_a == stop_b) {
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

	for (auto [stop, axis] : {std::pair{stop_a, axis_a}, std::pair{stop_b, axis_b}}) {
		CommandCost c = Command<Commands::BuildRoadStop>::Do(DoCommandFlags{DoCommandFlag::Execute, DoCommandFlag::NoTestTownRating}, stop, 1, 1,
				RoadStopType::Truck, true, AxisToDiagDir(axis), ROADTYPE_ROAD, ROADSTOP_CLASS_DFLT, 0, NEW_STATION, false);
		if (c.Failed()) {
			result.error = STR_AUTOCONNECT_ERR_BUILD_FAILED;
			result.error_detail = c.GetErrorMessage().base();
			cur_company.Restore();
			return result;
		}
		result.cost += c.GetCost();
	}

	/* Depot neben dem Weg nahe Rampe A (Anschluss vor Bau getestet). */
	TileIndex depot = INVALID_TILE;
	for (size_t i = 0; i < std::min<size_t>(path.size(), 20) && depot == INVALID_TILE; i++) {
		if (!IsNormalRoadTile(path[i])) continue;
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
				if (c2.Failed()) continue;
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
				Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, engine, true, cargo, ClientID::Invalid);
		if (cost_v.Failed()) {
			if (i == 0) {
				result.error = STR_AUTOCONNECT_ERR_NO_MONEY_VEHICLE;
				result.error_detail = cost_v.GetErrorMessage().base();
				cur_company.Restore();
				return result;
			}
			break;
		}
		result.cost += cost_v.GetCost();
		Order oa = MakeStationOrder(st_a->index);
		oa.SetLoadType(OrderLoadType::FullLoadAny); /* an der Quelle volladen */
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, oa);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, MakeStationOrder(st_b->index));
		AcStartStaggered(veh_id, i);
	}

	cur_company.Restore();
	result.ok = true;
	return result;
}

/** Widgets des Abnehmer-Dialogs. */
enum IndustryConnectWidgets : WidgetID {
	WID_IC_CAPTION,
	WID_IC_MODE,
	WID_IC_LIST,
	WID_IC_STATUS,
};

/** Abnehmer-Dialog: nahe Industrien, die die Fracht annehmen. */
struct IndustryConnectWindow : Window {
	static const uint MAX_ROWS = 6;
	uint8_t mode = 0; ///< 0 = Zug, 1 = LKW.
	CargoType cargo = INVALID_CARGO;
	std::vector<std::pair<IndustryID, uint>> acceptors; ///< Ziel + Distanz.
	std::string status;

	IndustryConnectWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->InitNested(number);
		this->Refresh();
	}

	void Refresh()
	{
		this->acceptors.clear();
		const Industry *src = Industry::GetIfValid(static_cast<IndustryID>(this->window_number));
		if (src == nullptr) return;
		this->cargo = INVALID_CARGO;
		for (const auto &p : src->produced) {
			if (IsValidCargoType(p.cargo)) { this->cargo = p.cargo; break; }
		}
		if (this->cargo == INVALID_CARGO) {
			this->status = GetString(STR_INDCON_NO_PRODUCTION);
			return;
		}
		for (const Industry *i : Industry::Iterate()) {
			if (i == src || !i->IsCargoAccepted(this->cargo)) continue;
			this->acceptors.emplace_back(i->index, DistanceManhattan(src->location.tile, i->location.tile));
		}
		std::sort(this->acceptors.begin(), this->acceptors.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
		if (this->acceptors.size() > MAX_ROWS) this->acceptors.resize(MAX_ROWS);
		this->status = GetString(this->acceptors.empty() ? STR_INDCON_NONE : STR_INDCON_PICK);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_IC_CAPTION:
				return GetString(STR_INDCON_CAPTION, static_cast<IndustryID>(this->window_number));
			case WID_IC_MODE:
				return GetString(this->mode == 0 ? STR_INDCON_MODE_RAIL : STR_INDCON_MODE_TRUCK);
			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_IC_STATUS) {
			DrawStringMultiLine(r, this->status, TextColour::Black);
			return;
		}
		if (widget != WID_IC_LIST) return;
		Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);
		int line = GetCharacterHeight(FontSize::Normal) + 2;
		int y = tr.top;
		for (const auto &[id, dist] : this->acceptors) {
			DrawString(tr.left, tr.right, y, GetString(STR_INDCON_ROW, id, dist));
			y += line;
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		int line = GetCharacterHeight(FontSize::Normal) + 2;
		if (widget == WID_IC_LIST) {
			size.width = std::max<uint>(size.width, ScaleGUITrad(240));
			size.height = std::max<uint>(size.height, MAX_ROWS * line + ScaleGUITrad(4));
		}
		if (widget == WID_IC_STATUS) {
			size.height = std::max<uint>(size.height, 3 * line);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_IC_MODE:
				this->mode = 1 - this->mode;
				this->SetDirty();
				break;

			case WID_IC_LIST: {
				if (_networking) break;
				const Industry *src = Industry::GetIfValid(static_cast<IndustryID>(this->window_number));
				if (src == nullptr || this->cargo == INVALID_CARGO) break;
				Rect r = this->GetWidget<NWidgetBase>(WID_IC_LIST)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
				int line = GetCharacterHeight(FontSize::Normal) + 2;
				uint row = (pt.y - r.top) / line;
				if (row >= this->acceptors.size()) break;
				const Industry *dst = Industry::GetIfValid(this->acceptors[row].first);
				if (dst == nullptr) break;
				AutoConnectResult res = this->mode == 0
						? BuildRailConnection(src->location.tile, dst->location.tile, 1, 0, this->cargo, false)
						: BuildTruckConnection(src, dst, this->cargo, 2);
				if (res.ok) {
					this->status = GetString(STR_AUTOCONNECT_STATUS_DONE, res.cost);
					if (res.error_detail != 0) this->status += ": " + GetString(StringID(res.error_detail));
				} else {
					this->status = GetString(res.error);
					if (res.error_detail != 0) this->status += ": " + GetString(StringID(res.error_detail));
				}
				this->SetDirty();
				break;
			}
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_industry_connect_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Brown),
		NWidget(WWT_CAPTION, Colours::Brown, WID_IC_CAPTION), SetStringTip(STR_INDCON_CAPTION),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Brown),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_IC_MODE), SetFill(1, 0), SetMinimalSize(240, 14), SetToolTip(STR_INDCON_MODE_TOOLTIP),
			NWidget(WWT_EMPTY, Colours::Invalid, WID_IC_LIST), SetFill(1, 0), SetMinimalSize(240, 90), SetToolTip(STR_INDCON_LIST_TOOLTIP),
			NWidget(WWT_EMPTY, Colours::Invalid, WID_IC_STATUS), SetFill(1, 0), SetMinimalSize(240, 42),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _industry_connect_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::IndustryConnect, WindowClass::None,
	{},
	_nested_industry_connect_widgets
);

/** Abnehmer-Dialog fuer eine Industrie oeffnen (Fork-Feature). */
void ShowIndustryConnectWindow(IndustryID ind)
{
	AllocateWindowDescFront<IndustryConnectWindow>(_industry_connect_desc, ind.base());
}


/* =================== Auto-Modernisierung (Fork) ===================
 * Fuer jedes genutzte Fahrzeugmodell mit klar besserem, verfuegbarem
 * Nachfolger (gleiche Fracht, Kapazitaet und Tempo mindestens gleich,
 * eines echt besser) wird automatisch eine Ersetzen-Regel angelegt.
 * Der Tausch passiert wie gewohnt beim Depotbesuch - die Fahrplaene
 * der Fahrzeuge bleiben dabei vollstaendig erhalten. */
static void AutoModernizeMonthly()
{
	extern bool AssistantModernizeEnabled();
	if (!_settings_client.gui.fork_autoconnect) return;
	if (!AssistantModernizeEnabled()) return;
	if (_game_mode != GameMode::Normal) return;
	if (!Company::IsValidID(_local_company)) return;
	Company *c = Company::Get(_local_company);

	std::set<EngineID> used;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->owner != _local_company || !v->IsPrimaryVehicle()) continue;
		if (v->type == VehicleType::Train) continue; /* Waggonketten: spaeter */
		used.insert(v->engine_type);
	}

	for (EngineID from : used) {
		const Engine *ef = Engine::Get(from);
		uint cap_f = ef->GetDisplayDefaultCapacity();
		uint spd_f = ef->GetDisplayMaxSpeed();
		uint best_score = cap_f * 3 + spd_f;
		EngineID best = EngineID::Invalid();
		for (const Engine *e : Engine::IterateType(ef->type)) {
			if (e->index == from) continue;
			if (!e->company_avail.Test(_local_company)) continue;
			if (e->GetDefaultCargoType() != ef->GetDefaultCargoType()) continue;
			if (ef->type == VehicleType::Road && e->VehInfo<RoadVehicleInfo>().roadtype != ef->VehInfo<RoadVehicleInfo>().roadtype) continue;
			uint cap = e->GetDisplayDefaultCapacity();
			uint spd = e->GetDisplayMaxSpeed();
			if (cap < cap_f || spd < spd_f) continue;
			uint score = cap * 3 + spd;
			if (score > best_score) {
				best_score = score;
				best = e->index;
			}
		}
		if (best == EngineID::Invalid()) continue;
		if (EngineReplacementForCompany(c, from, ALL_GROUP) == best) continue;
		Backup<CompanyID> cur_company(_current_company, _local_company);
		Command<Commands::SetAutoreplace>::Do(DoCommandFlag::Execute, ALL_GROUP, from, best, false);
		cur_company.Restore();
	}
}

static const IntervalTimer<TimerGameCalendar> _auto_modernize_timer = {{TimerGameCalendar::Trigger::Month, TimerGameCalendar::Priority::None}, [](auto) {
	AutoModernizeMonthly();
}};
