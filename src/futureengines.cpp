/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file futureengines.cpp Zukunfts-Fahrzeuge bis zum Jahr 2500 (Fork-Feature).
 *
 * Im Original ist nach dem letzten Modell Schluss: wer bis 2200 spielt,
 * kauft dieselben Flugzeuge wie 2050. Dieser Fork setzt die Entwicklung
 * fort - ohne eine einzige neue Grafik. Jede Generation nimmt sich das
 * beste vorhandene Modell ihrer Gattung als Vorbild, uebernimmt dessen
 * Aussehen und legt bei Groesse, Tempo und Leistung nach. Aus 200 Sitzen
 * werden so ueber die Jahrhunderte 1000.
 *
 * Technisch sind das echte, eigenstaendige Modelle (wie NewGRF-Fahrzeuge,
 * nur ohne GRF-Datei): eigene Engine-Eintraege mit eigenen Namen, eigenem
 * Erscheinungsjahr und eigenen Werten. Sie tauchen im Kaufmenue auf,
 * sobald ihr Jahr erreicht ist, und lassen sich ganz normal per
 * Autoreplace einsetzen.
 */

#include "stdafx.h"
#include "engine_base.h"
#include "engine_func.h"
#include "engine_type.h"
#include "vehicle_type.h"
#include "rail.h"
#include "cargotype.h"
#include "landscape_type.h"
#include "settings_type.h"
#include "company_func.h"
#include "company_base.h"
#include "timer/timer_game_calendar.h"
#include "table/strings.h"
#include "debug.h"

#include "safeguards.h"

/**
 * Erste NewGRF-freie Kennung fuer Fork-Modelle. Weit oberhalb der
 * Original-Fahrzeuge (hoechstens 116) und der ueblichen NewGRF-IDs,
 * damit sich nichts in die Quere kommt.
 */
static constexpr uint16_t FUTURE_FIRST_ID = 600;

static constexpr int FUTURE_GENERATIONS = 12;                      ///< Wieviele Stufen es gibt.
static constexpr int FUTURE_FIRST_YEAR = 2050;                     ///< Jahr der ersten Stufe.
static constexpr int FUTURE_YEAR_STEP = 40;                        ///< Abstand zwischen den Stufen.
static constexpr uint16_t FUTURE_MAX_AIRCRAFT_PAX = 1000;          ///< Grenze fuer Flugzeug-Sitze.
static constexpr uint16_t FUTURE_MAX_AIRCRAFT_SPEED = 8000;        ///< Grenze fuers Tempo (intern rund km/h).
static constexpr uint16_t FUTURE_MAX_HELI_PAX = 200;               ///< Grenze fuer Hubschrauber-Sitze.
static constexpr uint16_t FUTURE_MAX_HELI_SPEED = 900;             ///< Grenze fuers Hubschrauber-Tempo.

/** Eine Fahrzeug-Gattung, die weiterentwickelt wird. */
struct FutureLine {
	VehicleType type;
	const char *name;   ///< Namensstamm, z. B. "Aerobus".
	uint8_t slot;       ///< Feste Nummer der Linie (fuer stabile Kennungen).
};

/**
 * Die Gattungen. Die Reihenfolge ist Teil der Kennung und darf sich
 * nicht mehr aendern, sonst zeigen alte Spielstaende auf andere Modelle.
 */
static const FutureLine _future_lines[] = {
	{VehicleType::Aircraft, "Aerobus",     0}, ///< Grossraumjet: viele Sitze.
	{VehicleType::Aircraft, "Stratos",     1}, ///< Schnelljet: hohes Tempo.
	{VehicleType::Train,    "Titan",       2}, ///< Schiene.
	{VehicleType::Train,    "Voltus",      3}, ///< Elektrisch.
	{VehicleType::Train,    "Monorail",    4}, ///< Einschienenbahn.
	{VehicleType::Train,    "Levitan",     5}, ///< Magnetschwebebahn.
	{VehicleType::Road,     "Metrobus",    6}, ///< Bus.
	{VehicleType::Road,     "Cargomax",    7}, ///< Lastwagen.
	{VehicleType::Ship,     "Ozeanriese",  8}, ///< Schiff.
	{VehicleType::Aircraft, "Rotorex",     9}, ///< Hubschrauber: sonst waeren ab
	                                           ///< etwa 2050 gar keine mehr im Angebot.
};

/** Kennung eines Fork-Modells: Linie und Generation eindeutig verpackt. */
static uint16_t FutureInternalID(const FutureLine &line, int gen)
{
	return static_cast<uint16_t>(FUTURE_FIRST_ID + line.slot * 32 + gen);
}

/** Ist das eine Original-Engine (also ein moegliches Vorbild)? */
static bool IsOriginalEngine(const Engine *e)
{
	return e->grf_prop.local_id < GetOriginalEngineCount(e->type);
}

