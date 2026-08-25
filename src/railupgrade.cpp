/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file railupgrade.cpp Bahnhof im Betrieb ausbauen (Fork-Feature).
 *
 * Dasselbe Anliegen wie beim Flughafen: ein Bahnhof, der zu klein wird,
 * laesst sich in Vanilla nur abreissen und neu bauen - und dabei gehen
 * alle Auftraege verloren, die auf ihn zeigen.
 *
 * Ein Bahnhof waechst in zwei Richtungen:
 *   laenger  - die Bahnsteige werden verlaengert, dann passen laengere
 *              Zuege hinein (mehr Fracht je Fahrt).
 *   breiter  - ein zusaetzliches Gleis, dann muss der naechste Zug nicht
 *              vor dem Bahnhof warten.
 *
 * Beides wird ueber BuildRailStation mit station_to_join gebaut, damit
 * die Station dieselbe bleibt. Ein neues Gleis allein bringt allerdings
 * nichts: es braucht auch eine Weiche zum Nachbargleis, sonst kommt kein
 * Zug hin. Die legt AddSwitches vor dem Bahnhofskopf an.
 */

#include "stdafx.h"
#include "station_base.h"
#include "station_cmd.h"
#include "station_map.h"
#include "rail_cmd.h"
#include "rail_map.h"
#include "landscape_cmd.h"
#include "terraform_cmd.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "misc_cmd.h"
#include "core/backup_type.hpp"
#include "openttd.h"
#include "network/network.h"
#include "strings_func.h"
#include "debug.h"
#include "table/strings.h"

#include "safeguards.h"

/** Wie ein Bahnhof gerade dasteht. */
struct RailStationShape {
	TileIndex origin = INVALID_TILE; ///< Nordwestliche Ecke.
	Axis axis = Axis::X;             ///< Richtung der Bahnsteige.
	uint length = 0;                 ///< Felder je Bahnsteig.
	uint tracks = 0;                 ///< Anzahl Gleise.
	RailType rt = INVALID_RAILTYPE;
};

/**
 * Die Form eines Bahnhofs auslesen.
 * @param st Die Station.
 * @return Form; length == 0, wenn es kein Bahnhof ist.
 */
static RailStationShape ReadShape(const BaseStation *st)
{
	RailStationShape s;
	if (st == nullptr || st->train_station.tile == INVALID_TILE) return s;
	TileIndex t = st->train_station.tile;
	if (!IsTileType(t, TileType::Station) || !IsRailStationTile(t)) return s;

	s.origin = t;
	s.axis = GetRailStationAxis(t);
	s.rt = GetRailType(t);
	if (s.axis == Axis::X) {
		s.length = st->train_station.w;
		s.tracks = st->train_station.h;
	} else {
		s.length = st->train_station.h;
		s.tracks = st->train_station.w;
	}
	return s;
}

/**
 * Fork: Was der naechste Ausbauschritt bringt.
 *
 * Erst wird verlaengert, dann verbreitert - eine laengere Bahnsteigkante
 * hilft sofort jedem Zug, ein zweites Gleis erst, wenn wirklich zwei
 * Zuege gleichzeitig da sind.
 *
 * @param st Die Station.
 * @param[out] add_len Zusaetzliche Bahnsteigfelder.
 * @param[out] add_tracks Zusaetzliche Gleise.
 * @return true, wenn ueberhaupt etwas geht.
 */
bool RailUpgradeNextStep(const BaseStation *st, uint &add_len, uint &add_tracks)
{
	add_len = 0;
	add_tracks = 0;
	RailStationShape s = ReadShape(st);
	if (s.length == 0) return false;

	uint max_spread = _settings_game.station.station_spread;
	/* Abwechselnd wachsen statt erst endlos in die Laenge: ein Bahnsteig
	 * von sieben Feldern nimmt jeden ueblichen Zug auf, danach bringt ein
	 * zweites Gleis mehr als das achte Feld. Erst wenn beides steht, geht
	 * es weiter bis zur erlaubten Ausdehnung. */
	const uint comfortable_len = 7;
	const uint comfortable_tracks = 4;
	if (s.length < std::min(comfortable_len, max_spread)) {
		add_len = std::min<uint>(2, max_spread - s.length);
	} else if (s.tracks < std::min(comfortable_tracks, max_spread)) {
		add_tracks = 1;
	} else if (s.length < max_spread) {
		add_len = std::min<uint>(2, max_spread - s.length);
	} else if (s.tracks < max_spread) {
		add_tracks = 1;
	}
	return add_len > 0 || add_tracks > 0;
}

