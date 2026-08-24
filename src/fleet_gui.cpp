/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file fleet_gui.cpp Flotte aufstocken (Fork-Feature).
 *
 * Ein Fahrzeug zu kopieren kann OpenTTD laengst - aber immer nur eins,
 * und die Kopien fahren dann als Pulk hintereinander her. Dieses Fenster
 * macht beides besser: es baut mehrere Kopien auf einmal und sorgt
 * dafuer, dass sie sich gleichmaessig ueber die Strecke verteilen.
 *
 * Die Taktung uebernimmt das Spiel selbst. Der Fahrplan wird auf
 * "automatisch messen" gestellt; sobald eine Runde gefahren ist, kennt
 * das Spiel die Rundenzeit und verteilt alle Fahrzeuge der Linie
 * gleichmaessig darauf (derselbe Rechenweg wie beim Knopf "Fahrplan
 * gleichmaessig verteilen" im Fahrplanfenster).
 */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "vehicle_cmd.h"
#include "engine_base.h"
#include "engine_func.h"
#include "autoreplace_func.h"
#include "autoreplace_cmd.h"
#include "group.h"
#include "misc_cmd.h"
#include "dropdown_func.h"
#include "dropdown_type.h"
#include "timetable_cmd.h"
#include "order_base.h"
#include "depot_base.h"
#include "depot_map.h"
#include "station_base.h"
#include "company_base.h"
#include "company_func.h"
#include "company_gui.h"
#include "misc_cmd.h"
#include "openttd.h"
#include "command_func.h"
#include "signal_func.h"
#include "core/backup_type.hpp"
#include "error.h"
#include "debug.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_tick.h"
#include "timer/timer_window.h"
#include "widgets/fleet_widget.h"
#include "table/strings.h"

#include "safeguards.h"

/* ==================== Automatische Taktung ==================== */

/**
 * Eine Linie, deren Fahrplan gerade gemessen wird. Sobald er vollstaendig
 * ist, werden die Fahrzeuge gleichmaessig darauf verteilt.
 */
struct FleetPendingSpread {
	VehicleID vehicle; ///< Ein Fahrzeug der Linie (stellvertretend).
	int days_left;     ///< Aufgeben, wenn die Runde nie fertig wird.
};
static std::vector<FleetPendingSpread> _fleet_pending_spreads;

/**
 * Ein Fahrzeug, das auf seinen Modell-Tausch wartet. Der Tausch passiert
 * nicht ueber den Service-Besuch: dort verlangt OpenTTD eine Geldreserve
 * (Einstellung engine_renew_money, Standard 100.000), an der der Tausch
 * nach einem teuren Streckenbau leise scheitert. Stattdessen halten die
 * Fahrzeuge im Depot, der Tages-Timer tauscht sie dort direkt und
 * schickt die Nachfolger sofort wieder los.
 */
struct FleetPendingReplace {
	VehicleID vehicle; ///< Das alte Fahrzeug.
	EngineID from;     ///< Sein Modell (Tausch-Erkennung).
	EngineID to;       ///< Das Wunschmodell (fuer den Start des Nachfolgers).
	CompanyID owner;
	int days_left;     ///< Frist, danach wird aufgegeben.
};
static std::vector<FleetPendingReplace> _fleet_pending_replaces;

/** Genug Geld fuer den Tausch beschaffen - notfalls per Kredit. */
static bool FleetEnsureFunds(CompanyID owner, Money needed)
{
	const Company *c = Company::Get(owner);
	if (c->money >= needed) return true;
	Money avail = c->money + (c->GetMaxLoan() - c->current_loan);
	if (avail < needed) return false;
	Command<Commands::IncreaseLoan>::Do(DoCommandFlag::Execute, LoanCommand::Amount, needed - c->money);
	return Company::Get(owner)->money >= needed;
}

/**
 * Ein im Depot stehendes Fahrzeug jetzt tauschen und den Nachfolger
 * losschicken. Laeuft unter der Firma des Besitzers.
 * @return true bei Erfolg.
 */
static bool FleetSwapInDepot(const Vehicle *v, EngineID to)
{
	if (v == nullptr || !v->IsChainInDepot()) return false;
	/* Autoreplace erwartet ein haltendes Fahrzeug - ein Zug, der nur zum
	 * Service durchrollt, wuerde die Invarianten des Tauschs verletzen. */
	if (!v->vehstatus.Test(VehState::Stopped)) {
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, v->index, false);
		if (!v->vehstatus.Test(VehState::Stopped)) return false;
	}
	FleetEnsureFunds(v->owner, Engine::Get(to)->GetCost() + Engine::Get(to)->GetCost() / 5);
	TileIndex depot_tile = v->tile;
	CompanyID owner = v->owner;
	CommandCost swap = Command<Commands::AutoreplaceVehicle>::Do(DoCommandFlag::Execute, v->index);
	/* Direktes Do umgeht den Kommando-Wrapper, der den Signal-Puffer
	 * leert - hier nachholen, sonst stolpert der naechste Zug-Tick. */
	UpdateSignalsInBuffer();
	if (swap.Failed()) {
		Debug(misc, 0, "Flotte: Autoreplace-Fehler {}", swap.GetErrorMessage().base());
		if (swap.GetErrorMessage() == STR_ERROR_AUTOREPLACE_NOTHING_TO_DO) {
			/* Nichts zu tauschen (z. B. fabrikneu und noch jung): das
			 * Fahrzeug wieder losschicken statt es im Depot zu parken. */
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, v->index, false);
		}
		return false;
	}
	/* Das neue Fahrzeug hat eine neue Nummer - auf dem Depotfeld suchen
	 * und starten, damit es nicht vergessen im Depot steht. */
	for (const Vehicle *w : Vehicle::Iterate()) {
		if (w->tile != depot_tile || w->owner != owner) continue;
		if (!w->IsPrimaryVehicle() || w->engine_type != to) continue;
		if (!w->IsChainInDepot() || !w->vehstatus.Test(VehState::Stopped)) continue;
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, w->index, false);
	}
	return true;
}