/** Passt die Engine ins aktuelle Klima? */
static bool FitsClimate(const Engine *e)
{
	return e->info.climates.Test(LandscapeType{_settings_game.game_creation.landscape});
}

/**
 * Das Vorbild einer Linie suchen: die beste Original-Engine ihrer Gattung.
 * @param line Die Gattung.
 * @return Vorbild oder nullptr, wenn es in diesem Klima keines gibt.
 */
static const Engine *FindTemplate(const FutureLine &line)
{
	const Engine *best = nullptr;
	int best_score = -1;
	for (const Engine *e : Engine::IterateType(line.type)) {
		if (!IsOriginalEngine(e) || !FitsClimate(e)) continue;
		int score = -1;
		switch (line.type) {
			case VehicleType::Aircraft: {
				const AircraftVehicleInfo &avi = e->VehInfo<AircraftVehicleInfo>();
				bool is_heli = (avi.subtype & AIR_CTOL) == 0;
				/* Linie 9 sucht Hubschrauber, die anderen Flaechenflugzeuge -
				 * ein Helikopter taugt nicht als Vorbild fuer einen Grossraumjet
				 * und umgekehrt. */
				if (is_heli != (line.slot == 9)) continue;
				score = (line.slot == 0 || line.slot == 9) ? avi.passenger_capacity : avi.max_speed;
				break;
			}
			case VehicleType::Train: {
				const RailVehicleInfo &rvi = e->VehInfo<RailVehicleInfo>();
				if (rvi.railveh_type == RailVehicleType::Wagon) continue;
				static const RailType wanted[] = {RAILTYPE_RAIL, RAILTYPE_ELECTRIC, RAILTYPE_MONO, RAILTYPE_MAGLEV};
				if (!rvi.intended_railtypes.Test(wanted[line.slot - 2])) continue;
				score = rvi.power;
				break;
			}
			case VehicleType::Road: {
				const RoadVehicleInfo &rvi = e->VehInfo<RoadVehicleInfo>();
				bool pax = IsValidCargoType(e->info.cargo_type) &&
						CargoSpec::Get(e->info.cargo_type)->classes.Test(CargoClass::Passengers);
				/* Linie 6 sucht Busse, Linie 7 Lastwagen. */
				if (pax != (line.slot == 6)) continue;
				score = rvi.capacity * 4 + rvi.max_speed;
				break;
			}
			case VehicleType::Ship: {
				const ShipVehicleInfo &svi = e->VehInfo<ShipVehicleInfo>();
				score = svi.capacity;
				break;
			}
			default: break;
		}
		if (score > best_score) {
			best_score = score;
			best = e;
		}
	}
	return best;
}

/**
 * Gleichmaessig vom Ausgangswert zum Zielwert der letzten Generation.
 *
 * Bewusst nicht prozentual: sonst haengt es vom Vorbild ab, wann eine
 * Grenze erreicht ist - und mehrere Generationen waeren wertgleich. So
 * legt jede Stufe sichtbar zu, und die letzte trifft genau das Ziel.
 *
 * @param base Wert des Vorbilds.
 * @param target Wert der letzten Generation (wird bei @p cap gekappt).
 * @param gen Generation, 1-basiert.
 * @param cap Obergrenze des Datenfeldes.
 */
static uint Interpolate(uint base, uint64_t target, int gen, uint cap)
{
	target = std::min<uint64_t>(target, cap);
	if (target <= base) return std::min<uint>(base, cap);
	uint64_t v = base + (target - base) * gen / FUTURE_GENERATIONS;
	return static_cast<uint>(std::min<uint64_t>(v, cap));
}

/**
 * Ein Fork-Modell mit Werten fuellen.
 * @param e Das (frisch angelegte) Modell.
 * @param base Das Vorbild.
 * @param line Die Gattung.
 * @param gen Generation, 1-basiert.
 */
