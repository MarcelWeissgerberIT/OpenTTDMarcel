/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file airdivert.cpp Ausweichende Flugzeuge (Fork-Feature).
 *
 * Wenn zu viele Flugzeuge denselben Flughafen anfliegen, kreisen sie
 * dort und verbrennen Zeit und Geld. Ein echter Fluglotse wuerde sie
 * auf einen freien Ausweichflughafen schicken - genau das macht diese
 * Datei: sie zaehlt, wie lange ein Flugzeug ueber seinem Ziel kreist,
 * und leitet es bei anhaltendem Stau auf einen anderen eigenen
 * Flughafen in der Naehe um.
 *
 * Bewusst zurueckhaltend: umgeleitet wird nur, wenn das Ausweichziel
 * spuerbar leerer ist, es vom Flugzeug ueberhaupt genutzt werden kann
 * und in Reichweite liegt. Der Auftrag selbst bleibt unangetastet -
 * nach dem Halt geht es planmaessig weiter.
 */

#include "stdafx.h"
#include "aircraft.h"
#include "station_base.h"
#include "airport.h"
#include "order_base.h"
#include "company_base.h"
#include "company_func.h"
#include "vehicle_func.h"
#include "news_func.h"
#include "strings_func.h"
#include "settings_type.h"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"
#include "debug.h"
#include "economy_func.h"
#include "core/backup_type.hpp"
#include "table/strings.h"

#include "safeguards.h"

/** Wie oft ein Flugzeug beim Kreisen ertappt werden muss, bevor es ausweicht. */
static const uint8_t DIVERT_PATIENCE = 6;
/**
 * Wie weit der Ausweichflughafen hoechstens entfernt sein darf (Kacheln).
 *
 * 30 war zu knapp: in einem gewachsenen Netz liegen die Flughaefen
 * weiter auseinander, und ein Flugzeug, das ohnehin seit Minuten kreist,
 * fliegt lieber noch dreissig Kacheln weiter als gar nicht zu landen.
 */
static const uint DIVERT_RANGE = 60;
/** So nah muss ein Flugzeug am Ziel sein, damit es als "kreisend" gilt. */
static const uint DIVERT_CIRCLE_RANGE = 14;

/** Zaehler je Flugzeug: wie lange kreist es schon ueber seinem Ziel? */
static std::map<VehicleID, uint8_t> _divert_waiting;
/** Zuletzt umgeleitete Flugzeuge - nicht sofort erneut umleiten. */
static std::map<VehicleID, uint16_t> _divert_cooldown;

/** Anzahl der Terminals eines Flughafens (Hangars zaehlen nicht mit). */
static uint AirportTerminals(const Station *st)
{
	const AirportFTAClass *apc = st->airport.GetFTA();
	if (apc == nullptr) return 1;
	uint num = 0;
	for (uint i = apc->terminals[0]; i > 0; i--) num += apc->terminals[i];
	return std::max<uint>(1, num);
}

/** Wie viele Flugzeuge steuern diesen Flughafen gerade an? */
static uint AirportInbound(StationID id)
{
	uint n = 0;
	for (const Aircraft *a : Aircraft::Iterate()) {
		if (a->IsNormalAircraft() && a->targetairport == id) n++;
	}
	return n;
}

/**
 * Auslastung eines Flughafens in Prozent der Terminalzahl.
 * 100 heisst: fuer jedes Terminal ist genau ein Flugzeug unterwegs.
 */
static uint AirportPressure(const Station *st)
{
	return AirportInbound(st->index) * 100 / AirportTerminals(st);
}

/**
 * Einen leereren Flughafen in der Naehe suchen.
 * @param v Das kreisende Flugzeug.
 * @param busy Auslastung seines eigentlichen Ziels.
 * @return Ausweichflughafen oder nullptr.
 */
static const Station *FindDivertTarget(const Aircraft *v, uint busy)
{
	const Station *target = Station::GetIfValid(v->targetairport);
	if (target == nullptr) return nullptr;

	const Station *best = nullptr;
	uint best_pressure = busy;
	for (const Station *st : Station::Iterate()) {
		if (st == target) continue;
		if (!st->facilities.Test(StationFacility::Airport)) continue;
		if (st->owner != v->owner) continue;
		if (st->airport.blocks.Test(AirportBlock::AirportClosed)) continue;
		if (!CanVehicleUseStation(v, st)) continue;
		if (DistanceManhattan(st->airport.tile, target->airport.tile) > DIVERT_RANGE) continue;

		uint pressure = AirportPressure(st);
		/* Nur ausweichen, wenn es dort spuerbar ruhiger ist. Frueher
		 * mussten es 60 Prozentpunkte Unterschied sein - in einem Netz,
		 * in dem alle Flughaefen voll sind, kam das nie zustande, und die
		 * Flugzeuge kreisten weiter. 25 Punkte reichen: Hauptsache, das
		 * Ausweichziel ist wirklich freier. */
		if (pressure + 25 > busy) continue;
		if (best == nullptr || pressure < best_pressure) {
			best = st;
			best_pressure = pressure;
		}
	}
	return best;
}

