/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file citizen.h Stadtleben 3.0 (Fork-Feature): Buerger mit Zielen, Zivilautos, Klick-Info. */

#ifndef CITIZEN_H
#define CITIZEN_H

#include "tile_type.h"
#include "town_type.h"
#include <vector>

struct TileInfo;

/** Darstellungsart eines Buergers. */
enum class CitizenKind : uint8_t {
	Adult,    ///< Einzelner Erwachsener (Geh-Animation).
	Family,   ///< Erwachsener mit Kind an der Hand.
	Child,    ///< Kind allein.
	Stroller, ///< Person mit Kinderwagen.
	Car,      ///< Zivilauto (faehrt aus einer Parkluecke los und kehrt zurueck).
};

/** Was der Buerger gerade vorhat (nur Anzeige). */
enum class CitizenGoal : uint8_t {
	Station,  ///< Geht zum Bahnhof/zur Haltestelle.
	Visit,    ///< Besucht Freunde (verschwindet in einem Haus).
	Home,     ///< Geht nach Hause.
	Shopping, ///< Geht einkaufen.
	Drive,    ///< Faehrt eine Runde mit dem Auto.
	Stroll,   ///< Bummelt ohne festes Ziel durch die Stadt.
};

/** Lebenszustand: unterwegs oder gerade in einem Gebaeude/zu Hause. */
enum class CitizenState : uint8_t {
	Walking,  ///< Sichtbar auf der Strasse unterwegs.
	Dwelling, ///< Unsichtbar am Ziel (Haus/Laden/Bahnhof), wartet auf den naechsten Ausflug.
};

/** Ein Bewohner (nicht im Spielstand gespeichert - reine Atmosphaere). */
struct Citizen {
	uint32_t id;                  ///< Stabile ID; Name/Alter/Interesse werden daraus abgeleitet.
	TownID town;                  ///< Heimatstadt.
	TileIndex home;               ///< Wohnhaus (bzw. Parkluecke beim Auto).
	std::vector<TileIndex> path;  ///< Strassenkacheln vom Start zum Ziel.
	uint16_t pos;                 ///< Index der aktuellen Kachel in #path.
	uint8_t sub;                  ///< Fortschritt innerhalb der Kachel (0..15).
	CitizenKind kind;
	CitizenGoal goal;
	CitizenState state;    ///< Unterwegs oder verweilend.
	uint64_t dwell_until;  ///< Tick, ab dem der naechste Ausflug geplant wird.
	uint8_t stroll_legs;   ///< Verbleibende Bummel-Etappen vor dem Heimweg.
};

void RunCitizensTick();
void DrawCitizensOnTile(const TileInfo *ti);
bool IsParkedCarAway(TileIndex tile);
bool CheckClickOnCitizen(int world_x, int world_y);
void ClearCitizens();

#endif /* CITIZEN_H */
