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
#include "settings_type.h"
#include "openttd.h"
#include "gfx_func.h"
#include "engine_base.h"
#include "engine_func.h"
#include "order_base.h"
#include "order_cmd.h"
#include "vehicle_cmd.h"
#include "timer/timer_game_tick.h"
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

/* ==================== Mega-Flughafen: Diagnose ==================== */

/**
 * Fork: Den Mega-Flughafen bauen und seine Flugsteuerung pruefen.
 *
 * Der Test baut ihn neben der ersten Stadt und schaut nach, ob der
 * Automat vollstaendig ist: jede Position muss fuer jedes Ziel einen
 * Ausgang haben, sonst bliebe dort irgendwann ein Flugzeug stehen.
 */
std::string MegaAirportTest()
{
	const AirportSpec *as = AirportSpec::Get(AT_MEGA);
	if (as == nullptr || as->fsm == nullptr) return "Mega-Flughafen fehlt.";

	std::string out = fmt::format("Mega-Flughafen: {}x{} Kacheln, {} Terminals, {} Positionen",
			as->size_x, as->size_y, SpecTerminals(as), as->fsm->nofelements);

	/* Vollstaendigkeit: hat jede Bodenposition einen Ausgang fuer jedes Ziel?
	 *
	 * Nur Bodenpositionen zaehlen. Punkte in der Luft und auf den Bahnen
	 * haben absichtlich nur einen Ausgang - ein Flugzeug im Landeanflug
	 * hat kein anderes Ziel als die Bahn. Haengen bleiben kann es nur
	 * dort, wo es rollt oder parkt. */
	uint gaps = 0;
	uint checked = 0;
	std::string missing;
	for (uint pos = 0; pos < as->fsm->nofelements; pos++) {
		const AirportFTA *fta = &as->fsm->layout[pos];
		if (fta->position != pos) continue; /* Flugpositionen ohne Eintrag */

		const AirportMovingData *amd = &as->fsm->moving_data[pos];
		bool on_ground = amd->x >= 0 && amd->y >= 0 &&
				amd->x < (int)(as->size_x * TILE_SIZE) && amd->y < (int)(as->size_y * TILE_SIZE) &&
				!amd->flags.Any({AirportMovingDataFlag::Land, AirportMovingDataFlag::Takeoff,
						AirportMovingDataFlag::HeliRaise, AirportMovingDataFlag::HeliLower,
						AirportMovingDataFlag::Brake, AirportMovingDataFlag::NoSpeedClamp});
		if (!on_ground) continue;
		checked++;

		for (uint8_t heading = HANGAR; heading <= MAX_HEADINGS; heading++) {
			/* Nur Ziele pruefen, die dieser Flughafen ueberhaupt kennt. */
			if (heading > TERM8 && heading < TERM9 && heading != HELIPAD1 && heading != HELIPAD2) continue;
			bool found = false;
			for (const AirportFTA *p = fta; p != nullptr; p = p->next.get()) {
				if (p->heading == heading || p->heading == TO_ALL) { found = true; break; }
			}
			if (!found) {
				gaps++;
				if (missing.size() < 120) missing += fmt::format(" {}:{}", pos, heading);
			}
		}
	}
	out += gaps == 0 ? fmt::format("\n  Automat vollstaendig - alle {} Bodenpositionen kennen jedes Ziel", checked)
			: fmt::format("\n  {} Luecken im Automaten ({} Bodenpositionen geprueft):{}", gaps, checked, missing);

	Debug(misc, 0, "Mega-Test: {}", out);
	return out;
}

/* ============ Mega-Flughafen: Probebetrieb mit echten Flugzeugen ============ */

/**
 * Fork: Zustand des Probebetriebs.
 *
 * Der Automaten-Test oben prueft nur die Tabelle. Ob Flugzeuge auch
 * wirklich landen, parken und wieder starten, zeigt erst der Betrieb -
 * dafuer laesst dieser Test zwei Mega-Flughaefen anlegen, kauft eine
 * Flotte und schaut in Abstaenden nach, ob sich noch etwas bewegt.
 */
