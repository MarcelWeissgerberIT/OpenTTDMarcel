/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file difficulty_widget.h Types related to the difficulty level widgets. */

#ifndef WIDGETS_DIFFICULTY_WIDGET_H
#define WIDGETS_DIFFICULTY_WIDGET_H

/** Widgets of the #DifficultyWindow class. */
enum DifficultyWidgets : WidgetID {
	WID_DL_CAPTION,   ///< Fenstertitel.
	WID_DL_EASY,      ///< Stufe: leicht.
	WID_DL_MEDIUM,    ///< Stufe: mittel.
	WID_DL_HARD,      ///< Stufe: schwer.
	WID_DL_BRUTAL,    ///< Stufe: fast unmoeglich.
	WID_DL_DETAILS,   ///< Beschreibung der gewaehlten Stufe.
	WID_DL_APPLY,     ///< Stufe uebernehmen.
};

#endif /* WIDGETS_DIFFICULTY_WIDGET_H */
