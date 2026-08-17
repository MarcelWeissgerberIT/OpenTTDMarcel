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
	WID_AC_MODE,       ///< Toggle transport mode.
	WID_AC_CARGO,      ///< Toggle cargo type (passengers/mail).
	WID_AC_COUNT_DOWN, ///< Decrease vehicle count.
	WID_AC_COUNT,      ///< Vehicle count display.
	WID_AC_COUNT_UP,   ///< Increase vehicle count.
	WID_AC_ESTIMATE,   ///< Estimate cost without building.
	WID_AC_CHECK,      ///< Scan own network for problems.
	WID_AC_SUGGEST,    ///< Suggest and build a freight link.
	WID_AC_BUILD,      ///< Build the connection.
	WID_AC_STATUS,     ///< Status line.
};

#endif /* WIDGETS_AUTOCONNECT_WIDGET_H */
