/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file newsopt_widget.h Types related to the news settings window (fork feature). */

#ifndef WIDGETS_NEWSOPT_WIDGET_H
#define WIDGETS_NEWSOPT_WIDGET_H

/** Widgets of the #NewsOptWindow class. */
enum NewsOptWidgets : WidgetID {
	WID_NO_CAPTION,  ///< Fenstertitel.
	WID_NO_ALL_OFF,  ///< Alle Meldungen abschalten.
	WID_NO_ALL_SUM,  ///< Alle Meldungen als Kurzmeldung.
	WID_NO_ALL_FULL, ///< Alle Meldungen als Zeitung.
	WID_NO_PANEL,    ///< Liste der Kategorien.
};

#endif /* WIDGETS_NEWSOPT_WIDGET_H */
