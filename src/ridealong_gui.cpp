/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file ridealong_gui.cpp Mitfahr-Modus (Fork-Feature): die Hauptkamera
 * heftet sich an ein Fahrzeug, zoomt ganz heran und blendet die
 * Oberflaeche aus. Unten laeuft eine schmale Leiste mit Fahrzeugname,
 * Tempo und naechstem Halt mit. Beenden stellt alles wieder her.
 */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "vehicle_base.h"
#include "vehicle_gui.h"
#include "viewport_func.h"
#include "zoom_func.h"
#include "station_base.h"
#include "order_base.h"
#include "company_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"

#include "widgets/ridealong_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/** Zustand vor der Mitfahrt, um ihn danach zurueckzugeben. */
static ZoomLevel _ride_old_zoom = ZoomLevel::Min;
static VehicleID _ride_vehicle = VehicleID::Invalid();

/** So nah wie die Einstellungen es erlauben ans Fahrzeug heranzoomen. */
static void RideZoomIn(Window *main_w)
{
	for (int i = 0; i < 8 && main_w->viewport->zoom > _settings_client.gui.zoom_min; i++) {
		if (!DoZoomInOutWindow(ZOOM_IN, main_w)) break;
	}
}

/** Auf den vor der Mitfahrt eingestellten Zoom zurueck. */
static void RideRestoreZoom(Window *main_w)
{
	for (int i = 0; i < 8 && main_w->viewport->zoom < _ride_old_zoom; i++) {
		if (!DoZoomInOutWindow(ZOOM_OUT, main_w)) break;
	}
	for (int i = 0; i < 8 && main_w->viewport->zoom > _ride_old_zoom; i++) {
		if (!DoZoomInOutWindow(ZOOM_IN, main_w)) break;
	}
}

void StartRideAlong(VehicleID veh);

struct RideAlongWindow : Window {
	RideAlongWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->InitNested(number);
		this->PlaceAtBottom();
	}

	/** Die Leiste sitzt mittig am unteren Bildschirmrand. */
	void PlaceAtBottom()
	{
		this->left = (_screen.width - this->width) / 2;
		this->top = _screen.height - this->height - ScaleGUITrad(8);
		this->SetDirty();
	}

	const Vehicle *Ride() const
	{
		return Vehicle::GetIfValid(_ride_vehicle);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		const Vehicle *v = this->Ride();
		if (v == nullptr) return this->Window::GetWidgetString(widget, stringid);
		switch (widget) {
			case WID_RA_INFO: {
				std::string s = GetString(STR_RIDEALONG_SPEED, v->index, v->GetDisplaySpeed());
				/* Naechstes Ziel aus dem laufenden Auftrag. */
				const Order *o = v->GetNumOrders() > 0 ? v->GetOrder(v->cur_real_order_index) : nullptr;
				if (o != nullptr && o->IsType(OT_GOTO_STATION)) {
					s += GetString(STR_RIDEALONG_NEXT_STOP, o->GetDestination().ToStationID());
				} else if (o != nullptr && o->IsType(OT_GOTO_DEPOT)) {
					s += GetString(STR_RIDEALONG_NEXT_DEPOT);
				}
				return s;
			}
			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget == WID_RA_STOP) this->Close();
		if (widget == WID_RA_CAB) {
			/* Der Fuehrerstand uebernimmt; die Mitfahrt endet dabei. */
			extern void ShowCabView(VehicleID veh);
			VehicleID veh = _ride_vehicle;
			this->Close();
			ShowCabView(veh);
		}
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		/* Kamera loslassen, Zoom und Oberflaeche zurueckgeben. */
		Window *main_w = GetMainWindow();
		if (main_w != nullptr && main_w->viewport != nullptr) {
			if (main_w->viewport->follow_vehicle == _ride_vehicle) {
				main_w->viewport->follow_vehicle = VehicleID::Invalid();
			}
			RideRestoreZoom(main_w);
		}
		if (FindWindowById(WindowClass::MainToolbar, 0) == nullptr) ShowVitalWindows();
		_ride_vehicle = VehicleID::Invalid();
		this->Window::Close(data);
	}

	/** Tempo und Halt laufen mit; ein verschwundenes Fahrzeug beendet die Fahrt. */
	const IntervalTimer<TimerWindow> tick = {std::chrono::milliseconds(200), [this](auto) {
		if (this->Ride() == nullptr) {
			this->Close();
			return;
		}
		this->PlaceAtBottom();
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_ridealong_widgets = {
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0), SetPadding(WidgetDimensions::unscaled.framerect),
			NWidget(WWT_TEXT, Colours::Invalid, WID_RA_INFO), SetMinimalSize(360, 12), SetFill(1, 0),
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_RA_CAB), SetMinimalSize(110, 12), SetStringTip(STR_RIDEALONG_CAB, STR_RIDEALONG_CAB_TOOLTIP),
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_RA_STOP), SetMinimalSize(110, 12), SetStringTip(STR_RIDEALONG_STOP, STR_RIDEALONG_STOP_TOOLTIP),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _ridealong_desc(
	WindowPosition::Manual, {}, 0, 0,
	WindowClass::RideAlong, WindowClass::None,
	{},
	_nested_ridealong_widgets
);

/**
 * Fork: Konsolen-Einstieg "ridealong [index]" - ohne Angabe faehrt das
 * erste eigene Fahrzeug. Liefert false, wenn es keins gibt.
 */
bool StartRideAlongConsole(int index)
{
	VehicleID pick = VehicleID::Invalid();
	if (index >= 0) {
		pick = VehicleID(index);
	} else {
		for (const Vehicle *v : Vehicle::Iterate()) {
			if (v->owner == _local_company && v->IsPrimaryVehicle()) { pick = v->index; break; }
		}
	}
	if (!Vehicle::IsValidID(pick)) return false;
	StartRideAlong(pick);
	return true;
}

/**
 * Fork: Mitfahrt starten - Kamera ans Fahrzeug, ganz heranzoomen,
 * Oberflaeche ausblenden und die Mitfahr-Leiste zeigen.
 */
void StartRideAlong(VehicleID veh)
{
	const Vehicle *v = Vehicle::GetIfValid(veh);
	if (v == nullptr || _game_mode != GameMode::Normal) return;
	Window *main_w = GetMainWindow();
	if (main_w == nullptr || main_w->viewport == nullptr) return;

	/* Laeuft schon eine Mitfahrt, wird sie sauber beendet. */
	CloseWindowById(WindowClass::RideAlong, 0);

	_ride_vehicle = v->GetMovingFront()->index;
	_ride_old_zoom = main_w->viewport->zoom;
	main_w->viewport->follow_vehicle = _ride_vehicle;
	RideZoomIn(main_w);

	/* Freie Sicht: Werkzeugleiste, Statusleiste und alle offenen
	 * Fenster weg - sonst faehrt man hinter Dialogen mit. */
	CloseAllNonVitalWindows();
	CloseWindowById(WindowClass::MainToolbar, 0);
	CloseWindowById(WindowClass::Statusbar, 0);
	AllocateWindowDescFront<RideAlongWindow>(_ridealong_desc, 0);
}