/** Kosten des naechsten Schritts schaetzen. */
Money RailUpgradeCost(const BaseStation *st, uint &add_len, uint &add_tracks)
{
	if (!RailUpgradeNextStep(st, add_len, add_tracks)) return 0;
	RailStationShape s = ReadShape(st);
	uint tiles = add_len > 0 ? add_len * s.tracks : add_tracks * s.length;
	/* Listenpreis plus Zuschlag fuer Raeumen und Weichen. */
	return (Money)_price[Price::BuildStationRail] * tiles * 3;
}

/**
 * Platz schaffen: alles raeumen, was auf den neuen Feldern steht.
 * @param area Der neue Bereich.
 * @param[out] blocked Wie viele Felder sich nicht raeumen liessen.
 * @return Kosten des Raeumens.
 */
static Money ClearForStation(TileArea area, uint &blocked)
{
	Money cost = 0;
	blocked = 0;
	for (TileIndex t : area) {
		if (IsTileType(t, TileType::Clear)) continue;
		CommandCost cc = Command<Commands::LandscapeClear>::Do(
				DoCommandFlags{DoCommandFlag::Execute, DoCommandFlag::NoTestTownRating}, t);
		if (cc.Succeeded()) {
			cost += cc.GetCost();
		} else {
			blocked++;
		}
	}
	return cost;
}

/**
 * Weichen vor den Bahnhofskopf legen.
 *
 * Ein frisch angebautes Gleis endet sonst im Nichts. Vor dem Kopf
 * bekommt jede Spur ein gerades Stueck, dazu die Diagonalen zu beiden
 * Nachbarn - damit erreicht ein Zug von der Zufahrt aus jedes Gleis.
 * Was nicht gebaut werden kann (Wasser, fremdes Grundstueck), wird
 * uebergangen; der Bahnhof bleibt trotzdem nutzbar.
 *
 * @param s Form des Bahnhofs nach dem Ausbau.
 * @param front true = Kopf im Norden/Westen, false = im Sueden/Osten.
 * @return Anzahl gebauter Gleisstuecke.
 */
static uint AddSwitches(const RailStationShape &s, bool front)
{
	uint built = 0;
	int step = front ? -1 : (int)s.length;
	for (uint k = 0; k < s.tracks; k++) {
		int dx = (s.axis == Axis::X) ? step : (int)k;
		int dy = (s.axis == Axis::X) ? (int)k : step;
		int x = (int)TileX(s.origin) + dx;
		int y = (int)TileY(s.origin) + dy;
		if (x < 1 || y < 1 || x >= (int)Map::MaxX() || y >= (int)Map::MaxY()) continue;
		TileIndex t = TileXY(x, y);

		/* Gerades Stueck in Fahrtrichtung. */
		Track straight = (s.axis == Axis::X) ? Track::X : Track::Y;
		if (Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, t, s.rt, straight, false).Succeeded()) built++;

		/* Diagonalen zu den Nachbargleisen - erst dadurch wird aus den
		 * parallelen Gleisen ein Bahnhof, den jeder Zug anfahren kann. */
		for (Track diag : {Track::Upper, Track::Lower, Track::Left, Track::Right}) {
			if (Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, t, s.rt, diag, false).Succeeded()) built++;
		}
	}
	return built;
}

/**
 * Fork: Den Bahnhof eine Stufe groesser machen.
 * @param st Die Station.
 * @return Meldung fuer den Spieler.
 */
static StringID RailUpgradeStep(Station *st, uint add_len, uint add_tracks);

StringID RailUpgradeDo(Station *st)
{
	if (st == nullptr || !st->facilities.Test(StationFacility::Train)) return STR_RAILUPGRADE_ERR_NO_STATION;
	if (!Company::IsValidID(st->owner)) return STR_RAILUPGRADE_ERR_NO_STATION;
	if (_networking) return STR_RAILUPGRADE_ERR_SINGLEPLAYER;

	uint add_len = 0, add_tracks = 0;
	if (!RailUpgradeNextStep(st, add_len, add_tracks)) return STR_RAILUPGRADE_ERR_BIGGEST;

	StringID res = RailUpgradeStep(st, add_len, add_tracks);
	if (res == STR_RAILUPGRADE_DONE) return res;

	/* Klemmt die eine Richtung (Hang, Zufahrt, fremdes Gleis), dann die
	 * andere versuchen - ein Gleis mehr ist auch ein Ausbau. */
	RailStationShape sh = ReadShape(st);
	uint max_spread = _settings_game.station.station_spread;
	if (add_len > 0 && sh.tracks < max_spread) return RailUpgradeStep(st, 0, 1);
	if (add_tracks > 0 && sh.length < max_spread) return RailUpgradeStep(st, std::min<uint>(2, max_spread - sh.length), 0);
	return res;
}