/**
 * Diagnose "fleet fulltest": baut nach Firmengruendung eine Zuglinie und
 * stoesst dann den Fabrikneu-Tausch an - der komplette Ersetzen-Pfad
 * ohne einen einzigen Klick, damit Abstuerze headless reproduzierbar sind.
 */
int _fleet_fulltest_stage = 0; ///< 0 = aus, 1 = Linie bauen, 2 = Tausch anstossen.

/** Taeglich: fertig gemessene Fahrplaene gleichmaessig verteilen. */
static const IntervalTimer<TimerGameCalendar> _fleet_timer = {{TimerGameCalendar::Trigger::Day, TimerGameCalendar::Priority::None}, [](auto) {
	if (_fleet_fulltest_stage > 0) {
		/* Headless-Start (null-Video): dort gibt es keine Spielerfirma -
		 * fuer den Test eine gruenden und NUR fuer die Dauer des Zweigs
		 * uebernehmen (StateGameLoop stellt die Firma am Tick-Ende selbst
		 * wieder her und prueft die Invariante). */
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		const Company *c = nullptr;
		for (const Company *i : Company::Iterate()) { c = i; break; }
		if (c == nullptr) c = DoStartupNewCompany(false, CompanyID::Invalid());
		if (_pause_mode.Any()) Command<Commands::Pause>::Do(DoCommandFlag::Execute, PauseMode::Normal, false);
		if (c != nullptr) {
			Backup<CompanyID> local(_local_company, c->index);
			if (_fleet_fulltest_stage == 1) {
				extern std::string AutoConnectDebugBuild(std::string_view mode, uint a_idx, uint b_idx, uint count, bool auto_pick);
				Debug(misc, 0, "Fulltest Bau: {}", AutoConnectDebugBuild("rail", 0, 1, 1, true));
				_fleet_fulltest_stage = 2;
			} else {
				extern std::string FleetDebugSwapTest();
				Debug(misc, 0, "Fulltest Swap: {}", FleetDebugSwapTest());
				_fleet_fulltest_stage = 0;
			}
			local.Restore();
		}
	}

	for (auto it = _fleet_pending_spreads.begin(); it != _fleet_pending_spreads.end();) {
		const Vehicle *v = Vehicle::GetIfValid(it->vehicle);
		if (v == nullptr || v->orders == nullptr || --it->days_left <= 0) {
			it = _fleet_pending_spreads.erase(it);
			continue;
		}
		if (!v->orders->IsCompleteTimetable()) {
			++it;
			continue;
		}
		/* Die Runde ist gemessen - jetzt gleichmaessig verteilen. */
		Backup<CompanyID> cur_company(_current_company, v->owner);
		CommandCost res = Command<Commands::SetTimetableStart>::Do(DoCommandFlag::Execute, it->vehicle, true,
				TimerGameTick::counter + Ticks::DAY_TICKS);
		cur_company.Restore();
		Debug(misc, 0, "Flotte: Taktung fuer Linie {} {} (Dauer {} Ticks)", v->index.base(),
				res.Succeeded() ? "gesetzt" : "abgelehnt", v->orders->GetTimetableDurationIncomplete());
		if (res.Failed()) {
			/* Nicht aufgeben - vielleicht klappt es beim naechsten Versuch. */
			++it;
			continue;
		}
		it = _fleet_pending_spreads.erase(it);
	}

	for (auto it = _fleet_pending_replaces.begin(); it != _fleet_pending_replaces.end();) {
		const Vehicle *v = Vehicle::GetIfValid(it->vehicle);
		/* Weg oder anderes Modell: der Tausch ist durch. Frist um: aufgeben -
		 * aber ein im Depot abgestelltes Fahrzeug vorher wieder losschicken. */
		if (v == nullptr || v->engine_type != it->from || v->owner != it->owner || --it->days_left <= 0) {
			if (v != nullptr && v->engine_type == it->from && v->IsChainInDepot() && v->vehstatus.Test(VehState::Stopped)) {
				Backup<CompanyID> cur_company(_current_company, v->owner);
				Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, v->index, false);
				cur_company.Restore();
			}
			it = _fleet_pending_replaces.erase(it);
			continue;
		}
		if (!v->IsChainInDepot()) {
			/* Noch unterwegs. Hat das Fahrzeug seine Depot-Order verloren
			 * (z. B. durch einen Spieler-Klick), erneut schicken. */
			if (!v->current_order.IsType(OT_GOTO_DEPOT)) {
				Backup<CompanyID> cur_company(_current_company, it->owner);
				Command<Commands::SendVehicleToDepot>::Do(DoCommandFlag::Execute, it->vehicle, DepotCommandFlags{}, {});
				cur_company.Restore();
			}
			++it;
			continue;
		}
		Backup<CompanyID> cur_company(_current_company, it->owner);
		bool done = FleetSwapInDepot(v, it->to);
		cur_company.Restore();
		/* Faehrt das Fahrzeug wieder (Tausch unnoetig), ist der Eintrag
		 * ebenfalls erledigt - sonst stuende es morgen wieder im Depot. */
		bool moot = !done && !v->vehstatus.Test(VehState::Stopped);
		Debug(misc, 0, "Flotte: Tausch von Fahrzeug {} {}", it->vehicle.base(),
				done ? "erledigt" : (moot ? "unnoetig (faehrt weiter)" : "fehlgeschlagen, neuer Versuch morgen"));
		if (done || moot) {
			it = _fleet_pending_replaces.erase(it);
		} else {
			++it;
		}
	}
}};

