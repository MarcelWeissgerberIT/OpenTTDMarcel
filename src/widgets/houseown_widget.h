/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file houseown_widget.h Widgets des Haus-Dialogs (Fork-Feature). */

#ifndef WIDGETS_HOUSEOWN_WIDGET_H
#define WIDGETS_HOUSEOWN_WIDGET_H

/** Widgets of the #HouseInfoWindow class. */
enum HouseOwnWidgets : WidgetID {
	WID_HO_CAPTION, ///< Fenstertitel (Gebaeudename).
	WID_HO_INFO,    ///< Infozeilen (Stadt, Bewohner, Preis, Miete).
	WID_HO_BUY,     ///< Kaufen-Knopf.
	WID_HO_UPGRADE, ///< Ausbauen-Knopf (groesseres Gebaeude).
	WID_HO_RENOVATE, ///< Renovieren-Knopf (verjuengt das Gebaeude).
	WID_HO_VIEW,    ///< Ansicht zentrieren.
	WID_HO_BUY_TOWN,     ///< Alle freien Haeuser der Stadt kaufen.
	WID_HO_UPGRADE_TOWN, ///< Alle eigenen Haeuser der Stadt ausbauen.
};

#endif /* WIDGETS_HOUSEOWN_WIDGET_H */
