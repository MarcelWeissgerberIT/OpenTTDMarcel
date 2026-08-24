/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file fleet_widget.h Types related to the fleet window (fork feature). */

#ifndef WIDGETS_FLEET_WIDGET_H
#define WIDGETS_FLEET_WIDGET_H

/** Widgets of the #FleetWindow class. */
enum FleetWidgets : WidgetID {
	WID_FL_CAPTION,     ///< Fenstertitel.
	WID_FL_VEHICLE,     ///< Zeile: welches Fahrzeug vervielfacht wird.
	WID_FL_COUNT_DOWN,  ///< Weniger Kopien.
	WID_FL_COUNT,       ///< Anzahl der Kopien.
	WID_FL_COUNT_UP,    ///< Mehr Kopien.
	WID_FL_SHARE,       ///< Umschalter: Auftraege teilen.
	WID_FL_TIMETABLE,   ///< Umschalter: gleichmaessig takten.
	WID_FL_COST,        ///< Geschaetzte Kosten.
	WID_FL_BUILD,       ///< Kopien bauen.
	WID_FL_SPREAD,      ///< Linie jetzt gleichmaessig takten.
	WID_FL_MODEL,       ///< Dropdown: neues Modell fuer den Tausch.
	WID_FL_REPLACE,     ///< Alle baugleichen Fahrzeuge ersetzen.
	WID_FL_STATUS,      ///< Statuszeile.
};

#endif /* WIDGETS_FLEET_WIDGET_H */
