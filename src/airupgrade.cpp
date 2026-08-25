/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file airupgrade.cpp Flughafen im Betrieb ausbauen (Fork-Feature).
 *
 * Vanilla laesst einen Flughafen nur abreissen und neu bauen - dabei
 * geht die Station verloren, und mit ihr alle Auftraege, die auf sie
 * zeigen. Wer zwanzig Flugzeuge auf einen Flughafen schickt, baut ihn
 * deshalb nie aus.
 *
 * Dieser Umbau haelt die Station am Leben: der neue Flughafen wird mit
 * station_to_join auf dieselbe StationID gesetzt. Auftraege, Fahrplaene
 * und der Name bleiben unveraendert - nur der Beton wird groesser.
 *
 * Der heikle Teil ist die Reihenfolge: erst muessen alle Flugzeuge weg
 * (OpenTTD verweigert sonst den Abriss), dann faellt der alte Flughafen,
 * dann steht der neue. Geht der Neubau schief, wird der alte wieder
 * aufgebaut - ein Flughafen darf nicht einfach verschwinden.
 */

#include "stdafx.h"
#include "aircraft.h"
#include "station_base.h"
#include "station_cmd.h"
#include "airport.h"
#include "newgrf_airport.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "landscape_cmd.h"
#include "misc_cmd.h"
#include "terraform_cmd.h"
#include "economy_func.h"
#include "core/backup_type.hpp"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "strings_func.h"
#include "debug.h"
#include "table/strings.h"

#include "safeguards.h"

/**
 * Ein Flughafen, der auf seinen Umbau wartet.
 *
 * Der Umbau braucht einen leeren Flughafen - bei einer grossen Flotte
 * steht aber immer eines am Terminal. Deshalb wird der Flughafen erst
 * geschlossen (dann starten die letzten und keine neuen landen), und
 * sobald der Beton frei ist, baut der Tages-Timer um.
 */
struct PendingUpgrade {
	StationID station;
	CompanyID owner;
	int days_left; ///< Frist, danach wird wieder geoeffnet.
};
static std::vector<PendingUpgrade> _pending_upgrades;

/** Wie viele Terminals hat dieser Flughafentyp? */
static uint SpecTerminals(const AirportSpec *as)
{
	if (as == nullptr || as->fsm == nullptr) return 0;
	uint num = 0;
	for (uint i = as->fsm->terminals[0]; i > 0; i--) num += as->fsm->terminals[i];
	return num;
}

/**
 * Den naechstgroesseren Flughafentyp finden.
 *
 * "Groesser" heisst: mehr Terminals. Bei Gleichstand entscheidet die
 * Flaeche - ein Umbau soll sich lohnen und nicht nur anders aussehen.
 * @param cur Aktueller Typ.
 * @return Naechster Typ oder AT_INVALID.
 */
uint8_t AirUpgradeNextType(uint8_t cur)
{
	const AirportSpec *cur_as = AirportSpec::Get(cur);
	uint cur_terms = SpecTerminals(cur_as);

	uint8_t best = AT_INVALID;
	uint best_terms = cur_terms;
	for (uint8_t type = 0; type < NUM_AIRPORTS; type++) {
		const AirportSpec *as = AirportSpec::Get(type);
		if (as == nullptr || !as->IsAvailable()) continue;
		/* Hubschrauber-Plattformen sind kein Ausbau eines Flughafens -
		 * ein Flugzeug koennte dort nicht landen. */
		if (as->fsm == nullptr || as->class_index == APC_HELIPORT) continue;
		uint terms = SpecTerminals(as);
		if (terms <= cur_terms) continue;
		if (best == AT_INVALID || terms < best_terms) {
			best = type;
			best_terms = terms;
		}
	}
	return best;
}

