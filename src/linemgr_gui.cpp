/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file linemgr_gui.cpp Linien-Manager (Fork-Feature).
 *
 * Wer zweihundert Fahrzeuge besitzt, verwaltet keine Fahrzeuge mehr -
 * er verwaltet Linien. Dieses Fenster fasst alle Fahrzeuge mit
 * gemeinsamen Auftraegen zu einer Linie zusammen und zeigt, woran es
 * hakt: halb leere Fahrzeuge fressen Unterhalt, ueberfuellte Bahnsteige
 * kosten Fahrgaeste.
 *
 * Der Vorschlag rechnet auf eine Zielauslastung: zu viele Fahrzeuge
 * werden zum Verkauf ins Depot gerufen, zu wenige nachgebaut. Beides
 * laeuft ueber den Tages-Timer, nie mitten im Klick - ein verkauftes
 * Fahrzeug schliesst sonst Fenster, die gerade selbst zeichnen.
 */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "vehicle_gui.h"
#include "vehicle_cmd.h"
#include "order_base.h"
#include "station_base.h"
#include "company_base.h"
#include "company_func.h"
#include "command_func.h"
#include "core/backup_type.hpp"
#include "core/geometry_func.hpp"
#include "error.h"
#include "debug.h"
#include "viewport_func.h"
#include "zoom_func.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_window.h"
#include "widgets/linemgr_widget.h"
#include "table/strings.h"
#include "table/sprites.h"

#include "safeguards.h"

/** Zielauslastung in Prozent: darunter stehen Fahrzeuge leer herum. */
static const int LM_TARGET_OCCUPANCY = 70;
/** Hoechstens so viele Fahrzeuge je Klick bauen oder verkaufen. */
static const int LM_MAX_STEP = 8;

/* ==================== Verkauf ueber den Tages-Timer ==================== */

/** Ein Fahrzeug, das im Depot verkauft werden soll. */
struct LmPendingSell {
	VehicleID vehicle;
	CompanyID owner;
	int days_left; ///< Frist, danach faehrt es wieder mit.
};
static std::vector<LmPendingSell> _lm_pending_sells;

/** Taeglich: im Depot angekommene Fahrzeuge verkaufen. */
static const IntervalTimer<TimerGameCalendar> _lm_timer = {{TimerGameCalendar::Trigger::Day, TimerGameCalendar::Priority::None}, [](auto) {
	for (auto it = _lm_pending_sells.begin(); it != _lm_pending_sells.end();) {
		const Vehicle *v = Vehicle::GetIfValid(it->vehicle);
		if (v == nullptr) {
			it = _lm_pending_sells.erase(it);
			continue;
		}
		if (--it->days_left <= 0) {
			/* Aufgeben: lieber weiterfahren lassen als im Depot vergessen. */
			if (v->IsChainInDepot() && v->vehstatus.Test(VehState::Stopped)) {
				Backup<CompanyID> cur_company(_current_company, it->owner);
				Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, v->index, false);
				cur_company.Restore();
			}
			it = _lm_pending_sells.erase(it);
			continue;
		}
		if (!v->IsChainInDepot()) {
			++it;
			continue;
		}
		Backup<CompanyID> cur_company(_current_company, it->owner);
		CommandCost res = Command<Commands::SellVehicle>::Do(DoCommandFlag::Execute, v->index, true, false, ClientID::Invalid);
		cur_company.Restore();
		Debug(misc, 0, "Linien: Fahrzeug {} {}", it->vehicle.base(), res.Succeeded() ? "verkauft" : "Verkauf abgelehnt");
		it = _lm_pending_sells.erase(it);
	}
}};

/* ==================== Kennzahlen je Linie ==================== */

/**
 * Eine Linie: alle Fahrzeuge, die dieselbe Route fahren.
 *
 * Bewusst nicht ueber geteilte Auftraege gebildet - wer Fahrzeuge
 * einzeln kopiert hat, hat acht Auftragslisten fuer eine Strecke.
 * Fuer den Spieler ist das trotzdem eine Linie.
 */