struct MegaFlyTest {
	bool pending = false;                ///< Startwunsch, wartet auf ein laufendes Spiel.
	bool running = false;
	uint checks = 0;                     ///< Wie oft schon nachgeschaut wurde.
	std::vector<VehicleID> fleet;
	std::map<VehicleID, uint> stuck;     ///< Wie oft ein Flugzeug unveraendert dastand.
	std::map<VehicleID, uint32_t> last;  ///< Letzte gesehene Stellung.
	CompanyID owner = CompanyID::Invalid();
	StationID a = StationID::Invalid();
	StationID b = StationID::Invalid();
};
static MegaFlyTest _megafly;

static std::string MegaFlyTestRun();

/** Halteauftrag bauen, wie ihn das Auftrags-Fenster erzeugt. */
static Order MegaStationOrder(StationID station)
{
	Order o;
	o.MakeGoToStation(station);
	o.SetStopLocation(OrderStopLocation::FarEnd);
	return o;
}

/**
 * Eine Stelle suchen, an der ein Flughafen des Typs Platz hat.
 * @param as Der Flughafentyp.
 * @param avoid Kachel, um die herum nicht gesucht wird (der andere Flughafen).
 * @return Die Ecke, oder INVALID_TILE.
 */
static TileIndex MegaFindSite(const AirportSpec *as, TileIndex avoid)
{
	for (uint tries = 0; tries < 4000; tries++) {
		TileIndex t = TileXY(8 + (tries * 7) % (Map::SizeX() - as->size_x - 16),
				8 + (tries * 13) % (Map::SizeY() - as->size_y - 16));
		/* Nah beieinander: der Testflug soll kurz sein, damit in der
		 * knappen Testzeit mehrere volle Runden zustande kommen. */
		if (avoid != INVALID_TILE && (DistanceManhattan(t, avoid) < 16 || DistanceManhattan(t, avoid) > 40)) continue;
		if (!as->IsWithinMapBounds(0, t)) continue;
		/* Nicht auf Meereshoehe bauen: dort planiert der Test das Gelaende
		 * bis auf den Wasserspiegel, und die naechste Flut nimmt den
		 * Flughafen samt Flugzeugen mit. */
		if (TileHeight(t) < 2) continue;

		/* Erst planieren, dann bauen - sonst scheitert es an jeder Delle. */
		for (uint dy = 0; dy < as->size_y; dy++) {
			for (uint dx = 0; dx < as->size_x; dx++) {
				Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute, t + TileDiffXY(dx, dy));
			}
		}
		Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, t,
				t + TileDiffXY(as->size_x - 1, as->size_y - 1), false, LevelMode::Level);

		CommandCost cc = Command<Commands::BuildAirport>::Do(DoCommandFlag::Execute, t, AT_MEGA, 0, StationID::Invalid(), false);
		if (cc.Succeeded()) return t;
		/* Nicht jeder Fehlschlag traegt eine Meldung - GetString wuerde auf
		 * einer leeren StringID abstuerzen. */
		if (tries < 3) {
			Debug(misc, 0, "Mega-Flug: Bau bei {} abgelehnt: {}", t,
					cc.GetErrorMessage() == INVALID_STRING_ID ? "ohne Begruendung" : GetString(cc.GetErrorMessage()));
		}

	}
	return INVALID_TILE;
}

/**
 * Fork: Den Probebetrieb starten.
 *
 * Baut zwei Mega-Flughaefen, kauft Flugzeuge und schickt sie hin und her.
 * Das Ergebnis meldet der Timer weiter unten.
 */