/**
 * Ein einzelner Ausbauschritt in genau einer Richtung.
 * @param st Die Station.
 * @param add_len Zusaetzliche Bahnsteigfelder (0 = nicht verlaengern).
 * @param add_tracks Zusaetzliche Gleise (0 = nicht verbreitern).
 * @return Meldung fuer den Spieler.
 */
static StringID RailUpgradeStep(Station *st, uint add_len, uint add_tracks)
{
	if (add_len == 0 && add_tracks == 0) return STR_RAILUPGRADE_ERR_BIGGEST;

	RailStationShape s = ReadShape(st);
	Backup<CompanyID> cur_company(_current_company, st->owner);

	/* Geld sichern, sonst scheitert der Ausbau am Kontostand. */
	{
		uint dl = 0, dt = 0;
		Money need = RailUpgradeCost(st, dl, dt) * 3;
		const Company *c = Company::GetIfValid(st->owner);
		if (c != nullptr && c->money < need) {
			Command<Commands::IncreaseLoan>::Do(DoCommandFlag::Execute, LoanCommand::Interval, 0);
		}
	}

	/* Wo der neue Teil hinkommt. Erst nach Sueden beziehungsweise Osten,
	 * dann nach Norden/Westen - auf einer Seite liegt oft die Zufahrt
	 * oder ein Hang, auf der anderen ist Platz. */
	struct Spot { TileIndex tile; TileArea area; };
	std::vector<Spot> spots;
	uint numtracks = add_len > 0 ? s.tracks : add_tracks;
	uint plat_len = add_len > 0 ? add_len : s.length;
	uint aw = (s.axis == Axis::X) ? plat_len : numtracks;
	uint ah = (s.axis == Axis::X) ? numtracks : plat_len;
	for (int side = 0; side < 2; side++) {
		int dx, dy;
		if (add_len > 0) {
			int off = (side == 0) ? (int)s.length : -(int)add_len;
			dx = (s.axis == Axis::X) ? off : 0;
			dy = (s.axis == Axis::X) ? 0 : off;
		} else {
			int off = (side == 0) ? (int)s.tracks : -(int)add_tracks;
			dx = (s.axis == Axis::X) ? 0 : off;
			dy = (s.axis == Axis::X) ? off : 0;
		}
		int x = (int)TileX(s.origin) + dx, y = (int)TileY(s.origin) + dy;
		if (x < 1 || y < 1 || x + (int)aw > (int)Map::MaxX() || y + (int)ah > (int)Map::MaxY()) continue;
		TileIndex t = TileXY(x, y);
		spots.push_back({t, TileArea(t, static_cast<uint8_t>(aw), static_cast<uint8_t>(ah))});
	}
	if (spots.empty()) { cur_company.Restore(); return STR_RAILUPGRADE_ERR_BUILD; }

	uint blocked = 0;
	Money clear_cost = 0;
	CommandCost c = CommandCost(STR_RAILUPGRADE_ERR_BUILD);
	TileArea area = spots.front().area;
	for (const Spot &sp : spots) {
		uint b = 0;
		Money cc = ClearForStation(sp.area, b);
		/* Auf Bahnhofshoehe planieren, nicht auf die des Hangs: die
		 * Referenz ist die vorhandene Bahnhofsecke. Sonst legt das Spiel
		 * den Anbau auf ein anderes Niveau und lehnt ihn ab. */
		TileIndex far = TileAddXY(sp.tile, sp.area.w - 1, sp.area.h - 1);
		TileIndex near_corner = (sp.tile == s.origin) ? far : sp.tile;
		Command<Commands::LevelLand>::Do(DoCommandFlag::Execute,
				(TileX(far) >= TileX(s.origin) && TileY(far) >= TileY(s.origin)) ? far : near_corner,
				s.origin, false, LevelMode::Level);
		CommandCost try_build = Command<Commands::BuildRailStation>::Do(
				DoCommandFlags{DoCommandFlag::Execute, DoCommandFlag::NoTestTownRating},
				sp.tile, s.rt, s.axis, static_cast<uint8_t>(numtracks), static_cast<uint8_t>(plat_len),
				STAT_CLASS_DFLT, 0, st->index, true);
		blocked = b;
		clear_cost = cc;
		area = sp.area;
		c = try_build;
		if (try_build.Succeeded()) break;
		Debug(misc, 1, "Bahnhof-Ausbau: Seite abgelehnt ({}), naechste probieren", try_build.GetErrorMessage().base());
	}
	if (c.Failed()) {
		cur_company.Restore();
		Debug(misc, 0, "Bahnhof-Ausbau: abgelehnt ({}), {} Felder blockiert", c.GetErrorMessage().base(), blocked);
		return c.GetErrorMessage() != INVALID_STRING_ID ? c.GetErrorMessage() : STR_RAILUPGRADE_ERR_BUILD;
	}

	/* Weichen an beiden Enden, damit die neuen Gleise erreichbar sind. */
	uint switches = 0;
	if (add_tracks > 0) {
		RailStationShape now = ReadShape(st);
		switches += AddSwitches(now, true);
		switches += AddSwitches(now, false);
	}

	cur_company.Restore();
	Debug(misc, 0, "Bahnhof-Ausbau: Station {} jetzt {} Gleise x {} Felder ({} geraeumt fuer {}, {} blockiert, {} Weichenstuecke)",
			st->index, add_tracks > 0 ? s.tracks + add_tracks : s.tracks,
			add_len > 0 ? s.length + add_len : s.length,
			area.w * area.h, (int64_t)clear_cost, blocked, switches);
	return STR_RAILUPGRADE_DONE;
}