struct LineInfo {
	VehicleID first{};      ///< Stellvertretendes Fahrzeug der Linie.
	std::vector<VehicleID> members; ///< Alle Fahrzeuge dieser Route.
	VehicleType type{};     ///< Fahrzeugart (fuer das Symbol).
	uint vehicles = 0;      ///< Anzahl Fahrzeuge.
	int occupancy = 0;      ///< Mittlere Auslastung der letzten Fahrten (%).
	bool has_occupancy = false; ///< Hat schon jemand eine Fahrt beendet?
	bool all_measured = false;  ///< Haben alle Fahrzeuge schon Zahlen geliefert?
	Money profit = 0;       ///< Gewinn im laufenden Jahr.
	uint waiting = 0;       ///< Wartende Fracht an den Stationen der Linie.
	uint capacity = 0;      ///< Gesamtkapazitaet aller Fahrzeuge.
	int advice = 0;         ///< Vorschlag: + bauen, - verkaufen, 0 passt.
	TileIndex tile = INVALID_TILE; ///< Sprungziel auf der Karte.
	std::string name;       ///< "Anfang - Ende"
};

static std::vector<LineInfo> _lm_lines;

/** Kapazitaet eines ganzen Fahrzeugs (alle Teile zusammen). */
static uint LmCapacity(const Vehicle *v)
{
	uint cap = 0;
	for (const Vehicle *u = v; u != nullptr; u = u->Next()) cap += u->cargo_cap;
	return cap;
}

/** Frachtarten, die diese Linie ueberhaupt befoerdern kann. */
static CargoTypes LmCargoMask(const std::vector<VehicleID> &members)
{
	CargoTypes mask{};
	for (VehicleID id : members) {
		const Vehicle *v = Vehicle::GetIfValid(id);
		if (v == nullptr) continue;
		for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
			if (u->cargo_cap > 0 && IsValidCargoType(u->cargo_type)) mask.Set(u->cargo_type);
		}
	}
	return mask;
}

/** Wartende Fracht an allen Stationen der Linie zusammenzaehlen. */
static uint LmWaitingCargo(const Vehicle *first, CargoTypes mask)
{
	uint waiting = 0;
	for (const Order &o : first->Orders()) {
		if (!o.IsType(OT_GOTO_STATION)) continue;
		const Station *st = Station::GetIfValid(o.GetDestination().ToStationID());
		if (st == nullptr) continue;
		for (CargoType c : EnumRange(NUM_CARGO)) {
			if (!mask.Test(c)) continue;
			const GoodsEntry &ge = st->goods[c];
			if (!ge.HasData()) continue;
			waiting += ge.GetData().cargo.AvailableCount();
		}
	}
	return waiting;
}

/** Name der Linie: erste und letzte angefahrene Station. */
static std::string LmLineName(const Vehicle *first, TileIndex &tile)
{
	std::vector<StationID> stops;
	for (const Order &o : first->Orders()) {
		if (!o.IsType(OT_GOTO_STATION)) continue;
		StationID id = o.GetDestination().ToStationID();
		if (Station::IsValidID(id) && (stops.empty() || stops.back() != id)) stops.push_back(id);
	}
	if (stops.empty()) return GetString(STR_LINEMGR_NO_STOPS);
	const Station *st = Station::Get(stops.front());
	tile = st->xy;
	if (stops.size() == 1) return GetString(STR_STATION_NAME, stops.front());
	std::string out = GetString(STR_LINEMGR_ROUTE, stops.front(), stops.back());
	if (stops.size() > 2) out += GetString(STR_LINEMGR_ROUTE_MORE, (uint)stops.size() - 2);
	return out;
}

/**
 * Wieviele Fahrzeuge fehlen (oder sind zu viel)?
 *
 * Grundlage ist die Auslastung der letzten Fahrten: bei 35 Prozent
 * Auslastung tut es die Haelfte der Flotte, bei 95 Prozent und vollen
 * Bahnsteigen braucht es mehr. Wartende Fracht zaehlt extra, sonst
 * bliebe eine ueberfuellte Strecke unentdeckt, deren Fahrzeuge
 * zwangslaeufig immer voll sind.
 */