std::string MegaFlyTestStart()
{
	/* Aus der Konsole heraus kann das Spiel noch beim Hochfahren sein -
	 * dann sind die Spieleinstellungen leer und jeder Bau scheitert.
	 * Deshalb nur vormerken; der Timer startet, sobald es laeuft. */
	if (_game_mode != GameMode::Normal || _settings_game.station.station_spread == 0) {
		_megafly = MegaFlyTest{};
		_megafly.pending = true;
		return "Probebetrieb vorgemerkt - startet, sobald das Spiel laeuft.";
	}
	return MegaFlyTestRun();
}

/** Fork: Der eigentliche Aufbau des Probebetriebs. */
static std::string MegaFlyTestRun()
{
	const AirportSpec *as = AirportSpec::Get(AT_MEGA);
	if (as == nullptr || as->fsm == nullptr) return "Mega-Flughafen fehlt.";
	if (!as->IsAvailable()) return fmt::format("Mega-Flughafen gibt es erst ab {}.", as->min_year);

	/* Headless (null-Video) gibt es keine Spielerfirma - fuer den Test
	 * eine gruenden, sonst gehoert der Flughafen niemandem. */
	const Company *co = nullptr;
	for (const Company *i : Company::Iterate()) { co = i; break; }
	if (co == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		co = DoStartupNewCompany(false, CompanyID::Invalid());
	}
	if (co == nullptr) return "Keine Firma vorhanden.";
	CompanyID cid = co->index;
	Backup<CompanyID> cur_company(_current_company, cid);

	/* Genug Geld, damit der Test nicht am Kontostand scheitert. Der
	 * Geld-Cheat wirkt nur ueber den Kommando-Wrapper, deshalb direkt
	 * verbuchen - sonst geht die Testfirma pleite und alle Flugzeuge
	 * verschwinden mitsamt der Firma. */
	SubtractMoneyFromCompany(cid, CommandCost(ExpensesType::Other, -Money(2000000000)));

	TileIndex ta = MegaFindSite(as, INVALID_TILE);
	if (ta == INVALID_TILE) { cur_company.Restore(); return "Kein Platz fuer den ersten Mega-Flughafen."; }
	TileIndex tb = MegaFindSite(as, ta);
	if (tb == INVALID_TILE) { cur_company.Restore(); return "Kein Platz fuer den zweiten Mega-Flughafen."; }

	Station *sa = Station::GetByTile(ta);
	Station *sb = Station::GetByTile(tb);

	/* Ein Flugzeug aussuchen, das es zu dieser Zeit gibt. */
	EngineID engine = EngineID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Aircraft)) {
		if (!e->IsEnabled() || !IsEngineBuildable(e->index, VehicleType::Aircraft, cid)) continue;
		engine = e->index;
		break;
	}
	if (engine == EngineID::Invalid()) { cur_company.Restore(); return "Kein baubares Flugzeug gefunden."; }

	TileIndex hangar = sa->airport.GetHangarTile(0);
	_megafly = MegaFlyTest{};
	for (uint i = 0; i < 24; i++) {
		auto [cost, veh_id, cap, mail, caps] =
				Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, hangar, engine, true, INVALID_CARGO, ClientID::Invalid);
		if (cost.Failed()) break;
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 0, MegaStationOrder(sa->index));
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_id, 1, MegaStationOrder(sb->index));
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_id, false);
		_megafly.fleet.push_back(veh_id);
	}
	cur_company.Restore();

	if (_megafly.fleet.empty()) return "Keine Flugzeuge gebaut - Test nicht moeglich.";

	bool was_pending = _megafly.pending;
	_megafly.running = true;
	_megafly.pending = false;
	(void)was_pending;
	/* Zeitraffer an: der Probebetrieb braucht Spielzeit, und headless
	 * laeuft die Uhr sonst nur im Schneckentempo. */
	extern void ChangeGameSpeed(bool enable_fast_forward);
	_settings_client.gui.fast_forward_speed_limit = 2500;
	ChangeGameSpeed(true);
	_megafly.owner = cid;
	_megafly.a = sa->index;
	_megafly.b = sb->index;
	uint alive = 0;
	for (VehicleID id : _megafly.fleet) { if (Aircraft::GetIfValid(id) != nullptr) alive++; }
	return fmt::format("Probebetrieb laeuft: 2 Mega-Flughaefen, {} Flugzeuge gebaut, {} davon sofort gueltig.",
			_megafly.fleet.size(), alive);
}