static void FillFutureEngine(Engine *e, const Engine *base, const FutureLine &line, int gen, uint &last_value)
{
	e->info = base->info;
	e->original_image_index = base->original_image_index;
	e->info.base_intro = TimerGameCalendar::ConvertYMDToDate(
			TimerGameCalendar::Year{FUTURE_FIRST_YEAR + (gen - 1) * FUTURE_YEAR_STEP}, 0, 1);
	e->info.base_life = TimerGameCalendar::Year{0xFF}; /* Bleibt dauerhaft im Angebot. */
	e->info.lifelength = TimerGameCalendar::Year{40};
	e->info.variant_id = EngineID::Invalid();
	e->info.string_id = base->info.string_id; /* Ruecktfall; sichtbar ist der eigene Name. */

	uint value = 0; /* Kennzahl, die im Namen steht. */
	switch (line.type) {
		case VehicleType::Aircraft: {
			AircraftVehicleInfo avi = base->VehInfo<AircraftVehicleInfo>();
			if (line.slot == 9) {
				/* Hubschrauber bleiben Hubschrauber: sie landen weiter auf
				 * Plattformen, werden aber groesser und schneller. Die Grenzen
				 * sind bewusst niedriger als bei den Jets. */
				avi.passenger_capacity = static_cast<uint16_t>(Interpolate(avi.passenger_capacity, FUTURE_MAX_HELI_PAX, gen, FUTURE_MAX_HELI_PAX));
				avi.mail_capacity = static_cast<uint8_t>(Interpolate(avi.mail_capacity, avi.mail_capacity * 4ULL, gen, 255));
				avi.max_speed = static_cast<uint16_t>(Interpolate(avi.max_speed, avi.max_speed * 2ULL, gen, FUTURE_MAX_HELI_SPEED));
				value = avi.passenger_capacity;
			} else if (line.slot == 0) {
				avi.passenger_capacity = static_cast<uint16_t>(Interpolate(avi.passenger_capacity, FUTURE_MAX_AIRCRAFT_PAX, gen, FUTURE_MAX_AIRCRAFT_PAX));
				avi.mail_capacity = static_cast<uint8_t>(Interpolate(avi.mail_capacity, avi.mail_capacity * 4ULL, gen, 255));
				avi.max_speed = static_cast<uint16_t>(Interpolate(avi.max_speed, avi.max_speed * 3ULL / 2, gen, FUTURE_MAX_AIRCRAFT_SPEED));
				value = avi.passenger_capacity;
			} else {
				avi.max_speed = static_cast<uint16_t>(Interpolate(avi.max_speed, avi.max_speed * 2ULL, gen, FUTURE_MAX_AIRCRAFT_SPEED));
				avi.passenger_capacity = static_cast<uint16_t>(Interpolate(avi.passenger_capacity, avi.passenger_capacity * 3ULL, gen, FUTURE_MAX_AIRCRAFT_PAX));
				avi.mail_capacity = static_cast<uint8_t>(Interpolate(avi.mail_capacity, avi.mail_capacity * 3ULL, gen, 255));
				/* Der interne Wert entspricht bereits ungefaehr km/h. */
				value = avi.max_speed;
			}
			avi.max_range = static_cast<uint16_t>(Interpolate(avi.max_range, avi.max_range * 4ULL, gen, 65000));
			avi.cost_factor = static_cast<uint8_t>(Interpolate(avi.cost_factor, avi.cost_factor * 5ULL, gen, 255));
			avi.running_cost = static_cast<uint8_t>(Interpolate(avi.running_cost, avi.running_cost * 3ULL, gen, 255));
			e->VehInfo<AircraftVehicleInfo>() = avi;
			break;
		}
		case VehicleType::Train: {
			RailVehicleInfo rvi = base->VehInfo<RailVehicleInfo>();
			rvi.power = static_cast<uint16_t>(Interpolate(rvi.power, rvi.power * 8ULL, gen, 60000));
			rvi.max_speed = static_cast<uint16_t>(Interpolate(rvi.max_speed, rvi.max_speed * 5ULL / 2, gen, 4000));
			rvi.tractive_effort = static_cast<uint8_t>(Interpolate(rvi.tractive_effort, rvi.tractive_effort * 3ULL, gen, 255));
			rvi.capacity = static_cast<uint8_t>(Interpolate(rvi.capacity, rvi.capacity * 3ULL, gen, 255));
			rvi.cost_factor = static_cast<uint8_t>(Interpolate(rvi.cost_factor, rvi.cost_factor * 4ULL, gen, 255));
			rvi.running_cost = static_cast<uint8_t>(Interpolate(rvi.running_cost, rvi.running_cost * 5ULL / 2, gen, 255));
			e->VehInfo<RailVehicleInfo>() = rvi;
			value = rvi.power;
			break;
		}
		case VehicleType::Road: {
			RoadVehicleInfo rvi = base->VehInfo<RoadVehicleInfo>();
			rvi.capacity = static_cast<uint8_t>(Interpolate(rvi.capacity, rvi.capacity * 4ULL, gen, 255));
			rvi.max_speed = static_cast<uint8_t>(Interpolate(rvi.max_speed, rvi.max_speed * 2ULL, gen, 255));
			rvi.power = static_cast<uint8_t>(Interpolate(rvi.power, rvi.power * 4ULL, gen, 255));
			rvi.tractive_effort = static_cast<uint8_t>(Interpolate(rvi.tractive_effort, rvi.tractive_effort * 3ULL, gen, 255));
			rvi.cost_factor = static_cast<uint8_t>(Interpolate(rvi.cost_factor, rvi.cost_factor * 4ULL, gen, 255));
			rvi.running_cost = static_cast<uint8_t>(Interpolate(rvi.running_cost, rvi.running_cost * 5ULL / 2, gen, 255));
			e->VehInfo<RoadVehicleInfo>() = rvi;
			value = rvi.capacity;
			break;
		}
		case VehicleType::Ship: {
			ShipVehicleInfo svi = base->VehInfo<ShipVehicleInfo>();
			svi.capacity = static_cast<uint16_t>(Interpolate(svi.capacity, svi.capacity * 6ULL, gen, 65000));
			svi.max_speed = static_cast<uint8_t>(Interpolate(svi.max_speed, svi.max_speed * 2ULL, gen, 255));
			svi.cost_factor = static_cast<uint8_t>(Interpolate(svi.cost_factor, svi.cost_factor * 4ULL, gen, 255));
			svi.running_cost = static_cast<uint8_t>(Interpolate(svi.running_cost, svi.running_cost * 5ULL / 2, gen, 255));
			e->VehInfo<ShipVehicleInfo>() = svi;
			value = svi.capacity;
			break;
		}
		default: break;
	}

	/* Der Name traegt die Kennzahl - so sieht man im Kaufmenue sofort,
	 * was die neue Generation kann. Stoesst ein Wert an seine Grenze,
	 * unterscheidet die Generationsziffer die Modelle weiterhin. */
	e->name = (value > last_value)
			? fmt::format("{} {}", line.name, value)
			: fmt::format("{} {} G{}", line.name, value, gen);
	last_value = std::max(last_value, value);
}

