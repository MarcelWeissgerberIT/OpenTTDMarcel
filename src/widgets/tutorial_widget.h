/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file tutorial_widget.h Types related to the guided start widgets. */

#ifndef WIDGETS_TUTORIAL_WIDGET_H
#define WIDGETS_TUTORIAL_WIDGET_H

/** Widgets of the #TutorialWindow class. */
enum TutorialWidgets : WidgetID {
	WID_TU_CAPTION, ///< Fenstertitel.
	WID_TU_STEPS,   ///< Fortschrittsbalken "Schritt x von y".
	WID_TU_PANEL,   ///< Text des aktuellen Schrittes.
	WID_TU_BACK,    ///< Einen Schritt zurueck.
	WID_TU_ACTION,  ///< Schritt-Aktion ausfuehren (z. B. Fenster oeffnen).
	WID_TU_NEXT,    ///< Schritt ueberspringen bzw. abschliessen.
};

#endif /* WIDGETS_TUTORIAL_WIDGET_H */