static int LmAdvice(const LineInfo &line)
{
	if (line.vehicles == 0 || !line.has_occupancy) return 0;

	/* Wieviele Fahrzeuge braeuchte es fuer die Zielauslastung? Beim
	 * Ausbau aufrunden (lieber ein Fahrzeug zu viel als stehende
	 * Fahrgaeste), beim Abbau kaufmaennisch runden. */
	double ratio = (double)line.occupancy / LM_TARGET_OCCUPANCY;
	int want = line.occupancy > LM_TARGET_OCCUPANCY
			? (int)std::ceil(line.vehicles * ratio)
			: (int)std::lround(line.vehicles * ratio);
	want = std::max(1, want);

	/* Wartende Fracht zaehlt nur, wenn die Fahrzeuge auch voll ankommen.
	 * Bei halb leeren Fahrzeugen ist genug Platz da - was dort wartet,
	 * steigt beim naechsten Halt ein. Und nur ein Drittel des Staus auf
	 * einmal: ein aufgestauter Berg ist kein Dauerzustand. */
	int extra = 0;
	if (line.waiting > 0 && line.capacity > 0 && line.occupancy >= 60) {
		uint per_vehicle = std::max<uint>(1, line.capacity / line.vehicles);
		extra = (int)(line.waiting / (per_vehicle * 3));
		if (extra > 0) want = std::max(want, (int)line.vehicles + extra);
	}

	int advice = want - (int)line.vehicles;
	/* Nicht wegen einer Kleinigkeit die Flotte umbauen. */
	if (advice > 0 && line.occupancy < 80 && extra == 0) advice = 0;
	if (advice < 0 && line.occupancy > LM_TARGET_OCCUPANCY - 15) advice = 0;
	/* Frisch gebaute Fahrzeuge haben noch keine Fahrt hinter sich - erst
	 * abwarten, sonst verkauft der naechste Klick sie gleich wieder. */
	if (advice < 0 && !line.all_measured) advice = 0;
	return Clamp(advice, -std::max(0, (int)line.vehicles - 1), LM_MAX_STEP);
}

/** Routen-Schluessel: Fahrzeugart und die angefahrenen Stationen. */
static std::string LmRouteKey(const Vehicle *v)
{
	std::vector<uint16_t> stops;
	for (const Order &o : v->Orders()) {
		if (!o.IsType(OT_GOTO_STATION)) continue;
		stops.push_back(o.GetDestination().ToStationID().base());
	}
	if (stops.empty()) return {};
	/* Reihenfolge egal, Startpunkt egal: sortiert vergleichen. Zwei Busse
	 * derselben Strecke gehoeren zusammen, auch wenn einer rueckwaerts
	 * eingetragen wurde. */
	std::sort(stops.begin(), stops.end());
	stops.erase(std::unique(stops.begin(), stops.end()), stops.end());
	std::string key = fmt::format("{}", (int)v->type);
	for (uint16_t s : stops) key += fmt::format(":{}", s);
	return key;
}

/** Alle Linien der eigenen Firma einsammeln. */
static void LmCollect()
{
	_lm_lines.clear();
	if (!Company::IsValidID(_local_company)) return;

	/* Erst nach Route gruppieren ... */
	std::map<std::string, std::vector<VehicleID>> routes;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle() || v->owner != _local_company) continue;
		std::string key = LmRouteKey(v);
		if (key.empty()) continue; /* Ohne Halt: das findet die Netz-Diagnose. */
		routes[key].push_back(v->index);
	}

	/* ... dann je Gruppe die Kennzahlen rechnen. */
	for (auto &[key, members] : routes) {
		const Vehicle *first = Vehicle::GetIfValid(members.front());
		if (first == nullptr) continue;

		LineInfo line;
		line.first = first->index;
		line.members = members;
		line.type = first->type;
		int occ_sum = 0;
		uint occ_count = 0;
		for (VehicleID id : members) {
			const Vehicle *w = Vehicle::GetIfValid(id);
			if (w == nullptr) continue;
			line.vehicles++;
			line.profit += w->GetDisplayProfitThisYear();
			line.capacity += LmCapacity(w);
			if (w->trip_occupancy > 0) {
				occ_sum += w->trip_occupancy;
				occ_count++;
			}
		}
		line.has_occupancy = occ_count > 0;
		/* Vier von fuenf reichen: in einer grossen Flotte ist immer eines
		 * frisch im Depot. */
		line.all_measured = occ_count * 5 >= line.vehicles * 4;
		line.occupancy = occ_count > 0 ? occ_sum / (int)occ_count : 0;
		line.waiting = LmWaitingCargo(first, LmCargoMask(members));
		line.name = LmLineName(first, line.tile);
		line.advice = LmAdvice(line);
		_lm_lines.push_back(std::move(line));
	}

	/* Handlungsbedarf nach oben, danach die groessten Verlustbringer. */
	std::sort(_lm_lines.begin(), _lm_lines.end(), [](const LineInfo &a, const LineInfo &b) {
		if ((a.advice != 0) != (b.advice != 0)) return a.advice != 0;
		if (a.profit != b.profit) return a.profit < b.profit;
		return a.name < b.name;
	});
}