/** Kachel der Nordwest-Ecke, so dass der neue Flughafen den alten moeglichst deckt. */
static TileIndex UpgradeOrigin(const Station *st, const AirportSpec *as)
{
	TileIndex old_tile = st->airport.tile;
	const AirportSpec *old_as = AirportSpec::Get(st->airport.type);
	/* Mittig ueber dem alten Flughafen ansetzen, dann bleibt das
	 * Einzugsgebiet ungefaehr gleich. */
	int dx = ((int)old_as->size_x - (int)as->size_x) / 2;
	int dy = ((int)old_as->size_y - (int)as->size_y) / 2;
	int x = Clamp((int)TileX(old_tile) + dx, 1, (int)Map::SizeX() - as->size_x - 1);
	int y = Clamp((int)TileY(old_tile) + dy, 1, (int)Map::SizeY() - as->size_y - 1);
	return TileXY(x, y);
}

/** Genug Geld beschaffen - notfalls per Kredit. */
static bool UpgradeEnsureFunds(CompanyID owner, Money needed)
{
	const Company *c = Company::GetIfValid(owner);
	if (c == nullptr) return false;
	if (c->money >= needed) return true;
	Money avail = c->money + (c->GetMaxLoan() - c->current_loan);
	if (avail < needed) return false;
	Backup<CompanyID> cur_company(_current_company, owner);
	Command<Commands::IncreaseLoan>::Do(DoCommandFlag::Execute, LoanCommand::Amount, needed - c->money);
	cur_company.Restore();
	return Company::Get(owner)->money >= needed;
}

/**
 * Was wuerde der Ausbau kosten? Trockenlauf ohne Nebenwirkungen.
 * @param st Der Flughafen.
 * @param[out] next Der Zieltyp.
 * @return Geschaetzte Kosten, 0 wenn kein Ausbau moeglich ist.
 */
Money AirUpgradeCost(const Station *st, uint8_t &next)
{
	next = AT_INVALID;
	if (st == nullptr || !st->facilities.Test(StationFacility::Airport)) return 0;
	next = AirUpgradeNextType(st->airport.type);
	if (next == AT_INVALID) return 0;

	const AirportSpec *as = AirportSpec::Get(next);
	TileIndex origin = UpgradeOrigin(st, as);
	if (!as->IsWithinMapBounds(0, origin)) {
		next = AT_INVALID;
		return 0;
	}

	Backup<CompanyID> cur_company(_current_company, st->owner);
	CommandCost build = Command<Commands::BuildAirport>::Do(DoCommandFlags{}, origin, next, 0, st->index, true);
	cur_company.Restore();
	/* Der Trockenlauf scheitert, solange der alte Flughafen noch steht -
	 * das ist erwartbar. Dann schaetzen wir ueber den Listenpreis. */
	if (build.Succeeded()) return build.GetCost();
	return (Money)_price[Price::BuildStationAirport] * as->size_x * as->size_y * 3;
}

/** Steht auf dem Flughafen noch ein Flugzeug? */
static bool AirportOccupied(const Station *st)
{
	for (const Aircraft *a : Aircraft::Iterate()) {
		if (!a->IsNormalAircraft()) continue;
		if (a->targetairport != st->index) continue;
		if (a->state != FLYING) return true;
	}
	return false;
}

/**
 * Fork: Umbau anstossen. Ist der Flughafen frei, wird sofort umgebaut -
 * sonst wird er geschlossen und der Timer baut um, sobald der letzte
 * Flieger gestartet ist.
 * @return Meldung fuer den Spieler.
 */