/**
 * Taktung anstossen: ist die Rundenzeit bekannt, sofort verteilen -
 * sonst den Fahrplan messen lassen und spaeter verteilen.
 * @return true, wenn schon jetzt verteilt wurde.
 */
static bool FleetStartSpreading(const Vehicle *v)
{
	if (v == nullptr || v->orders == nullptr) return false;
	Backup<CompanyID> cur_company(_current_company, v->owner);
	bool now = false;
	if (v->orders->IsCompleteTimetable()) {
		now = Command<Commands::SetTimetableStart>::Do(DoCommandFlag::Execute, v->index, true,
				TimerGameTick::counter + Ticks::DAY_TICKS).Succeeded();
	}
	if (!now) {
		for (const Vehicle *w = v->orders->GetFirstSharedVehicle(); w != nullptr; w = w->NextShared()) {
			Command<Commands::AutofillTimetable>::Do(DoCommandFlag::Execute, w->index, true, false);
		}
		bool known = false;
		for (const FleetPendingSpread &p : _fleet_pending_spreads) known |= (p.vehicle == v->index);
		if (!known) _fleet_pending_spreads.push_back({v->index, 365 * 3});
	}
	cur_company.Restore();
	return now;
}

/**
 * Fork: Fahrzeug fuer den Modell-Tausch vormerken. Der Tages-Timer ruft
 * es ins Depot und tauscht dort - bewusst NICHT sofort im Klick: der
 * Verkauf schliesst das Fahrzeugfenster, und wer mitten im eigenen
 * Klick-Handler geloescht wird, reisst das Spiel mit ab.
 */