/* ==================== Vorschlaege umsetzen ==================== */

/** Passendes Depot zum Nachbauen finden (dieselbe Logik wie im Flotten-Fenster). */
extern TileIndex FleetFindDepotFor(const Vehicle *v);
extern void FleetSpreadLine(const Vehicle *v);
extern bool FleetEnsureLimitFor(CompanyID owner, VehicleType type, uint extra);

/**
 * Eine Linie auf die Zielauslastung bringen.
 * @return Text fuer die Statuszeile.
 */
static std::string LmApply(const LineInfo &line)
{
	const Vehicle *v = Vehicle::GetIfValid(line.first);
	if (v == nullptr) return GetString(STR_LINEMGR_STATUS_GONE);
	if (_networking) return GetString(STR_LINEMGR_ERR_SINGLEPLAYER);
	if (line.advice == 0) return GetString(STR_LINEMGR_STATUS_NOTHING);

	if (line.advice > 0) {
		TileIndex depot = FleetFindDepotFor(v);
		if (depot == INVALID_TILE) return GetString(STR_LINEMGR_ERR_NO_DEPOT);
		FleetEnsureLimitFor(v->owner, v->type, line.advice);
		/* Auftraege nur teilen, wenn die Linie schon geteilt faehrt -
		 * sonst bekaeme ein Einzelgaenger ploetzlich Mitfahrer. */
		bool share = v->orders != nullptr && v->orders->GetNumVehicles() > 1;
		Backup<CompanyID> cur_company(_current_company, v->owner);
		Money spent = 0;
		int built = 0;
		for (int i = 0; i < line.advice; i++) {
			auto [cost, new_id] = Command<Commands::CloneVehicle>::Do(DoCommandFlag::Execute, depot, line.first, share);
			if (cost.Failed()) break;
			spent += cost.GetCost();
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, new_id, false);
			built++;
		}
		cur_company.Restore();
		if (built == 0) return GetString(STR_LINEMGR_ERR_BUILD);
		if (share) FleetSpreadLine(v);
		return GetString(STR_LINEMGR_STATUS_BUILT, (uint)built, spent);
	}

	/* Abbauen: die unrentabelsten Fahrzeuge zuerst. */
	std::vector<const Vehicle *> fleet;
	for (VehicleID id : line.members) {
		const Vehicle *w = Vehicle::GetIfValid(id);
		if (w != nullptr) fleet.push_back(w);
	}
	if (fleet.size() < 2) return GetString(STR_LINEMGR_ERR_SELL);
	std::sort(fleet.begin(), fleet.end(), [](const Vehicle *a, const Vehicle *b) {
		return a->GetDisplayProfitThisYear() < b->GetDisplayProfitThisYear();
	});

	uint want = std::min<uint>(fleet.size() - 1, (uint)(-line.advice));
	uint sent = 0;
	Backup<CompanyID> cur_company(_current_company, v->owner);
	for (uint i = 0; i < fleet.size() && sent < want; i++) {
		const Vehicle *w = fleet[i];
		bool known = false;
		for (const LmPendingSell &p : _lm_pending_sells) known |= (p.vehicle == w->index);
		if (known) continue;
		if (!w->IsChainInDepot()) {
			/* Mit Halt ins Depot - der Timer verkauft es dort. */
			if (Command<Commands::SendVehicleToDepot>::Do(DoCommandFlag::Execute, w->index,
					DepotCommandFlags{}, {}).Failed()) continue;
		}
		_lm_pending_sells.push_back({w->index, w->owner, 365});
		sent++;
	}
	cur_company.Restore();
	if (sent == 0) return GetString(STR_LINEMGR_ERR_SELL);
	return GetString(STR_LINEMGR_STATUS_SELLING, sent);
}

