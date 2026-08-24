/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file linemgr_widget.h Types related to the line manager (fork feature). */

#ifndef WIDGETS_LINEMGR_WIDGET_H
#define WIDGETS_LINEMGR_WIDGET_H

/** Widgets of the #LineManagerWindow class. */
enum LineManagerWidgets : WidgetID {
	WID_LM_CAPTION,    ///< Fenstertitel.
	WID_LM_FILTER_ALL, ///< Alle Linien zeigen.
	WID_LM_FILTER_TODO,///< Nur Linien mit Handlungsbedarf.
	WID_LM_HEADER,     ///< Spaltenueberschriften.
	WID_LM_LIST,       ///< Die Linienliste.
	WID_LM_SCROLLBAR,  ///< Scrollbalken der Liste.
	WID_LM_SUMMARY,    ///< Zusammenfassung unten.
	WID_LM_APPLY_ALL,  ///< Alle Vorschlaege umsetzen.
	WID_LM_STATUS,     ///< Statuszeile.
};

#endif /* WIDGETS_LINEMGR_WIDGET_H */