/** Fork: Diagnose - was koennte der erste eigene Bahnhof werden? */
std::string RailUpgradeDebug(bool apply)
{
	for (Station *st : Station::Iterate()) {
		if (!st->facilities.Test(StationFacility::Train)) continue;
		if (st->owner != _local_company) continue;
		RailStationShape s = ReadShape(st);
		uint dl = 0, dt = 0;
		Money cost = RailUpgradeCost(st, dl, dt);
		std::string out = fmt::format("Bahnhof {}: {} Gleise x {} Felder", st->index, s.tracks, s.length);
		if (dl == 0 && dt == 0) {
			out += " - schon am Anschlag";
		} else {
			out += fmt::format(" -> +{} Felder, +{} Gleise fuer {}", dl, dt, (int64_t)cost);
			if (apply) {
				StringID res = RailUpgradeDo(st);
				RailStationShape after = ReadShape(st);
				out += fmt::format(" | jetzt {} Gleise x {} Felder, Ergebnis: {}",
						after.tracks, after.length,
						res == STR_RAILUPGRADE_DONE ? "fertig" : GetString(res));
			}
		}
		Debug(misc, 0, "Bahnhof-Diagnose: {}", out);
		return out;
	}
	return "Kein eigener Bahnhof gefunden.";
}

/**
 * Fork: Die Ausbaustufen an einem echten Bahnhof durchspielen.
 *
 * Wie beim Flughafen baut der Auto-Modus die Ausgangslage - der findet
 * eine Stelle, an der auch ein Spieler bauen wuerde.
 */
std::string RailUpgradeMaxTest()
{
	if (_game_mode != GameMode::Normal || _settings_game.station.station_spread == 0) return "Spiel laeuft noch nicht.";

	const Company *co = nullptr;
	for (const Company *i : Company::Iterate()) { co = i; break; }
	if (co == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		co = DoStartupNewCompany(false, CompanyID::Invalid());
	}
	if (co == nullptr) return "Keine Firma vorhanden.";
	CompanyID cid = co->index;
	Backup<CompanyID> cur_company(_current_company, cid);
	Backup<CompanyID> local_company(_local_company, cid);
	SubtractMoneyFromCompany(cid, CommandCost(ExpensesType::Other, -Money(2000000000)));

	extern std::string AutoConnectDebugBuild(std::string_view mode, uint a_idx, uint b_idx, uint count, bool auto_pick);
	std::string built = AutoConnectDebugBuild("rail", 0, 1, 1, true);

	Station *st = nullptr;
	for (Station *i : Station::Iterate()) {
		if (i->owner == cid && i->facilities.Test(StationFacility::Train)) { st = i; break; }
	}
	if (st == nullptr) { local_company.Restore(); cur_company.Restore(); return fmt::format("Kein Bahnhof zum Ausbauen ({})", built); }

	RailStationShape s0 = ReadShape(st);
	std::string out = fmt::format("Bahnhof-Test: Start {} Gleise x {} Felder", s0.tracks, s0.length);

	uint steps = 0;
	StringID last = STR_RAILUPGRADE_DONE;
	for (uint i = 0; i < 20; i++) {
		uint dl = 0, dt = 0;
		if (!RailUpgradeNextStep(st, dl, dt)) break;
		RailStationShape before = ReadShape(st);
		last = RailUpgradeDo(st);
		RailStationShape after = ReadShape(st);
		if (after.length == before.length && after.tracks == before.tracks) break;
		steps++;
	}
	RailStationShape s1 = ReadShape(st);
	out += fmt::format(" -> nach {} Stufe(n) {} Gleise x {} Felder, zuletzt: {}",
			steps, s1.tracks, s1.length, last == STR_RAILUPGRADE_DONE ? "fertig" : GetString(last));

	local_company.Restore();
	cur_company.Restore();
	return out;
}