/* ==================== Das Fenster ==================== */

static bool _lm_only_todo = false; ///< Nur Linien mit Handlungsbedarf zeigen.

struct LineManagerWindow : Window {
	Scrollbar *vscroll = nullptr;
	uint line_height = 0;
	std::string status;

	LineManagerWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_LM_SCROLLBAR);
		this->FinishInitNested(number);
		this->status = GetString(STR_LINEMGR_STATUS_HINT);
		this->Rebuild();
	}

	/** Nur die Linien, die der Filter durchlaesst. */
	std::vector<const LineInfo *> Shown() const
	{
		std::vector<const LineInfo *> out;
		for (const LineInfo &l : _lm_lines) {
			if (_lm_only_todo && l.advice == 0) continue;
			out.push_back(&l);
		}
		return out;
	}

	void Rebuild()
	{
		LmCollect();
		this->vscroll->SetCount((uint)this->Shown().size());
		this->SetDirty();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget != WID_LM_SUMMARY) return this->Window::GetWidgetString(widget, stringid);
		Money profit = 0;
		uint vehicles = 0, todo = 0;
		for (const LineInfo &l : _lm_lines) {
			profit += l.profit;
			vehicles += l.vehicles;
			if (l.advice != 0) todo++;
		}
		return GetString(STR_LINEMGR_SUMMARY, (uint)_lm_lines.size(), vehicles, profit, todo);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_LM_LIST:
				this->line_height = GetCharacterHeight(FontSize::Normal) + ScaleGUITrad(4);
				resize.height = this->line_height;
				size.height = 10 * this->line_height;
				size.width = std::max<uint>(size.width, ScaleGUITrad(560));
				break;

			case WID_LM_HEADER:
				size.height = GetCharacterHeight(FontSize::Normal) + ScaleGUITrad(3);
				break;
		}
	}

	/** Spaltenraster: Linie | Fz | Auslastung | Gewinn | wartend | Vorschlag. */
	void Columns(const Rect &r, int &c_veh, int &c_occ, int &c_profit, int &c_wait, int &c_advice) const
	{
		int w = r.Width();
		c_advice = r.right - ScaleGUITrad(74);
		c_wait = c_advice - ScaleGUITrad(72);
		c_profit = c_wait - ScaleGUITrad(104);
		c_occ = c_profit - ScaleGUITrad(60);
		c_veh = c_occ - ScaleGUITrad(34);
		(void)w;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_LM_HEADER) {
			Rect hr = r.Shrink(WidgetDimensions::scaled.framerect.left, 0, WidgetDimensions::scaled.framerect.right, 0);
			int c_veh, c_occ, c_profit, c_wait, c_advice;
			this->Columns(hr, c_veh, c_occ, c_profit, c_wait, c_advice);
			DrawString(hr.left, c_veh - 2, hr.top, GetString(STR_LINEMGR_COL_LINE), TextColour::White);
			DrawString(c_veh, c_occ - 2, hr.top, GetString(STR_LINEMGR_COL_VEHICLES), TextColour::White, AlignmentH::End);
			DrawString(c_occ, c_profit - 2, hr.top, GetString(STR_LINEMGR_COL_OCCUPANCY), TextColour::White, AlignmentH::End);
			DrawString(c_profit, c_wait - 2, hr.top, GetString(STR_LINEMGR_COL_PROFIT), TextColour::White, AlignmentH::End);
			DrawString(c_wait, c_advice - 2, hr.top, GetString(STR_LINEMGR_COL_WAITING), TextColour::White, AlignmentH::End);
			DrawString(c_advice, hr.right, hr.top, GetString(STR_LINEMGR_COL_ADVICE), TextColour::White, AlignmentH::End);
			return;
		}
		if (widget != WID_LM_LIST) return;

		std::vector<const LineInfo *> shown = this->Shown();
		Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
		int c_veh, c_occ, c_profit, c_wait, c_advice;
		this->Columns(ir, c_veh, c_occ, c_profit, c_wait, c_advice);

		if (shown.empty()) {
			DrawString(ir, GetString(_lm_only_todo ? STR_LINEMGR_ALL_FINE : STR_LINEMGR_EMPTY),
					TextColour::White, AlignmentH::Centre);
			return;
		}

		uint first = this->vscroll->GetPosition();
		uint last = std::min<uint>((uint)shown.size(), first + this->vscroll->GetCapacity());
		int y = ir.top;
		for (uint i = first; i < last; i++) {
			const LineInfo &l = *shown[i];
			int text_y = y + ScaleGUITrad(2);

			DrawString(ir.left, c_veh - 2, text_y, l.name, TextColour::Black);
			DrawString(c_veh, c_occ - 2, text_y, GetString(STR_JUST_INT, l.vehicles), TextColour::Black, AlignmentH::End);

			/* Auslastung faerben: rot = Fahrzeuge fahren leer herum. */
			TextColour occ_col = TextColour::Black;
			if (l.has_occupancy) occ_col = l.occupancy < 35 ? TextColour::Red : (l.occupancy > 85 ? TextColour::Orange : TextColour::Green);
			DrawString(c_occ, c_profit - 2, text_y,
					l.has_occupancy ? GetString(STR_LINEMGR_PERCENT, l.occupancy) : GetString(STR_LINEMGR_NO_DATA),
					occ_col, AlignmentH::End);

			DrawString(c_profit, c_wait - 2, text_y, GetString(STR_JUST_CURRENCY_LONG, l.profit),
					l.profit < 0 ? TextColour::Red : TextColour::Black, AlignmentH::End);
			DrawString(c_wait, c_advice - 2, text_y, GetString(STR_JUST_INT, l.waiting),
					l.waiting > l.capacity ? TextColour::Orange : TextColour::Black, AlignmentH::End);

			if (l.advice > 0) {
				DrawString(c_advice, ir.right, text_y, GetString(STR_LINEMGR_ADVICE_MORE, l.advice), TextColour::Orange, AlignmentH::End);
			} else if (l.advice < 0) {
				DrawString(c_advice, ir.right, text_y, GetString(STR_LINEMGR_ADVICE_LESS, -l.advice), TextColour::Red, AlignmentH::End);
			} else {
				DrawString(c_advice, ir.right, text_y, GetString(STR_LINEMGR_ADVICE_OK), TextColour::Green, AlignmentH::End);
			}
			y += this->line_height;
		}
	}

	void DrawWidgetStatus(const Rect &r) const
	{
		DrawStringMultiLine(r, this->status, TextColour::Black);
	}

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_LM_FILTER_ALL, !_lm_only_todo);
		this->SetWidgetLoweredState(WID_LM_FILTER_TODO, _lm_only_todo);
		this->DrawWidgets();
	}

	/** Zeile unter dem Mauszeiger; nullptr wenn daneben. */
	const LineInfo *LineAt(Point pt) const
	{
		int row = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_LM_LIST, WidgetDimensions::scaled.framerect.top);
		if (row == INT_MAX) return nullptr;
		std::vector<const LineInfo *> shown = this->Shown();
		if (row < 0 || row >= (int)shown.size()) return nullptr;
		return shown[row];
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_LM_FILTER_ALL:
				_lm_only_todo = false;
				this->Rebuild();
				break;

			case WID_LM_FILTER_TODO:
				_lm_only_todo = true;
				this->Rebuild();
				break;

			case WID_LM_APPLY_ALL: {
				uint done = 0;
				/* Auf einer Kopie arbeiten: LmApply baut und verkauft,
				 * danach stimmen die gesammelten Daten nicht mehr. */
				std::vector<LineInfo> copy = _lm_lines;
				for (const LineInfo &l : copy) {
					if (l.advice == 0) continue;
					LmApply(l);
					done++;
				}
				this->status = done == 0 ? GetString(STR_LINEMGR_STATUS_NOTHING) : GetString(STR_LINEMGR_STATUS_ALL, done);
				this->Rebuild();
				break;
			}

			case WID_LM_LIST: {
				const LineInfo *l = this->LineAt(pt);
				if (l == nullptr) break;
				int c_veh, c_occ, c_profit, c_wait, c_advice;
				Rect ir = this->GetWidget<NWidgetBase>(WID_LM_LIST)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
				this->Columns(ir, c_veh, c_occ, c_profit, c_wait, c_advice);
				if (pt.x >= c_advice && l->advice != 0) {
					/* Klick auf den Vorschlag setzt ihn um. */
					LineInfo copy = *l;
					this->status = LmApply(copy);
					this->Rebuild();
				} else if (l->tile != INVALID_TILE) {
					ScrollMainWindowToTile(l->tile);
				}
				break;
			}
		}
	}

	/** Rechtsklick oeffnet die Fahrzeugliste der Linie. */
	bool OnRightClick([[maybe_unused]] Point pt, WidgetID widget) override
	{
		if (widget != WID_LM_LIST) return false;
		const LineInfo *l = this->LineAt(pt);
		if (l != nullptr) {
			const Vehicle *v = Vehicle::GetIfValid(l->first);
			if (v != nullptr) ShowVehicleListWindow(v);
		}
		return true;
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_LM_LIST, WidgetDimensions::scaled.framerect.Vertical());
	}

	/** Zahlen leben - alle paar Sekunden nachrechnen. */
	const IntervalTimer<TimerWindow> refresh = {std::chrono::seconds(4), [this](auto) {
		this->Rebuild();
	}};
};

