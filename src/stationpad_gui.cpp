/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file stationpad_gui.cpp Stationen-Pad (Fork-Feature): alle eigenen
 * Stationen als Knopfraster, filterbar nach Art. Ein Klick springt zur
 * Station; ist oben ein Fahrzeug gewaehlt, haengt der Klick die Station
 * stattdessen an dessen Fahrplan an. Gruene Knoepfe sind Stationen, die
 * das Fahrzeug bereits anfaehrt.
 */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "station_base.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "company_func.h"
#include "viewport_func.h"
#include "zoom_func.h"
#include "dropdown_func.h"
#include "dropdown_type.h"
#include "command_func.h"
#include "order_base.h"
#include "order_cmd.h"
#include "settings_type.h"
#include "error.h"
#include "timer/timer.h"
#include "timer/timer_window.h"

#include "widgets/stationpad_widget.h"

#include "table/strings.h"
#include "table/sprites.h"

#include "safeguards.h"

/** Filterarten des Pads. */
enum class PadFilter : uint8_t {
	All,  ///< Alle Stationen.
	Rail, ///< Bahnhoefe.
	Road, ///< Bus- und LKW-Stationen.
	Air,  ///< Flughaefen.
	Dock, ///< Haefen.
};

static PadFilter _sp_filter = PadFilter::All;
static VehicleID _sp_vehicle = VehicleID::Invalid();

/** Halteauftrag wie im Auftrags-GUI konstruieren. */
static Order MakePadStationOrder(StationID station)
{
	Order o;
	o.MakeGoToStation(station);
	o.SetStopLocation(OrderStopLocation::FarEnd);
	return o;
}

/** Passt die Station zum eingestellten Filter? */
static bool PadMatchesFilter(const Station *st)
{
	switch (_sp_filter) {
		case PadFilter::Rail: return st->facilities.Test(StationFacility::Train);
		case PadFilter::Road: return st->facilities.Any({StationFacility::BusStop, StationFacility::TruckStop});
		case PadFilter::Air:  return st->facilities.Test(StationFacility::Airport);
		case PadFilter::Dock: return st->facilities.Test(StationFacility::Dock);
		default: return true;
	}
}

struct StationPadWindow : Window {
	std::vector<StationID> stations; ///< Angezeigte Stationen (gefiltert, sortiert).
	Scrollbar *vscroll = nullptr;
	uint line_height = 0;
	static const uint COLUMNS = 2;

	StationPadWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_SP_SCROLLBAR);
		this->FinishInitNested(number);
		this->BuildList();
	}

	/** Das gewaehlte Fahrzeug, sofern es noch existiert und uns gehoert. */
	const Vehicle *SelectedVehicle() const
	{
		const Vehicle *v = Vehicle::GetIfValid(_sp_vehicle);
		if (v == nullptr || v->owner != _local_company || !v->IsPrimaryVehicle()) return nullptr;
		return v;
	}

	/** Faehrt das gewaehlte Fahrzeug diese Station bereits an? */
	static bool InOrders(const Vehicle *v, StationID id)
	{
		if (v == nullptr) return false;
		for (const Order &o : v->Orders()) {
			if (o.IsType(OT_GOTO_STATION) && o.GetDestination() == id) return true;
		}
		return false;
	}

	void BuildList()
	{
		this->stations.clear();
		for (const Station *st : Station::Iterate()) {
			if (st->owner != _local_company) continue;
			if (st->facilities.Test(StationFacility::Waypoint)) continue;
			if (!PadMatchesFilter(st)) continue;
			this->stations.push_back(st->index);
		}
		std::sort(this->stations.begin(), this->stations.end(), [](StationID a, StationID b) {
			return GetString(STR_STATION_NAME, a) < GetString(STR_STATION_NAME, b);
		});
		uint rows = static_cast<uint>((this->stations.size() + COLUMNS - 1) / COLUMNS);
		this->vscroll->SetCount(rows);
		this->SetDirty();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_SP_VEHICLE: {
				const Vehicle *v = this->SelectedVehicle();
				return v == nullptr ? GetString(STR_STATIONPAD_NO_VEHICLE) : GetString(STR_STATIONPAD_VEHICLE, v->index);
			}
			case WID_SP_HINT:
				return this->SelectedVehicle() == nullptr
						? GetString(STR_STATIONPAD_HINT_JUMP)
						: GetString(STR_STATIONPAD_HINT_ORDERS);
			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_SP_PANEL) return;
		this->line_height = GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.framerect.Vertical() + ScaleGUITrad(2);
		resize.height = this->line_height;
		size.height = 8 * this->line_height;
		size.width = std::max<uint>(size.width, ScaleGUITrad(300));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_SP_PANEL) return;
		const Vehicle *v = this->SelectedVehicle();
		Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
		uint col_w = ir.Width() / COLUMNS;
		uint first = this->vscroll->GetPosition() * COLUMNS;
		uint last = std::min<uint>(static_cast<uint>(this->stations.size()), first + this->vscroll->GetCapacity() * COLUMNS);

		for (uint i = first; i < last; i++) {
			const Station *st = Station::GetIfValid(this->stations[i]);
			if (st == nullptr) continue;
			uint slot = i - first;
			int x = ir.left + (slot % COLUMNS) * col_w;
			int y = ir.top + (slot / COLUMNS) * this->line_height;
			Rect br = {x + 1, y + 1, x + (int)col_w - 2, y + (int)this->line_height - 2};

			/* Gruen = wird schon angefahren, Grau = Fahrzeug kann hier nicht
			 * halten, sonst Gelb wie die uebrigen Knoepfe im Fork. */
			bool served = InOrders(v, st->index);
			bool usable = v == nullptr || CanVehicleUseStation(v, st);
			Colours colour = served ? Colours::Green : (usable ? Colours::Yellow : Colours::Grey);
			DrawFrameRect(br, colour, served ? FrameFlag::Lowered : FrameFlags{});
			DrawString(br.Shrink(WidgetDimensions::scaled.framerect), GetString(STR_STATION_NAME, st->index),
					usable ? TextColour::Black : TextColour::Grey, AlignmentH::Centre);
		}

		if (this->stations.empty()) {
			DrawString(ir, GetString(STR_STATIONPAD_EMPTY), TextColour::White, AlignmentH::Centre);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_SP_FILTER_ALL:
			case WID_SP_FILTER_RAIL:
			case WID_SP_FILTER_ROAD:
			case WID_SP_FILTER_AIR:
			case WID_SP_FILTER_DOCK:
				_sp_filter = static_cast<PadFilter>(widget - WID_SP_FILTER_ALL);
				this->BuildList();
				break;

			case WID_SP_VEHICLE: {
				DropDownList list;
				list.push_back(MakeDropDownListStringItem(STR_STATIONPAD_NO_VEHICLE, -1));
				for (const Vehicle *v : Vehicle::Iterate()) {
					if (v->owner != _local_company || !v->IsPrimaryVehicle()) continue;
					list.push_back(MakeDropDownListStringItem(GetString(STR_STATIONPAD_VEHICLE_ITEM, v->index), v->index.base()));
				}
				ShowDropDownList(this, std::move(list), this->SelectedVehicle() == nullptr ? -1 : _sp_vehicle.base(), widget);
				break;
			}

			case WID_SP_PANEL: {
				int index = this->SlotAt(pt);
				if (index < 0 || index >= (int)this->stations.size()) break;
				StationID id = this->stations[index];
				const Station *st = Station::GetIfValid(id);
				if (st == nullptr) break;
				const Vehicle *v = this->SelectedVehicle();
				if (v == nullptr) {
					ScrollMainWindowToTile(st->xy);
					break;
				}
				if (!CanVehicleUseStation(v, st)) {
					ShowErrorMessage(GetEncodedString(STR_STATIONPAD_ERR_WRONG_TYPE), {}, WarningLevel::Info);
					break;
				}
				if (InOrders(v, id)) {
					/* Zweiter Klick nimmt die Station wieder aus dem Fahrplan
					 * (von hinten, damit die Indizes gueltig bleiben). */
					auto orders = v->Orders();
					for (int i = static_cast<int>(orders.size()) - 1; i >= 0; i--) {
						if (orders[i].IsType(OT_GOTO_STATION) && orders[i].GetDestination() == id) {
							Command<Commands::DeleteOrder>::Post(STR_ERROR_CAN_T_DELETE_THIS_ORDER, v->tile, v->index, static_cast<VehicleOrderID>(i));
						}
					}
				} else {
					Command<Commands::InsertOrder>::Post(STR_ERROR_CAN_T_INSERT_NEW_ORDER, v->tile, v->index,
							static_cast<VehicleOrderID>(v->GetNumOrders()), MakePadStationOrder(id));
				}
				this->SetDirty();
				break;
			}
		}
	}

	/** Rasterplatz unter dem Mauszeiger; -1 wenn daneben. */
	int SlotAt(Point pt) const
	{
		Rect r = this->GetWidget<NWidgetBase>(WID_SP_PANEL)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
		if (!IsInsideMM(pt.y, r.top, r.bottom + 1) || !IsInsideMM(pt.x, r.left, r.right + 1)) return -1;
		uint col_w = r.Width() / COLUMNS;
		uint col = std::min<uint>(COLUMNS - 1, (pt.x - r.left) / std::max<uint>(1, col_w));
		uint row = (pt.y - r.top) / std::max<uint>(1, this->line_height);
		return static_cast<int>((this->vscroll->GetPosition() + row) * COLUMNS + col);
	}

	/** Rechtsklick springt immer zur Station, auch mit gewaehltem Fahrzeug. */
	bool OnRightClick([[maybe_unused]] Point pt, WidgetID widget) override
	{
		if (widget != WID_SP_PANEL) return false;
		int index = this->SlotAt(pt);
		if (index >= 0 && index < (int)this->stations.size()) {
			const Station *st = Station::GetIfValid(this->stations[index]);
			if (st != nullptr) ScrollMainWindowToTile(st->xy);
		}
		return true;
	}

	void OnDropdownSelect(WidgetID widget, int index, int) override
	{
		if (widget != WID_SP_VEHICLE) return;
		_sp_vehicle = index < 0 ? VehicleID::Invalid() : VehicleID(index);
		this->SetDirty();
	}

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_SP_FILTER_ALL, _sp_filter == PadFilter::All);
		this->SetWidgetLoweredState(WID_SP_FILTER_RAIL, _sp_filter == PadFilter::Rail);
		this->SetWidgetLoweredState(WID_SP_FILTER_ROAD, _sp_filter == PadFilter::Road);
		this->SetWidgetLoweredState(WID_SP_FILTER_AIR, _sp_filter == PadFilter::Air);
		this->SetWidgetLoweredState(WID_SP_FILTER_DOCK, _sp_filter == PadFilter::Dock);
		this->DrawWidgets();
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_SP_PANEL);
	}

	/** Stationen und Fahrplaene aendern sich im Spiel - Liste nachziehen. */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		this->BuildList();
	}

	const IntervalTimer<TimerWindow> rebuild_interval = {std::chrono::seconds(2), [this](auto) {
		this->BuildList();
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_stationpad_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_SP_CAPTION), SetStringTip(STR_STATIONPAD_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_SP_FILTER_ALL), SetFill(1, 0), SetStringTip(STR_STATIONPAD_FILTER_ALL, STR_STATIONPAD_FILTER_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_SP_FILTER_RAIL), SetFill(1, 0), SetStringTip(STR_STATIONPAD_FILTER_RAIL, STR_STATIONPAD_FILTER_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_SP_FILTER_ROAD), SetFill(1, 0), SetStringTip(STR_STATIONPAD_FILTER_ROAD, STR_STATIONPAD_FILTER_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_SP_FILTER_AIR), SetFill(1, 0), SetStringTip(STR_STATIONPAD_FILTER_AIR, STR_STATIONPAD_FILTER_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_SP_FILTER_DOCK), SetFill(1, 0), SetStringTip(STR_STATIONPAD_FILTER_DOCK, STR_STATIONPAD_FILTER_TOOLTIP),
			EndContainer(),
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_SP_VEHICLE), SetFill(1, 0), SetToolTip(STR_STATIONPAD_VEHICLE_TOOLTIP),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PANEL, Colours::DarkGreen, WID_SP_PANEL), SetFill(1, 1), SetResize(1, 1), SetScrollbar(WID_SP_SCROLLBAR), EndContainer(),
				NWidget(NWID_VSCROLLBAR, Colours::DarkGreen, WID_SP_SCROLLBAR),
			EndContainer(),
			NWidget(WWT_TEXT, Colours::Invalid, WID_SP_HINT), SetFill(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_SPACER), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, Colours::DarkGreen),
	EndContainer(),
};

static WindowDesc _stationpad_desc(
	WindowPosition::Automatic, "stationpad", 320, 260,
	WindowClass::StationPad, WindowClass::None,
	{},
	_nested_stationpad_widgets
);

/** Stationen-Pad oeffnen (Fork-Feature). */
void ShowStationPadWindow()
{
	AllocateWindowDescFront<StationPadWindow>(_stationpad_desc, 0);
}