StringID AirUpgradeStart(Station *st)
{
	if (st == nullptr || !st->facilities.Test(StationFacility::Airport)) return STR_AIRUPGRADE_ERR_NO_AIRPORT;
	if (st->owner != _local_company) return STR_AIRUPGRADE_ERR_NO_AIRPORT;
	if (_networking) return STR_AIRUPGRADE_ERR_SINGLEPLAYER;
	if (AirUpgradeNextType(st->airport.type) == AT_INVALID) return STR_AIRUPGRADE_ERR_BIGGEST;

	extern StringID AirUpgradeDo(Station *st);
	if (!AirportOccupied(st)) return AirUpgradeDo(st);

	/* Schon vorgemerkt? Dann nicht doppelt schliessen. */
	for (const PendingUpgrade &p : _pending_upgrades) {
		if (p.station == st->index) return STR_AIRUPGRADE_QUEUED;
	}

	Backup<CompanyID> cur_company(_current_company, st->owner);
	if (!st->airport.blocks.Test(AirportBlock::AirportClosed)) {
		Command<Commands::OpenCloseAirport>::Do(DoCommandFlag::Execute, st->index);
	}
	cur_company.Restore();
	_pending_upgrades.push_back({st->index, st->owner, 365});
	Debug(misc, 0, "Flughafen-Ausbau: Station {} vorgemerkt, Flughafen geschlossen", st->index);
	return STR_AIRUPGRADE_QUEUED;
}

/** Taeglich: vorgemerkte Flughaefen umbauen, sobald sie frei sind. */
static const IntervalTimer<TimerGameCalendar> _airupgrade_timer = {{TimerGameCalendar::Trigger::Day, TimerGameCalendar::Priority::None}, [](auto) {
	extern StringID AirUpgradeDo(Station *st);
	for (auto it = _pending_upgrades.begin(); it != _pending_upgrades.end();) {
		Station *st = Station::GetIfValid(it->station);
		if (st == nullptr || !st->facilities.Test(StationFacility::Airport)) {
			it = _pending_upgrades.erase(it);
			continue;
		}
		bool give_up = --it->days_left <= 0;
		if (!give_up && AirportOccupied(st)) {
			++it;
			continue;
		}

		Backup<CompanyID> cur_company(_current_company, it->owner);
		StringID res = give_up ? STR_AIRUPGRADE_ERR_BUSY : AirUpgradeDo(st);
		/* Flughafen in jedem Fall wieder oeffnen - ein geschlossener
		 * Flughafen waere schlimmer als ein misslungener Umbau. */
		Station *now = Station::GetIfValid(it->station);
		if (now != nullptr && now->facilities.Test(StationFacility::Airport) &&
				now->airport.blocks.Test(AirportBlock::AirportClosed)) {
			Command<Commands::OpenCloseAirport>::Do(DoCommandFlag::Execute, it->station);
		}
		cur_company.Restore();
		Debug(misc, 0, "Flughafen-Ausbau: Station {} {}", it->station,
				res == STR_AIRUPGRADE_DONE ? "umgebaut" : "nicht umgebaut");
		it = _pending_upgrades.erase(it);
	}
}};

/**
 * Fork: Flughafen auf den naechstgroesseren Typ umbauen.
 * @param st Der Flughafen.
 * @return Meldung fuer den Spieler.
 */