/**
 * Fork: Kreisende Flugzeuge erkennen und umleiten.
 * @param report Diagnosetext statt stiller Arbeit.
 * @return Beschreibung fuer die Konsole.
 */
std::string AirDivertRun(bool report, bool force)
{
	std::string out;
	uint circling = 0, diverted = 0;

	for (auto it = _divert_cooldown.begin(); it != _divert_cooldown.end();) {
		if (it->second <= 1) {
			it = _divert_cooldown.erase(it);
		} else {
			it->second--;
			++it;
		}
	}

	for (Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft() || !Company::IsValidID(v->owner)) continue;
		if (!v->current_order.IsType(OT_GOTO_STATION)) continue;

		const Station *target = Station::GetIfValid(v->targetairport);
		if (target == nullptr || !target->facilities.Test(StationFacility::Airport)) continue;

		/* Kreist es ueber seinem Ziel? Es fliegt noch (kein Terminal frei)
		 * und ist dabei ganz nah dran. */
		bool near_target = DistanceManhattan(v->tile, target->airport.tile) <= DIVERT_CIRCLE_RANGE;
		bool flying = v->state == FLYING;
		if (force && flying) near_target = true; /* Diagnose: Umleitung erzwingen. */
		if (!flying || !near_target) {
			_divert_waiting.erase(v->index);
			continue;
		}

		circling++;
		uint8_t &waited = _divert_waiting[v->index];
		if (waited < 255) waited++;
		if (!force && waited < DIVERT_PATIENCE) continue;
		if (!force && _divert_cooldown.count(v->index) != 0) continue;

		uint busy = AirportPressure(target);
		const Station *alt = FindDivertTarget(v, busy);
		if (report) {
			out += fmt::format("\n  Flugzeug {} kreist ueber {} (Andrang {}%) -> {}",
					v->index.base(), target->index, busy,
					alt != nullptr ? fmt::format("weicht nach {} aus", alt->index) : "keine Alternative");
		}
		if (alt == nullptr) continue;

		/* Umleiten: nur das aktuelle Ziel, der Auftrag bleibt stehen.
		 * Nach dem Halt geht es planmaessig weiter. */
		v->targetairport = alt->index;
		v->dest_tile = alt->airport.tile;
		_divert_waiting.erase(v->index);
		_divert_cooldown[v->index] = 60;
		diverted++;

		if (v->owner == _local_company) {
			AddVehicleNewsItem(GetEncodedString(STR_AIRDIVERT_NEWS, v->index, target->index, alt->index),
					NewsType::Advice, v->index);
		}
		Debug(misc, 0, "Ausweichen: Flugzeug {} von {} nach {} (Andrang {}%)",
				v->index.base(), target->index, alt->index, busy);
	}

	if (report) {
		std::string text = fmt::format("Kreisende Flugzeuge: {}, umgeleitet: {}{}", circling, diverted, out);
		Debug(misc, 0, "Ausweich-Diagnose: {}", text);
		return text;
	}
	return {};
}

/** Alle paar Sekunden nachschauen - oefter waere Verschwendung. */
static const IntervalTimer<TimerGameTick> _airdivert_timer = {{TimerGameTick::Priority::None, 128}, [](auto) {
	if (!_settings_client.gui.fork_airdivert) return;
	AirDivertRun(false, false);
}};

/* ============ Ueberlastete Flughaefen wachsen von selbst ============ */

/**
 * Fork: Wie lange ein Flughafen schon ueberlastet ist.
 *
 * Ein einzelner Andrangs-Spitzenwert sagt nichts - Flugzeuge kommen in
 * Wellen. Erst wenn der Andrang ueber viele Kontrollen hinweg zu hoch
 * bleibt, lohnt der Umbau.
 */
static std::map<StationID, uint16_t> _airgrow_streak;

/** Ab diesem Andrang (Anfluege je Terminal, in Prozent) gilt ein Flughafen als ueberlastet. */
static const uint AIRGROW_PRESSURE = 150;
/** So viele Kontrollen in Folge muss es so bleiben. */
static const uint16_t AIRGROW_STREAK = 8;

