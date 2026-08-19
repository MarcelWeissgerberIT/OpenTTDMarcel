/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file assistant_widget.h Widgets des persoenlichen Assistenten (Fork-Feature). */

#ifndef WIDGETS_ASSISTANT_WIDGET_H
#define WIDGETS_ASSISTANT_WIDGET_H

/** Widgets of the #AssistantWindow class. */
enum AssistantWidgets : WidgetID {
	WID_AS_CAPTION,        ///< Fenstertitel.
	WID_AS_MODERNIZE,      ///< Schalter: Fahrzeuge automatisch modernisieren.
	WID_AS_RENEW,          ///< Schalter: alte Fahrzeuge automatisch erneuern.
	WID_AS_RENEW_LABEL,    ///< Beschriftung des Erneuerungs-Sliders.
	WID_AS_RENEW_SLIDER,   ///< Slider: Monate vor Lebensende erneuern.
	WID_AS_TOWN_LABEL,     ///< Beschriftung Stadtpflege-Budget.
	WID_AS_TOWN_SLIDER,    ///< Slider: Stadtpflege-Budget pro Monat.
	WID_AS_AD_LABEL,       ///< Beschriftung Werbebudget.
	WID_AS_AD_SLIDER,      ///< Slider: Werbebudget pro Monat.
	WID_AS_TV,             ///< Knopf: Werbespot im Fernsehen ansehen.
	WID_AS_LOG,            ///< Protokoll der letzten Assistenten-Aktionen.
};

/** Widgets of the #AssistantTvWindow class. */
enum AssistantTvWidgets : WidgetID {
	WID_ATV_CAPTION, ///< Fenstertitel.
	WID_ATV_SCREEN,  ///< Der Bildschirm mit dem laufenden Spot.
};

#endif /* WIDGETS_ASSISTANT_WIDGET_H */