void FleetQueueReplace(VehicleID veh, EngineID to)
{
	const Vehicle *v = Vehicle::GetIfValid(veh);
	if (v == nullptr || !Engine::IsValidID(to)) return;
	/* Fabrikneu-Tausch (gleiches Modell) macht das Spiel nur bei alten
	 * Fahrzeugen - ein junges braeuchte den Depotbesuch umsonst. */
	if (to == v->engine_type && !v->NeedsAutorenewing(Company::Get(v->owner), false)) return;
	for (const FleetPendingReplace &p : _fleet_pending_replaces) {
		if (p.vehicle == veh) return;
	}
	Backup<CompanyID> cur_company(_current_company, v->owner);
	if (!v->IsChainInDepot() && !v->current_order.IsType(OT_GOTO_DEPOT)) {
		Command<Commands::SendVehicleToDepot>::Do(DoCommandFlag::Execute, veh, DepotCommandFlags{}, {});
	}
	cur_company.Restore();
	_fleet_pending_replaces.push_back({veh, v->engine_type, to, v->owner, 365});
}

/* ==================== Hilfsfunktionen ==================== */

/** Wieviele Fahrzeuge teilen sich die Auftraege dieses Fahrzeugs? */
static uint FleetSharedCount(const Vehicle *v)
{
	if (v->orders == nullptr) return 1;
	uint n = 0;
	for (const Vehicle *w = v->orders->GetFirstSharedVehicle(); w != nullptr; w = w->NextShared()) n++;
	return std::max<uint>(1, n);
}

/** Passendes Depot (bzw. Hangar) zum Klonen finden. */
static TileIndex FleetFindDepot(const Vehicle *v)
{
	/* Steht das Fahrzeug schon im Depot, ist die Sache einfach. */
	if (v->IsInDepot()) return v->tile;

	if (v->type == VehicleType::Aircraft) {
		for (const Order &o : v->Orders()) {
			if (!o.IsType(OT_GOTO_STATION)) continue;
			const Station *st = Station::GetIfValid(o.GetDestination().ToStationID());
			if (st != nullptr && st->facilities.Test(StationFacility::Airport) && st->airport.HasHangar()) {
				return st->airport.GetHangarTile(0);
			}
		}
		return INVALID_TILE;
	}

	TileIndex best = INVALID_TILE;
	uint best_dist = UINT32_MAX;
	for (const Depot *d : Depot::Iterate()) {
		if (d->xy == INVALID_TILE || GetTileOwner(d->xy) != v->owner) continue;
		bool fits = (v->type == VehicleType::Train && IsRailDepotTile(d->xy)) ||
				(v->type == VehicleType::Road && IsRoadDepotTile(d->xy)) ||
				(v->type == VehicleType::Ship && IsShipDepotTile(d->xy));
		if (!fits) continue;
		uint dist = DistanceManhattan(d->xy, v->tile);
		if (dist < best_dist) {
			best_dist = dist;
			best = d->xy;
		}
	}
	return best;
}

/* ==================== Das Fenster ==================== */

static bool _fleet_share = true;     ///< Kopien fahren dieselben Auftraege.
static bool _fleet_timetable = true; ///< Kopien gleichmaessig ueber die Runde verteilen.

/** Alle Firmen-Fahrzeuge mit diesem Modell (nur Zugmaschinen). */
static std::vector<VehicleID> FleetSameEngine(const Vehicle *v)
{
	std::vector<VehicleID> out;
	for (const Vehicle *w : Vehicle::Iterate()) {
		if (!w->IsPrimaryVehicle() || w->owner != v->owner) continue;
		if (w->engine_type != v->engine_type) continue;
		out.push_back(w->index);
	}
	return out;
}

struct FleetWindow : Window {
	VehicleID veh;
	uint count = 3;
	EngineID new_engine = EngineID::Invalid(); ///< Gewaehltes Ersatzmodell.
	std::string status;