/**
 * Fork: Ueberlastete Flughaefen selbst ausbauen lassen.
 *
 * Das Ausweichen hilft nur, solange irgendwo Platz ist. Wenn alle
 * Flughaefen voll sind - genau der Zustand, in dem sich die kreisenden
 * Flugzeuge zu einer Kette auftuermen - bleibt nur: den Flughafen
 * groesser machen. Das passiert hier von selbst, sobald der Andrang
 * dauerhaft zu hoch ist und das Geld reicht.
 *
 * @param report Diagnosetext statt stiller Arbeit.
 * @return Beschreibung fuer die Konsole.
 */
std::string AirGrowRun(bool report, bool force)
{
	extern uint8_t AirUpgradeNextType(uint8_t cur);
	extern Money AirUpgradeCost(const Station *st, uint8_t &next);
	extern StringID AirUpgradeStart(Station *st);

	std::string out;
	uint checked = 0, started = 0;

	for (Station *st : Station::Iterate()) {
		if (!st->facilities.Test(StationFacility::Airport)) continue;
		if (!Company::IsValidID(st->owner)) continue;

		uint pressure = AirportPressure(st);
		uint16_t &streak = _airgrow_streak[st->index];
		if (pressure < AIRGROW_PRESSURE) {
			streak = 0;
			continue;
		}
		checked++;
		if (streak < 0xFFFF) streak++;

		uint8_t next = AT_INVALID;
		Money cost = AirUpgradeCost(st, next);
		if (report) {
			out += fmt::format("\n  Flughafen {}: Andrang {}%, seit {} Kontrollen{}",
					st->index, pressure, streak,
					next == AT_INVALID ? " - schon der groesste" : fmt::format(" -> Ausbau fuer {}", (int64_t)cost));
		}
		if (!force && streak < AIRGROW_STREAK) continue;
		if (next == AT_INVALID) continue;

		/* Nur ausbauen, wenn es die Firma nicht ruiniert. */
		const Company *c = Company::GetIfValid(st->owner);
		if (c == nullptr || c->money < cost * 2) continue;

		StringID res = AirUpgradeStart(st);
		if (res == STR_AIRUPGRADE_DONE || res == STR_AIRUPGRADE_QUEUED) {
			streak = 0;
			started++;
			Debug(misc, 0, "Flughafen waechst mit: Station {} bei Andrang {}% ({})",
					st->index, pressure, res == STR_AIRUPGRADE_DONE ? "sofort" : "vorgemerkt");
			if (st->owner == _local_company) {
				AddNewsItem(GetEncodedString(STR_AIRGROW_NEWS, st->index),
						NewsType::General, NewsStyle::Thin, {});
			}
		}
	}

	if (report) {
		std::string text = fmt::format("Ueberlastete Flughaefen: {}, Ausbau angestossen: {}{}", checked, started, out);
		Debug(misc, 0, "Wachstums-Diagnose: {}", text);
		return text;
	}
	return {};
}

/** Seltener als das Ausweichen - ein Umbau ist eine grosse Sache. */
static const IntervalTimer<TimerGameTick> _airgrow_timer = {{TimerGameTick::Priority::None, 1024}, [](auto) {
	if (!_settings_client.gui.fork_airgrow) return;
	AirGrowRun(false, false);
}};

/**
 * Fork: Stau erzeugen und zusehen, ob der Flughafen mitwaechst.
 *
 * Der Normalfall braucht viele Kontrollen Geduld - fuer den Test wird
 * eine Strecke mit viel zu vielen Flugzeugen gebaut und danach sofort
 * geprueft.
 */
std::string AirGrowStressTest()
{
	extern std::string AutoConnectDebugBuild(std::string_view mode, uint a_idx, uint b_idx, uint count, bool auto_pick);
	extern uint8_t AirUpgradeNextType(uint8_t cur);

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

	std::string built = AutoConnectDebugBuild("air", 0, 1, 60, true);

	Station *st = nullptr;
	for (Station *i : Station::Iterate()) {
		if (i->owner == cid && i->facilities.Test(StationFacility::Airport)) { st = i; break; }
	}
	if (st == nullptr) { local_company.Restore(); cur_company.Restore(); return fmt::format("Kein Flughafen ({})", built); }

	uint8_t before = st->airport.type;
	uint pressure = AirportPressure(st);
	std::string res = AirGrowRun(true, true);
	std::string out = fmt::format("Stresstest: Flughafen {} Typ {}, Andrang {}% -> jetzt Typ {}{}",
			st->index, before, pressure, st->airport.type,
			st->airport.type != before ? " (ausgebaut)" : " (unveraendert)");
	out += "\n" + res;

	local_company.Restore();
	cur_company.Restore();
	return out;
}
