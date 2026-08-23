/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stationpad_widget.h Types related to the station pad widgets. */

#ifndef WIDGETS_STATIONPAD_WIDGET_H
#define WIDGETS_STATIONPAD_WIDGET_H

/** Widgets of the #StationPadWindow class. */
enum StationPadWidgets : WidgetID {
	WID_SP_CAPTION,      ///< Fenstertitel.
	WID_SP_FILTER_ALL,   ///< Filter: alle Stationen.
	WID_SP_FILTER_RAIL,  ///< Filter: Bahnhoefe.
	WID_SP_FILTER_ROAD,  ///< Filter: Bus- und LKW-Stationen.
	WID_SP_FILTER_AIR,   ///< Filter: Flughaefen.
	WID_SP_FILTER_DOCK,  ///< Filter: Haefen.
	WID_SP_SEARCH,       ///< Eingabefeld: Stationen nach Namen filtern.
	WID_SP_VEHICLE,      ///< Fahrzeugwahl fuer die Fahrplan-Bearbeitung.
	WID_SP_PANEL,        ///< Knopfraster der Stationen.
	WID_SP_SCROLLBAR,    ///< Scrollbalken des Rasters.
	WID_SP_HINT,         ///< Hinweiszeile.
};

#endif /* WIDGETS_STATIONPAD_WIDGET_H */