	FleetWindow(WindowDesc &desc, WindowNumber number) : Window(desc), veh(static_cast<VehicleID>(number))
	{
		this->InitNested(number);
		this->status = GetString(STR_FLEET_STATUS_HINT);
	}

	const Vehicle *Veh() const { return Vehicle::GetIfValid(this->veh); }

	/** Was kosten die Kopien? Trockenlauf des Klon-Befehls. */
	Money EstimateCost() const
	{
		const Vehicle *v = this->Veh();
		if (v == nullptr) return 0;
		TileIndex depot = FleetFindDepot(v);
		if (depot == INVALID_TILE) return 0;
		Backup<CompanyID> cur_company(_current_company, v->owner);
		auto [cost, id] = Command<Commands::CloneVehicle>::Do(DoCommandFlags{}, depot, v->index, _fleet_share);
		cur_company.Restore();
		if (cost.Failed()) return 0;
		return cost.GetCost() * this->count;
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_FL_VEHICLE: {
				const Vehicle *v = this->Veh();
				if (v == nullptr) return GetString(STR_FLEET_VEHICLE_GONE);
				return GetString(STR_FLEET_VEHICLE, v->index, FleetSharedCount(v));
			}
			case WID_FL_COUNT:
				return GetString(STR_FLEET_COUNT, this->count);
			case WID_FL_COST: {
				Money cost = this->EstimateCost();
				return cost == 0 ? GetString(STR_FLEET_COST_UNKNOWN) : GetString(STR_FLEET_COST, cost);
			}
			case WID_FL_MODEL:
				if (this->new_engine == EngineID::Invalid()) return GetString(STR_FLEET_MODEL_PICK);
				return GetString(STR_ENGINE_NAME, this->new_engine);
			case WID_FL_REPLACE: {
				const Vehicle *v = this->Veh();
				return GetString(STR_FLEET_REPLACE, v == nullptr ? 0 : (uint)FleetSameEngine(v).size());
			}
			case WID_FL_STATUS:
				return this->status;
			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_FL_SHARE, _fleet_share);
		this->SetWidgetLoweredState(WID_FL_TIMETABLE, _fleet_timetable && _fleet_share);
		this->SetWidgetDisabledState(WID_FL_TIMETABLE, !_fleet_share);
		this->DrawWidgets();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_FL_STATUS) return;
		DrawStringMultiLine(r, this->status, TextColour::Black);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_FL_COUNT_DOWN:
				if (this->count > 1) this->count--;
				this->SetDirty();
				break;

			case WID_FL_COUNT_UP:
				if (this->count < 20) this->count++;
				this->SetDirty();
				break;

			case WID_FL_SHARE:
				_fleet_share = !_fleet_share;
				this->SetDirty();
				break;

			case WID_FL_TIMETABLE:
				_fleet_timetable = !_fleet_timetable;
				this->SetDirty();
				break;

			case WID_FL_BUILD:
				this->BuildCopies();
				break;

			case WID_FL_MODEL: {
				const Vehicle *v = this->Veh();
				if (v == nullptr) break;
				/* Alle baubaren Modelle desselben Typs, auf die das
				 * Autoreplace tauschen darf - beste zuerst. */
				std::vector<const Engine *> engines;
				for (const Engine *e : Engine::IterateType(v->type)) {
					if (e->index == v->engine_type) continue;
					if (!e->company_avail.Test(v->owner)) continue;
					if (!CheckAutoreplaceValidity(v->engine_type, e->index, v->owner)) continue;
					engines.push_back(e);
				}
				std::sort(engines.begin(), engines.end(), [](const Engine *a, const Engine *b) {
					return a->GetDisplayMaxSpeed() + a->GetDisplayDefaultCapacity() * 3
							> b->GetDisplayMaxSpeed() + b->GetDisplayDefaultCapacity() * 3;
				});
				DropDownList list;
				for (const Engine *e : engines) {
					list.push_back(MakeDropDownListStringItem(GetString(STR_FLEET_MODEL_ITEM, e->index,
							e->GetDisplayDefaultCapacity(), e->GetDisplayMaxSpeed()), e->index.base()));
				}
				if (list.empty()) {
					this->status = GetString(STR_FLEET_ERR_NO_MODEL);
					this->SetDirty();
					break;
				}
				ShowDropDownList(this, std::move(list),
						this->new_engine == EngineID::Invalid() ? -1 : (int)this->new_engine.base(), WID_FL_MODEL);
				break;
			}

			case WID_FL_REPLACE:
				this->ReplaceAll();
				break;

			case WID_FL_SPREAD: {
				/* Auch ohne neue Kopien: Linie jetzt gleichmaessig takten. */
				const Vehicle *v = this->Veh();
				if (v == nullptr || v->orders == nullptr || FleetSharedCount(v) < 2) {
					this->status = GetString(STR_FLEET_ERR_NO_LINE);
				} else {
					this->status = GetString(FleetStartSpreading(v) ? STR_FLEET_SPREAD_NOW : STR_FLEET_SPREAD_LATER);
				}
				this->SetDirty();
				break;
			}
		}
	}

	void OnDropdownSelect(WidgetID widget, int index, [[maybe_unused]] int click_result) override
	{
		if (widget != WID_FL_MODEL) return;
		this->new_engine = static_cast<EngineID>(index);
		this->SetDirty();
	}

	/**
	 * Alle baugleichen Fahrzeuge auf das gewaehlte Modell umruesten.
	 *
	 * Der Tausch selbst laeuft ueber das eingebaute Autoreplace: die
	 * Fahrzeuge werden zum Service ins Depot gerufen, dort verkauft und
	 * durch das neue Modell ersetzt - Auftraege, geteilte Fahrplaene und
	 * Taktung bleiben dabei vollstaendig erhalten.
	 */
	void ReplaceAll()
	{
		const Vehicle *v = this->Veh();
		if (v == nullptr) {
			this->status = GetString(STR_FLEET_VEHICLE_GONE);
			this->SetDirty();
			return;
		}
		if (_networking) {
			ShowErrorMessage(GetEncodedString(STR_FLEET_ERR_SINGLEPLAYER), {}, WarningLevel::Info);
			return;
		}
		if (this->new_engine == EngineID::Invalid()) {
			this->status = GetString(STR_FLEET_ERR_PICK_MODEL);
			this->SetDirty();
			return;
		}
		if (!CheckAutoreplaceValidity(v->engine_type, this->new_engine, v->owner)) {
			this->status = GetString(STR_FLEET_ERR_NO_MODEL);
			this->SetDirty();
			return;
		}

		std::vector<VehicleID> same = FleetSameEngine(v);
		EngineID from = v->engine_type;
		Backup<CompanyID> cur_company(_current_company, v->owner);
		CommandCost rule = Command<Commands::SetAutoreplace>::Do(DoCommandFlag::Execute,
				ALL_GROUP, from, this->new_engine, false);
		if (rule.Succeeded()) {
			/* Geld fuer die erste Runde Kaeufe sichern - der Rest folgt
			 * gestaffelt, wenn die Verkaufserloese wieder reinkommen. */
			FleetEnsureFunds(v->owner, Engine::Get(this->new_engine)->GetCost() * (int)std::min<size_t>(same.size(), 4));
		}
		cur_company.Restore();

		if (rule.Failed()) {
			this->status = GetString(STR_FLEET_ERR_NO_MODEL);
			this->SetDirty();
			return;
		}
		/* Der Tausch selbst laeuft NICHT hier im Klick: der Verkauf
		 * wuerde offene Fahrzeugfenster mitten im Ereignis schliessen.
		 * Der Tages-Timer erledigt ihn gefahrlos - auch fuer Fahrzeuge,
		 * die schon im Depot stehen. */
		for (VehicleID id : same) FleetQueueReplace(id, this->new_engine);
		this->status = GetString(STR_FLEET_REPLACE_DONE, (uint)same.size());
		this->SetDirty();
	}

	/** Die eigentliche Arbeit: klonen, staffeln, takten. */
	void BuildCopies()
	{
		const Vehicle *v = this->Veh();
		if (v == nullptr) {
			this->status = GetString(STR_FLEET_VEHICLE_GONE);
			this->SetDirty();
			return;
		}
		if (_networking) {
			ShowErrorMessage(GetEncodedString(STR_FLEET_ERR_SINGLEPLAYER), {}, WarningLevel::Info);
			return;
		}
		TileIndex depot = FleetFindDepot(v);
		if (depot == INVALID_TILE) {
			this->status = GetString(STR_FLEET_ERR_NO_DEPOT);
			this->SetDirty();
			return;
		}

		uint already = FleetSharedCount(v);
		Backup<CompanyID> cur_company(_current_company, v->owner);
		Money spent = 0;
		uint built = 0;
		std::vector<VehicleID> fresh;
		for (uint i = 0; i < this->count; i++) {
			auto [cost, new_id] = Command<Commands::CloneVehicle>::Do(DoCommandFlag::Execute, depot, this->veh, _fleet_share);
			if (cost.Failed()) break;
			spent += cost.GetCost();
			fresh.push_back(new_id);
			built++;
		}

		/* Sofort losschicken - wer im Depot wartet, wird leicht vergessen.
		 * Fuer die gleichmaessige Verteilung sorgt der Fahrplan. */
		for (VehicleID id : fresh) {
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, id, false);
		}
		cur_company.Restore();

		bool spreading = _fleet_share && _fleet_timetable && built > 0 && FleetSharedCount(v) > 1;
		if (spreading) FleetStartSpreading(v);

		if (built == 0) {
			this->status = GetString(STR_FLEET_ERR_FAILED);
		} else if (spreading) {
			this->status = GetString(STR_FLEET_STATUS_DONE_TIMETABLE, built, spent, already + built);
		} else {
			this->status = GetString(STR_FLEET_STATUS_DONE, built, spent);
		}
		this->SetDirty();
	}

	/** Kosten und Fahrzeugzahl aendern sich im Spiel - Anzeige nachziehen. */
	const IntervalTimer<TimerWindow> refresh_interval = {std::chrono::seconds(2), [this](auto) {
		this->SetDirty();
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_fleet_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_FL_CAPTION), SetStringTip(STR_FLEET_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(WWT_TEXT, Colours::Invalid, WID_FL_VEHICLE), SetFill(1, 0), SetMinimalSize(240, 0),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_FL_COUNT_DOWN), SetMinimalSize(20, 14), SetStringTip(STR_AUTOCONNECT_MINUS),
				NWidget(WWT_TEXT, Colours::Invalid, WID_FL_COUNT), SetFill(1, 0), SetAlignment({AlignmentH::Centre, AlignmentV::Middle}),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_FL_COUNT_UP), SetMinimalSize(20, 14), SetStringTip(STR_AUTOCONNECT_PLUS),
			EndContainer(),
			NWidget(WWT_TEXTBTN, Colours::Yellow, WID_FL_SHARE), SetFill(1, 0), SetMinimalSize(240, 14), SetStringTip(STR_FLEET_SHARE, STR_FLEET_SHARE_TOOLTIP),
			NWidget(WWT_TEXTBTN, Colours::Yellow, WID_FL_TIMETABLE), SetFill(1, 0), SetMinimalSize(240, 14), SetStringTip(STR_FLEET_TIMETABLE, STR_FLEET_TIMETABLE_TOOLTIP),
			NWidget(WWT_TEXT, Colours::Invalid, WID_FL_COST), SetFill(1, 0),
			NWidget(WWT_PUSHTXTBTN, Colours::Green, WID_FL_BUILD), SetFill(1, 0), SetMinimalSize(240, 16), SetStringTip(STR_FLEET_BUILD, STR_FLEET_BUILD_TOOLTIP),
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_FL_SPREAD), SetFill(1, 0), SetMinimalSize(240, 14), SetStringTip(STR_FLEET_SPREAD, STR_FLEET_SPREAD_TOOLTIP),
			NWidget(WWT_DROPDOWN, Colours::Yellow, WID_FL_MODEL), SetFill(1, 0), SetMinimalSize(240, 14), SetToolTip(STR_FLEET_MODEL_TOOLTIP),
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_FL_REPLACE), SetFill(1, 0), SetMinimalSize(240, 14), SetToolTip(STR_FLEET_REPLACE_TOOLTIP),
			NWidget(WWT_EMPTY, Colours::Invalid, WID_FL_STATUS), SetFill(1, 0), SetMinimalSize(240, 34),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _fleet_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::Fleet, WindowClass::None,
	{},
	_nested_fleet_widgets
);