/**
 * Fork: Alle 600 Ticks nachschauen, ob sich noch etwas bewegt.
 *
 * Ein Flugzeug gilt als haengend, wenn es bei fuenf Kontrollen in Folge
 * dieselbe Stellung hat, ohne im Hangar oder beim Beladen zu sein. Genau
 * dieser Fall waere der Fehler, den eine luekenhafte Flugsteuerung
 * erzeugt: das Flugzeug findet von seiner Position kein Ziel mehr.
 */
static const IntervalTimer<TimerGameTick> _megafly_timer = {{TimerGameTick::Priority::None, 100}, [](auto) {
	if (_megafly.pending) {
		if (_game_mode != GameMode::Normal || _settings_game.station.station_spread == 0) return;
		_megafly.pending = false;
		Debug(misc, 0, "Mega-Flug: {}", MegaFlyTestRun());
		return;
	}
	if (!_megafly.running) return;

	_megafly.checks++;
	uint flying = 0, parked = 0, hangared = 0, gone = 0;
	for (VehicleID id : _megafly.fleet) {
		const Aircraft *v = Aircraft::GetIfValid(id);
		if (v == nullptr) { gone++; continue; }
		/* Stellung = Position im Automaten plus Bildschirmkoordinaten. */
		uint32_t sig = ((uint32_t)v->state << 24) ^ ((uint32_t)v->x_pos << 12) ^ (uint32_t)v->y_pos;
		if (_megafly.last.count(id) != 0 && _megafly.last[id] == sig) {
			_megafly.stuck[id]++;
		} else {
			_megafly.stuck[id] = 0;
		}
		_megafly.last[id] = sig;

		if (v->state == FLYING) flying++;
		else if (v->state == HANGAR) hangared++;
		else parked++;

		/* Wer am Terminal, auf einem Helipad oder im Hangar steht, darf
		 * stehen - der laedt gerade. Haengen kann nur, wer unterwegs ist. */
		bool resting = v->state == HANGAR ||
				(v->state >= TERM1 && v->state <= HELIPAD2) ||
				v->state == TERM7 || v->state == TERM8 || v->state == HELIPAD3 ||
				(v->state >= TERM9 && v->state <= TERM16);
		if (resting) _megafly.stuck[id] = 0;
	}

	/* Zwischenstaende nur auf Wunsch (-d misc=1) - sonst waere das Log
	 * bei jeder Kontrolle voll. */
	uint total = 0;
	for (const Aircraft *a : Aircraft::Iterate()) { if (a->IsPrimaryVehicle()) total++; }
	Debug(misc, 1, "Mega-Flug: Kontrolle {}: {} fliegen, {} am Boden, {} im Hangar, {} verschwunden (im Spiel: {})",
			_megafly.checks, flying, parked, hangared, gone, total);

	if (_megafly.checks < 14) return;

	uint stuck = 0;
	std::string worst;
	for (const auto &[id, n] : _megafly.stuck) {
		if (n < 5) continue;
		stuck++;
		const Aircraft *v = Aircraft::GetIfValid(id);
		if (worst.size() < 100 && v != nullptr) worst += fmt::format(" #{}(Zustand {}, Pos {})", id, v->state, v->pos);
	}

	Debug(misc, 0, "Mega-Flug ERGEBNIS: {} Flugzeuge - {} in der Luft, {} am Boden, {} im Hangar, {} weg; {}",
			_megafly.fleet.size(), flying, parked, hangared, gone,
			stuck == 0 ? "keines haengt fest" : fmt::format("{} haengen fest:{}", stuck, worst));
	ChangeGameSpeed(false);
	_megafly.running = false;
}};