/** Statuszeile als eigenes Widget zeichnen. */
struct LineManagerStatus : Window {
	LineManagerStatus(WindowDesc &desc) : Window(desc) {}
};

static constexpr std::initializer_list<NWidgetPart> _nested_linemgr_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_LM_CAPTION), SetStringTip(STR_LINEMGR_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_LM_FILTER_ALL), SetFill(1, 0), SetMinimalSize(90, 14), SetStringTip(STR_LINEMGR_FILTER_ALL, STR_LINEMGR_FILTER_ALL_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_LM_FILTER_TODO), SetFill(1, 0), SetMinimalSize(140, 14), SetStringTip(STR_LINEMGR_FILTER_TODO, STR_LINEMGR_FILTER_TODO_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Green, WID_LM_APPLY_ALL), SetFill(1, 0), SetMinimalSize(150, 14), SetStringTip(STR_LINEMGR_APPLY_ALL, STR_LINEMGR_APPLY_ALL_TOOLTIP),
			EndContainer(),
			NWidget(WWT_EMPTY, Colours::Invalid, WID_LM_HEADER), SetFill(1, 0),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PANEL, Colours::DarkGreen, WID_LM_LIST), SetFill(1, 1), SetResize(1, 1), SetScrollbar(WID_LM_SCROLLBAR), EndContainer(),
				NWidget(NWID_VSCROLLBAR, Colours::DarkGreen, WID_LM_SCROLLBAR),
			EndContainer(),
			NWidget(WWT_TEXT, Colours::Invalid, WID_LM_SUMMARY), SetFill(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_SPACER), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, Colours::DarkGreen),
	EndContainer(),
};