/** Fork: Fenster "Flotte aufstocken" fuer ein Fahrzeug oeffnen. */
void ShowFleetWindow(VehicleID veh)
{
	const Vehicle *v = Vehicle::GetIfValid(veh);
	if (v == nullptr || !v->IsPrimaryVehicle()) return;
	CloseWindowByClass(WindowClass::Fleet);
	AllocateWindowDescFront<FleetWindow>(_fleet_desc, veh.base());
}

/** Fork: Diagnose - Fenster fuer das erste eigene Fahrzeug oeffnen. */
std::string FleetDebugOpen()
{
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle() || v->owner != _local_company) continue;
		ShowFleetWindow(v->index);
		return fmt::format("Flotten-Fenster fuer Fahrzeug {} geoeffnet ({} auf der Linie).",
				v->index.base(), FleetSharedCount(v));
	}
	return "Kein eigenes Fahrzeug gefunden.";
}

/**
 * Fork: Diagnose - Fabrikneu-Tausch des ersten eigenen Fahrzeugs anstossen.
 * Reproduziert den kompletten Ersetzen-Pfad (Depot-Ruf + Tausch im Depot)
 * ohne Klicks, damit sich Abstuerze headless nachstellen lassen.
 */
std::string FleetDebugSwapTest()
{
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle() || v->owner != _local_company) continue;
		Backup<CompanyID> cur_company(_current_company, v->owner);
		CommandCost rule = Command<Commands::SetAutoreplace>::Do(DoCommandFlag::Execute, ALL_GROUP, v->engine_type, v->engine_type, false);
		EngineID check = EngineReplacementForCompany(Company::Get(v->owner), v->engine_type, ALL_GROUP);
		Debug(misc, 0, "SwapTest Regel: {} (hinterlegt: {})", rule.Succeeded() ? "ok" : "abgelehnt", check.base());
		CommandCost sent = Command<Commands::SendVehicleToDepot>::Do(DoCommandFlag::Execute, v->index, DepotCommandFlags{}, {});
		cur_company.Restore();
		_fleet_pending_replaces.push_back({v->index, v->engine_type, v->engine_type, v->owner, 365});
		return fmt::format("Swap-Test: Fahrzeug {} (Typ {}) -> Depot: {}", v->index.base(),
				(int)v->type, sent.Succeeded() ? "unterwegs" : "abgelehnt");
	}
	return "Kein eigenes Fahrzeug gefunden.";
}

