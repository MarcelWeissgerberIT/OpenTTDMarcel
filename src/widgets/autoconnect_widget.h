/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file autoconnect_widget.h Widgets of the auto-connect window (fork feature). */

#ifndef WIDGETS_AUTOCONNECT_WIDGET_H
#define WIDGETS_AUTOCONNECT_WIDGET_H

/** Widgets of the #AutoConnectWindow class. */
enum AutoConnectWidgets : WidgetID {
	WID_AC_TOWN_A,     ///< Button: pick town A.
	WID_AC_TOWN_B,     ///< Button: pick town B.
	WID_AC_MODE_AIR,   ///< Umschalter oben: Flugzeuge.
	WID_AC_MODE_BUS,   ///< Umschalter oben: Busse.
	WID_AC_MODE_RAIL,  ///< Umschalter oben: Zuege.
	WID_AC_MODE_SHIP,  ///< Umschalter oben: Schiffe.
	WID_AC_CARGO,      ///< Toggle cargo type (passengers/mail).
	WID_AC_STOPS,      ///< Cycle bus stops per town (auto/1-4).
	WID_AC_TRAINLEN,   ///< Cycle train/platform length (3-7 tiles).
	WID_AC_LINE,       ///< Staedte-Linie: bis zu 8 Staedte nacheinander waehlen.
	WID_AC_TRACTION,   ///< Zug-Antrieb: Auto/Dampf/Diesel/Elektrisch/Einschiene/Maglev.
	WID_AC_PREF,       ///< Fahrzeugwahl: ausgewogen, Kapazitaet oder Tempo.
	WID_AC_COUNT_DOWN, ///< Decrease vehicle count.
	WID_AC_COUNT,      ///< Vehicle count display.
	WID_AC_COUNT_UP,   ///< Increase vehicle count.
	WID_AC_ESTIMATE,   ///< Estimate cost without building.
	WID_AC_CHECK,      ///< Scan own network for problems.
	WID_AC_SUGGEST,    ///< Suggest and build a freight link.
	WID_AC_TERRAFORM,  ///< Toggle: Flaechen fuer Stationen/Flughaefen planieren.
	WID_AC_BIGAIR,     ///< Toggle: groessten verfuegbaren Flughafen bevorzugen.
	WID_AC_BUILD,      ///< Build the connection.
	WID_AC_STATUS,     ///< Status line.
	WID_AC_SEL_LINE,   ///< Blende: Staedte-Linie (nur Busse).
	WID_AC_SEL_CARGO,  ///< Blende: Fracht-Wahl (nicht bei Flugzeugen).
	WID_AC_SEL_STOPS,  ///< Blende: Haltestellen je Stadt (nur Busse).
	WID_AC_SEL_TRAIN,  ///< Blende: Zuglaenge und Antrieb (nur Zuege).
	WID_AC_SEL_GROUND, ///< Blende: Planieren (Flugzeuge und Zuege).
	WID_AC_SEL_BIGAIR, ///< Blende: grosse Flughaefen (nur Flugzeuge).
};

#endif /* WIDGETS_AUTOCONNECT_WIDGET_H */