/**
 * Fork: Die Zukunfts-Modelle anlegen, soweit sie noch fehlen.
 *
 * Wird bei jedem Spielstart und nach jedem Laden aufgerufen und ist
 * absichtlich idempotent: vorhandene Modelle werden nur aufgefrischt,
 * fehlende ergaenzt. So bekommen auch alte Spielstaende die neuen
 * Generationen, ohne dass sich Kennungen verschieben.
 */
void ForkEnsureFutureEngines()
{
	if (!_settings_client.gui.fork_future_engines) return;

	uint created = 0;
	for (const FutureLine &line : _future_lines) {
		const Engine *base = FindTemplate(line);
		if (base == nullptr) continue;
		uint last_value = 0;

		for (int gen = 1; gen <= FUTURE_GENERATIONS; gen++) {
			uint16_t id = FutureInternalID(line, gen);
			EngineID existing = _engine_mngr.GetID(line.type, id, INVALID_GRFID);
			Engine *e;
			if (existing != EngineID::Invalid()) {
				e = Engine::Get(existing);
			} else {
				if (!Engine::CanAllocateItem()) return;
				e = Engine::Create(line.type, id);
				_engine_mngr.SetID(line.type, id, INVALID_GRFID,
						static_cast<uint8_t>(std::min<uint16_t>(id, GetOriginalEngineCount(line.type))), e->index);
				created++;
			}
			FillFutureEngine(e, base, line, gen, last_value);
		}
	}
	if (created > 0) Debug(misc, 1, "Zukunfts-Modelle: {} neu angelegt", created);
}

/** Fork: Diagnose - welche Zukunfts-Modelle gibt es und ab wann? */
std::string ForkFutureEnginesDebug()
{
	std::string out;
	uint n = 0;
	uint avail = 0;
	/* Headless laeuft ohne eigene Firma - dann die erste nehmen. */
	CompanyID who = _local_company;
	if (!Company::IsValidID(who)) {
		for (const Company *c : Company::Iterate()) { who = c->index; break; }
	}
	if (!Company::IsValidID(who)) {
		/* Diagnose ohne Firma (headless): fuer die Pruefung eine gruenden. */
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		const Company *c = DoStartupNewCompany(false, CompanyID::Invalid());
		if (c != nullptr) who = c->index;
	}
	for (const Engine *e : Engine::Iterate()) {
		if (e->grf_prop.local_id < FUTURE_FIRST_ID) continue;
		n++;
		bool buildable = Company::IsValidID(who) && IsEngineBuildable(e->index, e->type, who);
		if (buildable) avail++;
		if (n <= 12 || buildable) {
			out += fmt::format("\n{} ({}): ab {}{}", e->name, (int)e->type,
					TimerGameCalendar::ConvertDateToYMD(e->info.base_intro).year.base(),
					buildable ? " [KAUFBAR]" : "");
		}
	}
	out = fmt::format(" im Jahr {} ({} davon kaufbar){}", TimerGameCalendar::year.base(), avail, out);
	std::string res = fmt::format("Zukunfts-Modelle: {}{}", n, out);
	Debug(misc, 0, "{}", res);
	return res;
}