/** Fork: Diagnose - wie weit ist die automatische Taktung? */
std::string FleetDebugStatus()
{
	std::string out = fmt::format("Offene Taktungen: {} | offene Tausche: {}",
			_fleet_pending_spreads.size(), _fleet_pending_replaces.size());
	for (const FleetPendingSpread &p : _fleet_pending_spreads) {
		const Vehicle *v = Vehicle::GetIfValid(p.vehicle);
		if (v == nullptr || v->orders == nullptr) continue;
		out += fmt::format("\nLinie von Fahrzeug {}: Fahrplan {}, Dauer {} Ticks, {} Fahrzeuge",
				v->index.base(), v->orders->IsCompleteTimetable() ? "vollstaendig" : "wird gemessen",
				v->orders->GetTimetableDurationIncomplete(), FleetSharedCount(v));
	}
	/* Zusaetzlich: wie stehen die Fahrzeuge der ersten eigenen Linie? */
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle() || v->owner != _local_company || v->orders == nullptr) continue;
		out += fmt::format("\nLinie {}: Fahrplan {}, Dauer {} Ticks", v->index.base(),
				v->orders->IsCompleteTimetable() ? "vollstaendig" : "unvollstaendig",
				v->orders->GetTimetableDurationIncomplete());
		const Vehicle *first = v->orders->GetFirstSharedVehicle();
		for (const Vehicle *w = first; w != nullptr; w = w->NextShared()) {
			out += fmt::format("\n  Fahrzeug {}: Modell '{}', {} Auftraege, Auftrag {}, gestartet {}, Verspaetung {}",
					w->index.base(), GetString(STR_ENGINE_NAME, w->engine_type),
					w->GetNumOrders(), w->cur_real_order_index,
					w->vehicle_flags.Test(VehicleFlag::TimetableStarted) ? "ja" : "nein",
					w->lateness_counter);
		}
		break;
	}
	Debug(misc, 0, "Flotten-Diagnose: {}", out);
	return out;
}