StringID AirUpgradeDo(Station *st)
{
	if (st == nullptr || !st->facilities.Test(StationFacility::Airport)) return STR_AIRUPGRADE_ERR_NO_AIRPORT;
	if (!Company::IsValidID(st->owner)) return STR_AIRUPGRADE_ERR_NO_AIRPORT;
	if (_networking) return STR_AIRUPGRADE_ERR_SINGLEPLAYER;

	uint8_t next = AirUpgradeNextType(st->airport.type);
	if (next == AT_INVALID) return STR_AIRUPGRADE_ERR_BIGGEST;


	const AirportSpec *as = AirportSpec::Get(next);
	TileIndex origin = UpgradeOrigin(st, as);
	if (!as->IsWithinMapBounds(0, origin)) return STR_AIRUPGRADE_ERR_NO_ROOM;

	uint8_t old_type = st->airport.type;
	TileIndex old_tile = st->airport.tile;
	StationID id = st->index;

	/* Grosszuegig Geld sichern: Neubau, Planieren und ein Puffer. */
	uint8_t dummy = AT_INVALID;
	UpgradeEnsureFunds(st->owner, AirUpgradeCost(st, dummy) * 4);

	Backup<CompanyID> cur_company(_current_company, st->owner);

	/* Alten Flughafen abreissen. */
	CommandCost clear = Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute, old_tile);
	if (clear.Failed()) {
		cur_company.Restore();
		Debug(misc, 0, "Flughafen-Ausbau: Abriss abgelehnt ({})", clear.GetErrorMessage().base());
		return STR_AIRUPGRADE_ERR_BUSY;
	}

	/* Platz schaffen: der groessere Flughafen braucht ebenes Gelaende.
	 * Erst planieren, dann bauen - und zwar auf der Hoehe, auf der der
	 * alte Flughafen stand, damit nichts absackt. */
	TileIndex far = TileAddXY(origin, as->size_x - 1, as->size_y - 1);
	auto [level, level_cost, level_tile] = Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, origin, far, false, LevelMode::Level);
	if (level.Failed()) {
		/* Auch der zweite Anlauf ohne Diagonal-Beschraenkung darf scheitern -
		 * vielleicht liegt der Flughafen ohnehin eben genug. */
		Debug(misc, 0, "Flughafen-Ausbau: Planieren abgelehnt ({})", level.GetErrorMessage().base());
	}

	/* Neuen Flughafen auf dieselbe Station setzen - so bleiben Auftraege
	 * und Fahrplaene unberuehrt. */
	CommandCost build = Command<Commands::BuildAirport>::Do(DoCommandFlag::Execute, origin, next, 0, id, true);
	if (build.Failed() && build.GetErrorMessage() == STR_ERROR_LOCAL_AUTHORITY_REFUSES_TO_ALLOW_THIS) {
		/* Der Flughafen steht hier laengst - dass die Stadt seinen Ausbau
		 * am Ansehen scheitern laesst, waere Schikane. Derselbe Weg wie
		 * beim Auto-Modus: Stadtrats-Pruefung ueberspringen. */
		build = Command<Commands::BuildAirport>::Do(
				DoCommandFlags{DoCommandFlag::Execute, DoCommandFlag::NoTestTownRating},
				origin, next, 0, id, true);
	}
	if (build.Failed()) {
		/* Rueckbau: lieber der alte Flughafen als gar keiner. */
		Command<Commands::BuildAirport>::Do(DoCommandFlag::Execute, old_tile, old_type, 0, id, true);
		cur_company.Restore();
		Debug(misc, 0, "Flughafen-Ausbau: Neubau abgelehnt ({}), alter Zustand wiederhergestellt",
				build.GetErrorMessage().base());
		return STR_AIRUPGRADE_ERR_BUILD;
	}
	cur_company.Restore();

	Debug(misc, 0, "Flughafen-Ausbau: Station {} von Typ {} auf {} umgebaut ({})",
			id, old_type, next, (int64_t)build.GetCost());
	return STR_AIRUPGRADE_DONE;
}

/** Fork: Diagnose - Ausbau des ersten eigenen Flughafens. */
std::string AirUpgradeDebug(bool apply)
{
	for (Station *st : Station::Iterate()) {
		if (!st->facilities.Test(StationFacility::Airport)) continue;
		if (st->owner != _local_company) continue;
		uint8_t next = AT_INVALID;
		Money cost = AirUpgradeCost(st, next);
		std::string out = fmt::format("Flughafen {}: Typ {} ({} Terminals)", st->index,
				st->airport.type, SpecTerminals(AirportSpec::Get(st->airport.type)));
		if (next == AT_INVALID) {
			out += " - schon der groesste";
		} else {
			out += fmt::format(" -> Typ {} ({} Terminals) fuer {}", next,
					SpecTerminals(AirportSpec::Get(next)), (int64_t)cost);
			if (apply) {
				extern StringID AirUpgradeStart(Station *st);
				StringID res = AirUpgradeStart(st);
				out += fmt::format(" | Ergebnis: {}", res == STR_AIRUPGRADE_DONE ? "umgebaut" : (res == STR_AIRUPGRADE_QUEUED ? "vorgemerkt" : "abgelehnt"));
			}
		}
		Debug(misc, 0, "Ausbau-Diagnose: {}", out);
		return out;
	}
	return "Kein eigener Flughafen gefunden.";
}
