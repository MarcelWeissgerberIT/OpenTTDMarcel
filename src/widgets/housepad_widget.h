/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file housepad_widget.h Types related to the property pad widgets. */

#ifndef WIDGETS_HOUSEPAD_WIDGET_H
#define WIDGETS_HOUSEPAD_WIDGET_H

/** Widgets of the #HousePadWindow class. */
enum HousePadWidgets : WidgetID {
	WID_HP_CAPTION,   ///< Fenstertitel.
	WID_HP_SEARCH,    ///< Eingabefeld: nach Stadt oder Hausart filtern.
	WID_HP_ONLY_RISK, ///< Nur Haeuser zeigen, denen der Abriss droht.
	WID_HP_PANEL,     ///< Knopfraster der Haeuser.
	WID_HP_SCROLLBAR, ///< Scrollbalken des Rasters.
	WID_HP_SUMMARY,   ///< Zusammenfassung: Anzahl und Monatsmiete.
	WID_HP_HINT,      ///< Hinweiszeile zur Bedienung.
};

#endif /* WIDGETS_HOUSEPAD_WIDGET_H */
