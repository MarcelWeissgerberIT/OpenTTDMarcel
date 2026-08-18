/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file pixelstudio_widget.h Widgets des Pixel-Studios (Fork-Feature). */

#ifndef WIDGETS_PIXELSTUDIO_WIDGET_H
#define WIDGETS_PIXELSTUDIO_WIDGET_H

/** Widgets of the #PixelStudioWindow class. */
enum PixelStudioWidgets : WidgetID {
	WID_PS_CAPTION,     ///< Fenstertitel.
	WID_PS_FILTER_TRAIN, ///< Filter: Zuege anzeigen.
	WID_PS_FILTER_ROAD,  ///< Filter: Strassenfahrzeuge anzeigen.
	WID_PS_FILTER_SHIP,  ///< Filter: Schiffe anzeigen.
	WID_PS_FILTER_AIR,   ///< Filter: Flugzeuge anzeigen.
	WID_PS_FILTER_HOUSE, ///< Filter: Haeuser anzeigen.
	WID_PS_ENGINE_LIST, ///< Liste der bearbeitbaren Fahrzeuge.
	WID_PS_SCROLLBAR,   ///< Scrollbar der Liste.
	WID_PS_VIEW_PREV,   ///< Vorherige Blickrichtung.
	WID_PS_VIEW_LABEL,  ///< Anzeige "Ansicht x/8".
	WID_PS_VIEW_NEXT,   ///< Naechste Blickrichtung.
	WID_PS_CANVAS,      ///< Pixelraster zum Malen.
	WID_PS_PALETTE,     ///< 16x16-Farbpalette.
	WID_PS_COLOUR,      ///< Anzeige der gewaehlten Farbe.
	WID_PS_TOOL_PENCIL, ///< Werkzeug Stift.
	WID_PS_TOOL_FILL,   ///< Werkzeug Fuellen.
	WID_PS_TOOL_PICK,   ///< Werkzeug Pipette.
	WID_PS_TOOL_ERASE,  ///< Werkzeug Radierer.
	WID_PS_COPY,        ///< Ansicht als Bild in die Zwischenablage.
	WID_PS_PASTE,       ///< Bild aus der Zwischenablage einfuegen.
	WID_PS_UNDO,        ///< Rueckgaengig.
	WID_PS_RESET,       ///< Auf Original zuruecksetzen.
	WID_PS_SAVE,        ///< Uebernehmen und dauerhaft speichern.
	WID_PS_PREVIEW,     ///< Vorschau in Spielgroesse.
};

#endif /* WIDGETS_PIXELSTUDIO_WIDGET_H */