static WindowDesc _linemgr_desc(
	WindowPosition::Automatic, "linemgr", 620, 340,
	WindowClass::LineManager, WindowClass::None,
	{},
	_nested_linemgr_widgets
);

/** Fork: Linien-Manager oeffnen. */
void ShowLineManagerWindow()
{
	AllocateWindowDescFront<LineManagerWindow>(_linemgr_desc, 0);
}

/** Fork: Diagnose - Linien und Vorschlaege als Text. */
std::string LineManagerDebug(bool apply)
{
	LmCollect();
	std::string out = fmt::format("Linien: {}", _lm_lines.size());
	uint shown = 0;
	for (const LineInfo &l : _lm_lines) {
		if (shown++ >= 8) break;
		out += fmt::format("\n  {} | {} Fz | {}% | Gewinn {} | wartend {} | Vorschlag {:+d}",
				l.name, l.vehicles, l.has_occupancy ? l.occupancy : -1,
				(int64_t)l.profit, l.waiting, l.advice);
	}
	if (apply) {
		std::vector<LineInfo> copy = _lm_lines;
		uint done = 0;
		for (const LineInfo &l : copy) {
			if (l.advice == 0) continue;
			out += "\n  -> " + LmApply(l);
			done++;
		}
		out += fmt::format("\n  {} Linien angepasst.", done);
	}
	Debug(misc, 0, "Linien-Diagnose: {}", out);
	return out;
}
