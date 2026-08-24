/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file houseown_gui.cpp Haus-Dialog (Fork-Feature): Klick auf ein Wohnhaus
 * zeigt Bewohner und Eckdaten; das Haus laesst sich kaufen und bringt dann
 * monatlich Miete ein.
 *
 * Besitz wird nicht im Spielstand, sondern je Karten-Seed in houseown.dat
 * im Nutzerverzeichnis abgelegt (gleiche Karte -> gleicher Besitz). Wird
 * das Haus von der Stadt abgerissen, endet der Besitz mit einer Meldung.
 */

#include "stdafx.h"
#include "window_gui.h"
#include "querystring_gui.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "tile_map.h"
#include "town_map.h"
#include "town.h"
#include "house.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "core/backup_type.hpp"
#include "texteff.hpp"
#include "news_func.h"
#include "fileio_func.h"
#include "viewport_func.h"
#include "zoom_func.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "settings_type.h"
#include "error.h"
#include "town_cmd.h"
#include "command_func.h"

#include "widgets/houseown_widget.h"
#include "widgets/housepad_widget.h"

/* town_gui.cpp: Haus samt Mehrfach-Kacheln im GUI zeichnen. */
void DrawHouseInGUI(int x, int y, HouseID house_id, int view);

#include "misc_cmd.h"
#include "core/geometry_func.hpp"
#include "table/strings.h"

#include <fstream>
#include <unordered_set>

#ifdef __EMSCRIPTEN__
#	include <emscripten.h>
#endif

#include "safeguards.h"

/** Ein gekauftes Haus; gilt nur fuer die Karte mit passendem Seed. */
struct OwnedHouse {
	uint32_t seed;      ///< Karten-Seed, zu dem der Besitz gehoert.
	uint32_t tile;      ///< Nordkachel des Hauses.
	uint16_t house_type; ///< HouseID beim Kauf (Abriss-Erkennung).
	int64_t price;      ///< Gezahlter Kaufpreis.
	int64_t rent;       ///< Monatsmiete.
	int64_t warned = 0; ///< Datum der Abriss-Warnung (0 = keine ausstehend).
};

/** Ab diesem Hausalter darf die Stadt ein gekauftes Haus abreissen. */
static constexpr int HOUSEOWN_DEMOLITION_AGE = 75;
/** So viele Tage nach der Warnung bleibt Zeit zum Renovieren. */
static constexpr int64_t HOUSEOWN_WARN_GRACE_DAYS = 365;
static std::vector<OwnedHouse> _owned_houses;
static bool _owned_loaded = false;
static std::unordered_set<uint32_t> _owned_tiles; ///< Nordkacheln der aktuellen Karte (fuer die Fahne).

static uint32_t CurrentSeed()
{
	return _settings_game.game_creation.generation_seed;
}

static void RebuildOwnedTiles()
{
	_owned_tiles.clear();
	for (const OwnedHouse &h : _owned_houses) {
		if (h.seed == CurrentSeed()) _owned_tiles.insert(h.tile);
	}
}

/** Fork: Weht auf dieser Kachel die Firmen-Fahne (gekauftes Haus)? */
bool HouseOwnIsOwnedTile(TileIndex tile)
{
	return _owned_tiles.contains(tile.base());
}

static std::string HouseOwnFilePath()
{
	return _personal_dir + "houseown.dat";
}

static void HouseOwnSave()
{
	std::ofstream f(HouseOwnFilePath(), std::ios::binary | std::ios::trunc);
	if (!f.is_open()) return;
	f.write("HOW2", 4);
	for (const OwnedHouse &h : _owned_houses) {
		f.write(reinterpret_cast<const char *>(&h.seed), 4);
		f.write(reinterpret_cast<const char *>(&h.tile), 4);
		f.write(reinterpret_cast<const char *>(&h.house_type), 2);
		f.write(reinterpret_cast<const char *>(&h.price), 8);
		f.write(reinterpret_cast<const char *>(&h.rent), 8);
		f.write(reinterpret_cast<const char *>(&h.warned), 8);
	}
	f.close();
#ifdef __EMSCRIPTEN__
	EM_ASM(if (window["openttd_syncfs"]) openttd_syncfs());
#endif
}

static void HouseOwnLoad()
{
	if (_owned_loaded) return;
	_owned_loaded = true;
	std::ifstream f(HouseOwnFilePath(), std::ios::binary);
	if (!f.is_open()) return;
	char magic[4];
	f.read(magic, 4);
	/* HOW1 (ohne Warn-Datum) wird weiter gelesen, gespeichert wird HOW2. */
	bool v2 = f.good() && std::string_view(magic, 4) == "HOW2";
	if (!f.good() || (!v2 && std::string_view(magic, 4) != "HOW1")) return;
	for (;;) {
		OwnedHouse h;
		f.read(reinterpret_cast<char *>(&h.seed), 4);
		if (!f.good()) break;
		f.read(reinterpret_cast<char *>(&h.tile), 4);
		f.read(reinterpret_cast<char *>(&h.house_type), 2);
		f.read(reinterpret_cast<char *>(&h.price), 8);
		f.read(reinterpret_cast<char *>(&h.rent), 8);
		if (v2) f.read(reinterpret_cast<char *>(&h.warned), 8);
		if (!f.good()) break;
		_owned_houses.push_back(h);
	}
	RebuildOwnedTiles();
}

/** Nordkachel des Hauses auf dieser Kachel (mehrteilige Gebaeude). */
static TileIndex HouseNorthTile(TileIndex tile)
{
	HouseID house = GetHouseType(tile);
	return tile + GetHouseNorthPart(house);
}

static OwnedHouse *FindOwned(TileIndex tile)
{
	HouseOwnLoad();
	for (OwnedHouse &h : _owned_houses) {
		if (h.seed == CurrentSeed() && h.tile == tile.base()) return &h;
	}
	return nullptr;
}

/** Kaufpreis: Grundwert plus Bewohner; Miete zahlt ihn in ~7 Jahren ab. */
static Money HousePrice(const HouseSpec *hs)
{
	return 30000 + (Money)hs->population * 3500;
}

static Money HouseRent(const HouseSpec *hs)
{
	return HousePrice(hs) / 84;
}

/** Jaehrlicher Unterhalt: 1-5 % des Kaufpreises, fest je Kachel. */
static uint HouseUpkeepPct(uint32_t tile)
{
	return 1 + tile % 5;
}

/**
 * Fork: town_cmd.cpp fragt hier an, bevor die Stadt ein Haus fuer die
 * Erneuerung abreisst. Nicht gekaufte Haeuser: immer erlaubt (Vanilla).
 * Gekaufte Haeuser: erst ab HOUSEOWN_DEMOLITION_AGE Jahren, und auch dann
 * kommt zuerst eine Warnung mit einem Jahr Frist - wer rechtzeitig
 * renoviert (Alter sinkt unter die Grenze), behaelt das Haus.
 */
bool HouseOwnMayDemolish(TileIndex tile)
{
	HouseOwnLoad();
	if (_owned_tiles.empty()) return true;
	HouseID house = GetHouseType(tile);
	TileIndex north = tile + GetHouseNorthPart(house);
	if (!_owned_tiles.contains(north.base())) return true;
	OwnedHouse *owned = FindOwned(north);
	if (owned == nullptr) return true;

	auto age = GetHouseAge(north).base();
	if (age < HOUSEOWN_DEMOLITION_AGE) {
		/* Renoviert oder noch jung genug - eine alte Warnung verfaellt. */
		if (owned->warned != 0) {
			owned->warned = 0;
			HouseOwnSave();
			SetWindowClassesDirty(WindowClass::HouseInfo);
		}
		return false;
	}

	int64_t today = TimerGameCalendar::date.base();
	if (owned->warned == 0) {
		owned->warned = today;
		HouseOwnSave();
		Town *t = Town::GetByTile(north);
		if (t != nullptr) {
			AddTileNewsItem(GetEncodedString(STR_NEWS_HOUSEOWN_DECAY_WARNING, t->index, age),
					NewsType::General, north);
		}
		SetWindowClassesDirty(WindowClass::HouseInfo);
		return false;
	}
	return today >= owned->warned + HOUSEOWN_WARN_GRACE_DAYS;
}

/* Konsolen-Diagnose (housetest why): Zustand aller gekauften Haeuser. */
std::string HouseOwnDebugWhy()
{
	HouseOwnLoad();
	std::string out;
	for (const OwnedHouse &h : _owned_houses) {
		if (h.seed != CurrentSeed()) continue;
		TileIndex tile(h.tile);
		bool is_house = IsValidTile(tile) && IsTileType(tile, TileType::House);
		out += fmt::format("Kachel {}: haus={} typ={}/{}", h.tile, is_house ? 1 : 0, is_house ? GetHouseType(tile) : 0, h.house_type);
		if (is_house) {
			Town *t = Town::GetByTile(tile);
			out += fmt::format(" alter={} warned={} heute={} wachs={} rebuild={} minlife={}",
					GetHouseAge(tile).base(), h.warned, TimerGameCalendar::date.base(),
					t != nullptr && t->flags.Test(TownFlag::IsGrowing) ? 1 : 0,
					t != nullptr ? t->time_until_rebuild : 0,
					HouseSpec::Get(GetHouseType(tile))->minimum_life);
		}
		out += "; ";
	}
	if (out.empty()) out = "Keine gekauften Haeuser.";
	return out;
}

/* Konsolen-Diagnose (housetest decay/overdue): Haus kuenstlich altern
 * lassen, um Warnung und Abriss ohne 75 Spieljahre testen zu koennen. */
void HouseOwnDebugDecay(TileIndex tile, bool overdue)
{
	Tile t(tile);
	t.m5() = 200;
	OwnedHouse *owned = FindOwned(tile);
	if (owned != nullptr && overdue) {
		owned->warned = TimerGameCalendar::date.base() - (HOUSEOWN_WARN_GRACE_DAYS + 30);
		HouseOwnSave();
		/* Abriss-Lotterie der Stadt kurzschliessen, damit der Test nicht
		 * jahrelang auf --time_until_rebuild == 0 warten muss. WICHTIG:
		 * Town::GetByTile, nicht ClosestTownFromTile - die geografisch
		 * naechste Stadt kann eine andere sein als die Besitzer-Stadt. */
		Town *t2 = Town::GetByTile(tile);
		if (t2 != nullptr) {
			t2->time_until_rebuild = 1;
			/* Haus-Erneuerung laeuft nur in wachsenden Staedten (Vanilla);
			 * fuer den Test das Flag setzen (UpdateTownGrowth setzt es
			 * monatlich ohnehin neu). */
			t2->flags.Set(TownFlag::IsGrowing);
		}
	}
	MarkTileDirtyByTile(tile);
	SetWindowClassesDirty(WindowClass::HouseInfo);
}

/** Naechstgroesseres 1x1-Standardgebaeude gleicher Klimafamilie (Ausbau-Ziel). */
static HouseID FindUpgradeHouse(HouseID cur_id)
{
	const HouseSpec *cur = HouseSpec::Get(cur_id);
	if (!cur->building_flags.Test(BuildingFlag::Size1x1)) return INVALID_HOUSE_ID;
	HouseID best = INVALID_HOUSE_ID;
	uint best_pop = UINT_MAX;
	for (size_t id = 0; id < HouseSpec::Specs().size(); id++) {
		const HouseSpec *hs = HouseSpec::Get(id);
		if (!hs->enabled || hs->grf_prop.HasSpriteGroups()) continue;
		if (!hs->building_flags.Test(BuildingFlag::Size1x1)) continue;
		if (hs->population <= cur->population) continue;
		if (!(hs->building_availability & cur->building_availability).Any()) continue;
		if (hs->population < best_pop) {
			best_pop = hs->population;
			best = static_cast<HouseID>(id);
		}
	}
	return best;
}

/** Renovierungskosten: 15 % des Kaufpreises, mindestens 5k. */
static Money HouseRenovateCost(Money price)
{
	return std::max<Money>(price * 15 / 100, 5000);
}

/** Ausbaukosten: das 1,5-fache der Preisdifferenz, mindestens 20k. */
static Money HouseUpgradeCost(const HouseSpec *cur, const HouseSpec *next)
{
	return std::max<Money>((HousePrice(next) - HousePrice(cur)) * 3 / 2, 20000);
}

/* ---------- Monatliche Miete ---------- */

static void HouseOwnMonthly()
{
	if (_game_mode != GameMode::Normal) return;
	if (!Company::IsValidID(_local_company)) return;
	HouseOwnLoad();

	bool changed = false;
	for (auto it = _owned_houses.begin(); it != _owned_houses.end(); ) {
		if (it->seed != CurrentSeed()) { ++it; continue; }
		TileIndex tile(it->tile);
		if (!IsValidTile(tile) || !IsTileType(tile, TileType::House) || GetHouseType(tile) != it->house_type) {
			/* Die Stadt hat das Haus ersetzt - Besitz endet. */
			Town *t = ClosestTownFromTile(tile, UINT_MAX);
			if (t != nullptr) {
				AddNewsItem(GetEncodedString(STR_NEWS_HOUSEOWN_DEMOLISHED, t->index),
						NewsType::General, NewsStyle::Normal, {}, t->index);
			}
			it = _owned_houses.erase(it);
			changed = true;
			continue;
		}
		SubtractMoneyFromCompany(_local_company, CommandCost(ExpensesType::Other, -it->rent));
		ShowCostOrIncomeAnimation(TileX(tile) * TILE_SIZE + 8, TileY(tile) * TILE_SIZE + 8, GetTilePixelZ(tile), -it->rent);
		++it;
	}
	if (changed) {
		RebuildOwnedTiles();
		HouseOwnSave();
		SetWindowClassesDirty(WindowClass::HouseInfo);
	}
}

static const IntervalTimer<TimerGameCalendar> _houseown_monthly_timer = {{TimerGameCalendar::Trigger::Month, TimerGameCalendar::Priority::None}, [](auto) {
	HouseOwnMonthly();
}};

/** Jahresunterhalt der gekauften Haeuser (1-5 % vom Kaufpreis, Grundbesitz-Konto). */
static void HouseOwnYearly()
{
	if (_game_mode != GameMode::Normal) return;
	if (!Company::IsValidID(_local_company)) return;
	HouseOwnLoad();
	for (const OwnedHouse &h : _owned_houses) {
		if (h.seed != CurrentSeed()) continue;
		TileIndex tile(h.tile);
		if (!IsValidTile(tile) || !IsTileType(tile, TileType::House)) continue;
		Money upkeep = (Money)h.price * HouseUpkeepPct(h.tile) / 100;
		SubtractMoneyFromCompany(_local_company, CommandCost(ExpensesType::Property, upkeep));
		ShowCostOrIncomeAnimation(TileX(tile) * TILE_SIZE + 8, TileY(tile) * TILE_SIZE + 8, GetTilePixelZ(tile), upkeep);
	}
}

static const IntervalTimer<TimerGameCalendar> _houseown_yearly_timer = {{TimerGameCalendar::Trigger::Year, TimerGameCalendar::Priority::None}, [](auto) {
	HouseOwnYearly();
}};

/** Haus kaufen: prueft Spielmodus, Besitz und Geld; bucht und speichert. */
bool HouseOwnBuy(TileIndex tile)
{
	if (!IsValidTile(tile) || !IsTileType(tile, TileType::House)) return false;
	if (FindOwned(tile) != nullptr) return false;
	if (_game_mode != GameMode::Normal || !Company::IsValidID(_local_company)) return false;
	const HouseSpec *hs = HouseSpec::Get(GetHouseType(tile));
	Money price = HousePrice(hs);
	if (Company::Get(_local_company)->money < price) {
		ShowErrorMessage(GetEncodedString(STR_HOUSEOWN_NO_MONEY), {}, WarningLevel::Error);
		return false;
	}
	SubtractMoneyFromCompany(_local_company, CommandCost(ExpensesType::Property, price));
	ShowCostOrIncomeAnimation(TileX(tile) * TILE_SIZE + 8, TileY(tile) * TILE_SIZE + 8, GetTilePixelZ(tile), price);
	HouseOwnLoad();
	_owned_houses.push_back({CurrentSeed(), tile.base(), GetHouseType(tile), price, HouseRent(hs)});
	RebuildOwnedTiles();
	HouseOwnSave();
	SetWindowClassesDirty(WindowClass::HouseInfo);
	MarkWholeScreenDirty();
	return true;
}

/* ---------- Ganze Stadt auf einmal ---------- */

/**
 * Alle Haeuser einer Stadt einsammeln (je Gebaeude nur die Nordkachel).
 * Ueber eine Spirale statt die ganze Karte: eine Stadt ist selten
 * groesser als vierzig Kacheln im Radius.
 */
static std::vector<TileIndex> TownHouses(const Town *t)
{
	std::vector<TileIndex> out;
	if (t == nullptr) return out;
	std::set<uint32_t> seen;
	for (TileIndex tile : SpiralTileSequence(t->xy, 44)) {
		if (!IsTileType(tile, TileType::House)) continue;
		if (GetTownIndex(tile) != t->index) continue;
		TileIndex north = HouseNorthTile(tile);
		if (!seen.insert(north.base()).second) continue;
		out.push_back(north);
	}
	return out;
}

/**
 * Was kostet es, die ganze Stadt zu kaufen?
 * @param[out] count Anzahl der noch freien Haeuser.
 * @return Gesamtpreis.
 */
static Money TownBuyCost(const Town *t, uint &count)
{
	count = 0;
	Money sum = 0;
	for (TileIndex tile : TownHouses(t)) {
		if (FindOwned(tile) != nullptr) continue;
		sum += HousePrice(HouseSpec::Get(GetHouseType(tile)));
		count++;
	}
	return sum;
}

/**
 * Was kostet es, alle eigenen Haeuser der Stadt auszubauen?
 * @param[out] count Anzahl der ausbaufaehigen Haeuser.
 * @return Gesamtkosten.
 */
static Money TownUpgradeCost(const Town *t, uint &count)
{
	count = 0;
	Money sum = 0;
	for (TileIndex tile : TownHouses(t)) {
		if (FindOwned(tile) == nullptr) continue;
		HouseID cur = GetHouseType(tile);
		HouseID up = FindUpgradeHouse(cur);
		if (up == INVALID_HOUSE_ID) continue;
		sum += HouseUpgradeCost(HouseSpec::Get(cur), HouseSpec::Get(up));
		count++;
	}
	return sum;
}

/** Genug Geld beschaffen - notfalls per Kredit, wie beim Streckenbau. */
static bool HouseOwnEnsureFunds(Money needed)
{
	const Company *c = Company::Get(_local_company);
	if (c->money >= needed) return true;
	Money avail = c->money + (c->GetMaxLoan() - c->current_loan);
	if (avail < needed) return false;
	Command<Commands::IncreaseLoan>::Do(DoCommandFlag::Execute, LoanCommand::Amount, needed - c->money);
	return Company::Get(_local_company)->money >= needed;
}

/** Ein einzelnes Haus ausbauen. @return true bei Erfolg. */
static bool HouseOwnUpgradeOne(TileIndex tile)
{
	OwnedHouse *owned = FindOwned(tile);
	if (owned == nullptr) return false;
	HouseID cur_id = GetHouseType(tile);
	HouseID up = FindUpgradeHouse(cur_id);
	if (up == INVALID_HOUSE_ID) return false;
	const HouseSpec *up_hs = HouseSpec::Get(up);
	Money cost = HouseUpgradeCost(HouseSpec::Get(cur_id), up_hs);
	if (Company::Get(_local_company)->money < cost) return false;

	Backup<CompanyID> deity(_current_company, OWNER_DEITY);
	AutoRestoreBackup place(_settings_game.economy.place_houses, PlaceHouses::Allowed);
	CommandCost res = Command<Commands::PlaceHouse>::Do(DoCommandFlag::Execute, tile, up, false, true);
	deity.Restore();
	if (res.Failed()) return false;

	SubtractMoneyFromCompany(_local_company, CommandCost(ExpensesType::Property, cost));
	owned->house_type = GetHouseType(tile);
	owned->price = HousePrice(up_hs);
	owned->rent = HouseRent(up_hs);
	return true;
}

/**
 * Fork: Alle freien Haeuser einer Stadt kaufen.
 * @return Anzahl gekaufter Haeuser und ausgegebenes Geld.
 */
std::pair<uint, Money> HouseOwnBuyTown(TownID town_id)
{
	const Town *t = Town::GetIfValid(town_id);
	if (t == nullptr || !Company::IsValidID(_local_company)) return {0, 0};

	uint want = 0;
	Money needed = TownBuyCost(t, want);
	if (want == 0) return {0, 0};
	HouseOwnEnsureFunds(needed);

	uint bought = 0;
	Money spent = 0;
	for (TileIndex tile : TownHouses(t)) {
		if (FindOwned(tile) != nullptr) continue;
		Money price = HousePrice(HouseSpec::Get(GetHouseType(tile)));
		if (Company::Get(_local_company)->money < price) break;
		if (!HouseOwnBuy(tile)) continue;
		bought++;
		spent += price;
	}
	Debug(misc, 0, "Immobilien: {} von {} Haeusern in {} gekauft ({})", bought, want, t->index, (int64_t)spent);
	return {bought, spent};
}

/**
 * Fork: Alle eigenen Haeuser einer Stadt ausbauen.
 * @return Anzahl ausgebauter Haeuser und Kosten.
 */
std::pair<uint, Money> HouseOwnUpgradeTown(TownID town_id)
{
	const Town *t = Town::GetIfValid(town_id);
	if (t == nullptr || !Company::IsValidID(_local_company)) return {0, 0};

	uint want = 0;
	Money needed = TownUpgradeCost(t, want);
	if (want == 0) return {0, 0};
	HouseOwnEnsureFunds(needed);

	uint done = 0;
	Money before = Company::Get(_local_company)->money;
	for (TileIndex tile : TownHouses(t)) {
		if (HouseOwnUpgradeOne(tile)) done++;
	}
	Money spent = before - Company::Get(_local_company)->money;
	if (done > 0) {
		HouseOwnSave();
		SetWindowClassesDirty(WindowClass::HouseInfo);
		MarkWholeScreenDirty();
	}
	Debug(misc, 0, "Immobilien: {} von {} Haeusern in {} ausgebaut ({})", done, want, t->index, (int64_t)spent);
	return {done, spent};
}

/* ---------- Der Haus-Dialog ---------- */

struct HouseInfoWindow : Window {
	TileIndex tile; ///< Nordkachel des Hauses.
	/* Die Stadt-Zahlen kosten eine Spirale ueber tausende Kacheln -
	 * einmal rechnen und merken, nicht bei jedem Neuzeichnen. */
	uint town_buy_count = 0;
	Money town_buy_cost = 0;
	uint town_up_count = 0;
	Money town_up_cost = 0;

	HouseInfoWindow(WindowDesc &desc, WindowNumber number) : Window(desc), tile(number)
	{
		this->InitNested(number);
		this->RefreshTown();
	}

	/** Stadt-Zahlen neu berechnen. */
	void RefreshTown()
	{
		const Town *t = IsValidTile(this->tile) && IsTileType(this->tile, TileType::House)
				? Town::GetByTile(this->tile) : nullptr;
		this->town_buy_cost = TownBuyCost(t, this->town_buy_count);
		this->town_up_cost = TownUpgradeCost(t, this->town_up_count);
	}

	/** Alle paar Sekunden nachziehen - Staedte wachsen. */
	const IntervalTimer<TimerWindow> town_refresh = {std::chrono::seconds(5), [this](auto) {
		uint old_buy = this->town_buy_count, old_up = this->town_up_count;
		Money old_bc = this->town_buy_cost, old_uc = this->town_up_cost;
		this->RefreshTown();
		/* Nur neu vermessen, wenn sich wirklich etwas geaendert hat -
		 * ein ReInit bei jedem Tick laesst das Fenster zappeln. */
		if (old_buy != this->town_buy_count || old_up != this->town_up_count ||
				old_bc != this->town_buy_cost || old_uc != this->town_up_cost) {
			this->ReInit();
		}
		this->SetDirty();
	}};

	const HouseSpec *Spec() const
	{
		if (!IsValidTile(this->tile) || !IsTileType(this->tile, TileType::House)) return nullptr;
		return HouseSpec::Get(GetHouseType(this->tile));
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_HO_CAPTION) {
			const HouseSpec *hs = this->Spec();
			return GetString(STR_HOUSEOWN_CAPTION, hs != nullptr ? hs->building_name : STR_EMPTY);
		}
		if (widget == WID_HO_BUY_TOWN) {
			return this->town_buy_count == 0
					? GetString(STR_HOUSEOWN_BUY_TOWN_NONE)
					: GetString(STR_HOUSEOWN_BUY_TOWN, this->town_buy_count, this->town_buy_cost);
		}
		if (widget == WID_HO_UPGRADE_TOWN) {
			return this->town_up_count == 0
					? GetString(STR_HOUSEOWN_UPGRADE_TOWN_NONE)
					: GetString(STR_HOUSEOWN_UPGRADE_TOWN, this->town_up_count, this->town_up_cost);
		}
		return this->Window::GetWidgetString(widget, stringid);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget == WID_HO_BUY_TOWN || widget == WID_HO_UPGRADE_TOWN) {
			/* Die Zahlen stehen auf dem Knopf - das Fenster muss dafuer
			 * breit genug sein, sonst schneidet es den Preis ab. */
			Dimension d = GetStringBoundingBox(this->GetWidgetString(widget, STR_NULL));
			d.width += WidgetDimensions::scaled.framerect.Horizontal();
			d.height += WidgetDimensions::scaled.framerect.Vertical();
			size = maxdim(size, d);
			return;
		}
		if (widget != WID_HO_INFO) return;
		/* Breite an den laengsten Text anpassen - vorher wurde die
		 * Besitz-Zeile bei grossen Betraegen abgeschnitten. */
		uint text_w = ScaleGUITrad(200);
		const HouseSpec *hs = this->Spec();
		if (hs != nullptr) {
			uint pct = HouseUpkeepPct(this->tile.base());
			Money price = HousePrice(hs);
			std::string lines[] = {
				GetString(STR_HOUSEOWN_OWNED, price),
				GetString(STR_HOUSEOWN_PRICE, price),
				GetString(STR_HOUSEOWN_RENT_OUT, HouseRent(hs)),
				GetString(STR_HOUSEOWN_UPKEEP, price * pct / 100, pct),
			};
			for (const std::string &l : lines) text_w = std::max(text_w, GetStringBoundingBox(l).width);
		}
		size.width = std::max(size.width, static_cast<uint>(ScaleGUITrad(150)) + text_w + static_cast<uint>(ScaleGUITrad(16)));
		size.height = std::max({size.height, static_cast<uint>(8 * (GetCharacterHeight(FontSize::Normal) + 2) + ScaleGUITrad(8)), static_cast<uint>(ScaleGUITrad(120))});
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_HO_INFO) return;
		const HouseSpec *hs = this->Spec();
		if (hs == nullptr) return;
		Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);

		/* Portraet links: das angeklickte Gebaeude (inkl. Mehrfach-Kacheln). */
		int pw = ScaleGUITrad(140);
		{
			DrawPixelInfo tmp_dpi;
			int ph = tr.bottom - tr.top + 1;
			if (FillDrawPixelInfo(&tmp_dpi, tr.left, tr.top, pw, ph)) {
				AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
				/* Dieselbe Varianten-Wahl wie beim Kartenzeichnen, sonst
				 * zeigt das Portraet gelegentlich das falsche Gebaeude. */
				int view = TileHash2Bit(TileX(this->tile) * TILE_SIZE, TileY(this->tile) * TILE_SIZE);
				DrawHouseInGUI(pw / 2, ph * 7 / 10, GetHouseType(this->tile), view);
			}
		}
		tr.left += pw + ScaleGUITrad(8);

		int line = GetCharacterHeight(FontSize::Normal) + 2;
		int y = tr.top;
		Town *t = ClosestTownFromTile(this->tile, UINT_MAX);
		if (t != nullptr) {
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_TOWN, t->index));
			y += line;
		}
		if (hs->population > 0) {
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_POPULATION, hs->population));
		} else {
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_COMMERCIAL));
		}
		y += line;
		DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_AGE, GetHouseAge(this->tile).base()));
		y += line;

		const OwnedHouse *owned = FindOwned(this->tile);
		uint pct = HouseUpkeepPct(this->tile.base());
		if (owned != nullptr) {
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_OWNED, (Money)owned->price));
			y += line;
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_RENT_IN, (Money)owned->rent));
			y += line;
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_UPKEEP, (Money)owned->price * pct / 100, pct));
			y += line;
			HouseID up = FindUpgradeHouse(GetHouseType(this->tile));
			if (up != INVALID_HOUSE_ID) {
				const HouseSpec *cur_hs = HouseSpec::Get(GetHouseType(this->tile));
				const HouseSpec *up_hs = HouseSpec::Get(up);
				DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_UPGRADE_INFO,
						HouseUpgradeCost(cur_hs, up_hs), up_hs->population - cur_hs->population));
				y += line;
			}
			if (owned->warned != 0) {
				DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_DECAY_WARNED));
			}
		} else {
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_PRICE, HousePrice(hs)));
			y += line;
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_RENT_OUT, HouseRent(hs)));
			y += line;
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_UPKEEP, HousePrice(hs) * pct / 100, pct));
		}
	}

	void OnPaint() override
	{
		const HouseSpec *hs = this->Spec();
		bool can_buy = hs != nullptr && FindOwned(this->tile) == nullptr &&
				_game_mode == GameMode::Normal && Company::IsValidID(_local_company);
		this->SetWidgetDisabledState(WID_HO_BUY, !can_buy);
		bool can_upgrade = hs != nullptr && FindOwned(this->tile) != nullptr &&
				_game_mode == GameMode::Normal && Company::IsValidID(_local_company) &&
				FindUpgradeHouse(GetHouseType(this->tile)) != INVALID_HOUSE_ID;
		this->SetWidgetDisabledState(WID_HO_UPGRADE, !can_upgrade);
		bool can_renovate = hs != nullptr && FindOwned(this->tile) != nullptr &&
				_game_mode == GameMode::Normal && Company::IsValidID(_local_company) &&
				GetHouseAge(this->tile).base() > 0;
		this->SetWidgetDisabledState(WID_HO_RENOVATE, !can_renovate);
		bool playing = _game_mode == GameMode::Normal && Company::IsValidID(_local_company);
		this->SetWidgetDisabledState(WID_HO_BUY_TOWN, !playing || this->town_buy_count == 0);
		this->SetWidgetDisabledState(WID_HO_UPGRADE_TOWN, !playing || this->town_up_count == 0);
		this->DrawWidgets();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_HO_VIEW:
				ScrollMainWindowToTile(this->tile);
				break;

			case WID_HO_BUY_TOWN: {
				if (!IsValidTile(this->tile) || !IsTileType(this->tile, TileType::House)) break;
				const Town *t = Town::GetByTile(this->tile);
				if (t == nullptr) break;
				auto [bought, spent] = HouseOwnBuyTown(t->index);
				ShowErrorMessage(bought == 0 ? GetEncodedString(STR_HOUSEOWN_TOWN_NOTHING)
						: GetEncodedString(STR_HOUSEOWN_TOWN_BOUGHT, bought, spent), {}, WarningLevel::Info);
				this->RefreshTown();
				this->ReInit();
				break;
			}

			case WID_HO_UPGRADE_TOWN: {
				if (!IsValidTile(this->tile) || !IsTileType(this->tile, TileType::House)) break;
				const Town *t = Town::GetByTile(this->tile);
				if (t == nullptr) break;
				auto [done, spent] = HouseOwnUpgradeTown(t->index);
				ShowErrorMessage(done == 0 ? GetEncodedString(STR_HOUSEOWN_TOWN_NOTHING)
						: GetEncodedString(STR_HOUSEOWN_TOWN_UPGRADED, done, spent), {}, WarningLevel::Info);
				this->RefreshTown();
				this->ReInit();
				break;
			}

			case WID_HO_BUY:
				if (HouseOwnBuy(this->tile)) this->SetDirty();
				break;

			case WID_HO_RENOVATE: {
				OwnedHouse *owned = FindOwned(this->tile);
				if (owned == nullptr || _game_mode != GameMode::Normal || !Company::IsValidID(_local_company)) break;
				Money cost = HouseRenovateCost(owned->price);
				if (Company::Get(_local_company)->money < cost) {
					ShowErrorMessage(GetEncodedString(STR_HOUSEOWN_NO_MONEY), {}, WarningLevel::Error);
					break;
				}
				SubtractMoneyFromCompany(_local_company, CommandCost(ExpensesType::Property, cost));
				ShowCostOrIncomeAnimation(TileX(this->tile) * TILE_SIZE + 8, TileY(this->tile) * TILE_SIZE + 8, GetTilePixelZ(this->tile), cost);
				/* Verjuengen: Hausalter liegt in m5 (Jahre seit Fertigstellung). */
				Tile t(this->tile);
				t.m5() = static_cast<uint8_t>(std::max(0, (int)t.m5() - 10));
				/* Eine Abriss-Warnung ist mit der Renovierung vom Tisch;
				 * ist das Haus danach immer noch zu alt, warnt die Stadt
				 * neu und die Jahresfrist beginnt von vorn. */
				if (owned->warned != 0) {
					owned->warned = 0;
					HouseOwnSave();
				}
				MarkTileDirtyByTile(this->tile);
				this->SetDirty();
				break;
			}

			case WID_HO_UPGRADE: {
				OwnedHouse *owned = FindOwned(this->tile);
				if (owned == nullptr || _game_mode != GameMode::Normal || !Company::IsValidID(_local_company)) break;
				HouseID cur_id = GetHouseType(this->tile);
				HouseID up = FindUpgradeHouse(cur_id);
				if (up == INVALID_HOUSE_ID) break;
				const HouseSpec *cur_hs = HouseSpec::Get(cur_id);
				const HouseSpec *up_hs = HouseSpec::Get(up);
				Money cost = HouseUpgradeCost(cur_hs, up_hs);
				if (Company::Get(_local_company)->money < cost) {
					ShowErrorMessage(GetEncodedString(STR_HOUSEOWN_NO_MONEY), {}, WarningLevel::Error);
					break;
				}
				/* Neues Gebaeude an derselben Stelle errichten (ersetzt das alte). */
				Backup<CompanyID> deity(_current_company, OWNER_DEITY);
				AutoRestoreBackup place(_settings_game.economy.place_houses, PlaceHouses::Allowed);
				CommandCost res = Command<Commands::PlaceHouse>::Do(DoCommandFlag::Execute, this->tile, up, false, true);
				deity.Restore();
				if (res.Failed()) {
					ShowErrorMessage(GetEncodedString(STR_HOUSEOWN_UPGRADE_FAILED), {}, WarningLevel::Error);
					break;
				}
				SubtractMoneyFromCompany(_local_company, CommandCost(ExpensesType::Property, cost));
				ShowCostOrIncomeAnimation(TileX(this->tile) * TILE_SIZE + 8, TileY(this->tile) * TILE_SIZE + 8, GetTilePixelZ(this->tile), cost);
				owned->house_type = GetHouseType(this->tile);
				owned->price = HousePrice(up_hs);
				owned->rent = HouseRent(up_hs);
				HouseOwnSave();
				MarkWholeScreenDirty();
				break;
			}

			default:
				break;
		}
	}

	/** Jede Sekunde pruefen, ob es das Haus noch gibt (Stadt baut um). */
	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		if (this->Spec() == nullptr) this->Close();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_houseown_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_HO_CAPTION),
		NWidget(WWT_DEFSIZEBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey, WID_HO_INFO),
	EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_HO_VIEW), SetStringTip(STR_HOUSEOWN_VIEW, STR_HOUSEOWN_VIEW_TOOLTIP), SetFill(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_HO_RENOVATE), SetStringTip(STR_HOUSEOWN_RENOVATE, STR_HOUSEOWN_RENOVATE_TOOLTIP), SetFill(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_HO_UPGRADE), SetStringTip(STR_HOUSEOWN_UPGRADE, STR_HOUSEOWN_UPGRADE_TOOLTIP), SetFill(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Green, WID_HO_BUY), SetStringTip(STR_HOUSEOWN_BUY, STR_HOUSEOWN_BUY_TOOLTIP), SetFill(1, 0),
	EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_PUSHTXTBTN, Colours::Green, WID_HO_BUY_TOWN), SetToolTip(STR_HOUSEOWN_BUY_TOWN_TOOLTIP), SetFill(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_HO_UPGRADE_TOWN), SetToolTip(STR_HOUSEOWN_UPGRADE_TOWN_TOOLTIP), SetFill(1, 0),
	EndContainer(),
};

static WindowDesc _houseown_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::HouseInfo, WindowClass::None,
	{},
	_nested_houseown_widgets
);

/** Fork-Diagnose: Kaufpreis eines Hauses (fuer den housetest-Befehl). */
Money HouseOwnPriceAt(TileIndex tile)
{
	if (!IsValidTile(tile) || !IsTileType(tile, TileType::House)) return INT64_MAX;
	return HousePrice(HouseSpec::Get(GetHouseType(tile)));
}

/**
 * Fork: Viewport-Klick auf ein Wohnhaus - oeffnet den Haus-Dialog.
 * @return true wenn der Klick verarbeitet wurde.
 */
bool ShowHouseInfoOnClick(TileIndex tile)
{
	if (!_settings_client.gui.fork_houseown) return false;
	if (!IsValidTile(tile) || !IsTileType(tile, TileType::House)) return false;
	TileIndex north = HouseNorthTile(tile);
	AllocateWindowDescFront<HouseInfoWindow>(_houseown_desc, north.base());
	return true;
}

/* ==================== Immobilien-Pad (Fork-Feature) ==================== */

/**
 * Alle gekauften Haeuser auf einen Blick - wie das Stationen-Pad, nur
 * fuer Immobilien. Wer zwanzig Haeuser besitzt, findet sonst nicht mehr
 * heraus, welches davon gerade Aerger macht.
 *
 * Die Farbe sagt den Zustand: Rot heisst, die Stadt hat den Abriss
 * angekuendigt; Gelb heisst, das Haus ist alt genug, dass sie es bald
 * darf; Gruen heisst, alles in Ordnung.
 */
static bool _hp_only_risk = false; ///< Nur gefaehrdete Haeuser zeigen.

struct HousePadWindow : Window {
	std::vector<uint32_t> tiles;  ///< Angezeigte Haeuser (Nordkacheln).
	Scrollbar *vscroll = nullptr;
	uint line_height = 0;
	QueryString search_editbox;
	std::string search;
	static const uint COLUMNS = 2;

	HousePadWindow(WindowDesc &desc, WindowNumber number) : Window(desc), search_editbox(50)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_HP_SCROLLBAR);
		this->FinishInitNested(number);
		this->querystrings[WID_HP_SEARCH] = &this->search_editbox;
		this->search_editbox.cancel_button = QueryString::ACTION_CLEAR;
		this->BuildList();
	}

	/** Wie steht es um dieses Haus? 0 = gut, 1 = alt, 2 = Abriss droht. */
	static int RiskOf(const OwnedHouse &h)
	{
		TileIndex t{h.tile};
		if (!IsValidTile(t) || !IsTileType(t, TileType::House)) return 2;
		if (h.warned != 0) return 2;
		return GetHouseAge(t).base() >= HOUSEOWN_DEMOLITION_AGE ? 1 : 0;
	}

	/** Beschriftung: Stadt und Monatsmiete - danach sucht man. */
	static std::string LabelOf(const OwnedHouse &h)
	{
		TileIndex t{h.tile};
		const Town *town = IsValidTile(t) ? ClosestTownFromTile(t, UINT_MAX) : nullptr;
		std::string name = town != nullptr ? town->GetCachedName() : std::string("?");
		return name + " " + GetString(STR_HOUSEPAD_RENT, h.rent);
	}

	void BuildList()
	{
		this->tiles.clear();
		HouseOwnLoad();
		for (const OwnedHouse &h : _owned_houses) {
			if (h.seed != CurrentSeed()) continue;
			if (_hp_only_risk && RiskOf(h) == 0) continue;
			if (!this->search.empty()) {
				std::string label;
				for (char c : LabelOf(h)) label += (char)tolower((unsigned char)c);
				if (label.find(this->search) == std::string::npos) continue;
			}
			this->tiles.push_back(h.tile);
		}
		/* Die dringendsten Faelle nach oben. */
		std::sort(this->tiles.begin(), this->tiles.end(), [](uint32_t a, uint32_t b) {
			const OwnedHouse *ha = FindOwned(TileIndex{a});
			const OwnedHouse *hb = FindOwned(TileIndex{b});
			if (ha == nullptr || hb == nullptr) return false;
			int ra = RiskOf(*ha), rb = RiskOf(*hb);
			if (ra != rb) return ra > rb;
			return LabelOf(*ha) < LabelOf(*hb);
		});
		uint rows = static_cast<uint>((this->tiles.size() + COLUMNS - 1) / COLUMNS);
		this->vscroll->SetCount(rows);
		this->SetDirty();
	}

	void OnEditboxChanged(WidgetID widget) override
	{
		if (widget != WID_HP_SEARCH) return;
		this->search.clear();
		for (char c : this->search_editbox.text.GetText()) this->search += (char)tolower((unsigned char)c);
		this->BuildList();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget != WID_HP_SUMMARY && widget != WID_HP_HINT) return this->Window::GetWidgetString(widget, stringid);
		Money rent = 0;
		uint risk = 0;
		for (uint32_t tile : this->tiles) {
			const OwnedHouse *h = FindOwned(TileIndex{tile});
			if (h == nullptr) continue;
			rent += h->rent;
			if (RiskOf(*h) == 2) risk++;
		}
		if (widget == WID_HP_SUMMARY) return GetString(STR_HOUSEPAD_SUMMARY, (uint)this->tiles.size(), rent, risk);
		return GetString(STR_HOUSEPAD_HINT);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_HP_PANEL) return;
		this->line_height = GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.framerect.Vertical() + ScaleGUITrad(2);
		resize.height = this->line_height;
		size.height = 8 * this->line_height;
		size.width = std::max<uint>(size.width, ScaleGUITrad(340));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_HP_PANEL) return;
		Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
		uint col_w = ir.Width() / COLUMNS;
		uint first = this->vscroll->GetPosition() * COLUMNS;
		uint last = std::min<uint>(static_cast<uint>(this->tiles.size()), first + this->vscroll->GetCapacity() * COLUMNS);

		for (uint i = first; i < last; i++) {
			const OwnedHouse *h = FindOwned(TileIndex{this->tiles[i]});
			if (h == nullptr) continue;
			uint slot = i - first;
			int x = ir.left + (slot % COLUMNS) * col_w;
			int y = ir.top + (slot / COLUMNS) * this->line_height;
			Rect br = {x + 1, y + 1, x + (int)col_w - 2, y + (int)this->line_height - 2};
			int risk = RiskOf(*h);
			Colours colour = risk == 2 ? Colours::Red : (risk == 1 ? Colours::Orange : Colours::Green);
			DrawFrameRect(br, colour, FrameFlags{});
			DrawString(br.Shrink(WidgetDimensions::scaled.framerect), LabelOf(*h),
					TextColour::Black, AlignmentH::Centre);
		}

		if (this->tiles.empty()) {
			/* Zwischen "noch nichts gekauft" und "Filter zu eng" unterscheiden. */
			bool filtered = !this->search.empty() || _hp_only_risk;
			DrawString(ir, GetString(filtered ? STR_HOUSEPAD_NO_MATCH : STR_HOUSEPAD_EMPTY),
					TextColour::White, AlignmentH::Centre);
		}
	}

	/** Rasterplatz unter dem Mauszeiger; -1 wenn daneben. */
	int SlotAt(Point pt) const
	{
		Rect r = this->GetWidget<NWidgetBase>(WID_HP_PANEL)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
		if (!IsInsideMM(pt.y, r.top, r.bottom + 1) || !IsInsideMM(pt.x, r.left, r.right + 1)) return -1;
		uint col_w = r.Width() / COLUMNS;
		uint col = std::min<uint>(COLUMNS - 1, (pt.x - r.left) / std::max<uint>(1, col_w));
		uint row = (pt.y - r.top) / std::max<uint>(1, this->line_height);
		return static_cast<int>((this->vscroll->GetPosition() + row) * COLUMNS + col);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget == WID_HP_ONLY_RISK) {
			_hp_only_risk = !_hp_only_risk;
			this->BuildList();
			return;
		}
		if (widget != WID_HP_PANEL) return;
		int index = this->SlotAt(pt);
		if (index < 0 || index >= (int)this->tiles.size()) return;
		ScrollMainWindowToTile(TileIndex{this->tiles[index]});
	}

	/** Rechtsklick oeffnet den Haus-Dialog - dort wird renoviert. */
	bool OnRightClick([[maybe_unused]] Point pt, WidgetID widget) override
	{
		if (widget != WID_HP_PANEL) return false;
		int index = this->SlotAt(pt);
		if (index >= 0 && index < (int)this->tiles.size()) {
			AllocateWindowDescFront<HouseInfoWindow>(_houseown_desc, this->tiles[index]);
		}
		return true;
	}

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_HP_ONLY_RISK, _hp_only_risk);
		this->DrawWidgets();
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_HP_PANEL);
	}

	/** Alter und Warnungen aendern sich im Spiel - Liste nachziehen. */
	const IntervalTimer<TimerWindow> rebuild_interval = {std::chrono::seconds(3), [this](auto) {
		this->BuildList();
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_housepad_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_HP_CAPTION), SetStringTip(STR_HOUSEPAD_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_EDITBOX, Colours::Yellow, WID_HP_SEARCH), SetFill(1, 0), SetMinimalSize(150, 12), SetStringTip(STR_HOUSEPAD_SEARCH_HINT, STR_HOUSEPAD_SEARCH_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_HP_ONLY_RISK), SetMinimalSize(120, 12), SetStringTip(STR_HOUSEPAD_ONLY_RISK, STR_HOUSEPAD_ONLY_RISK_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PANEL, Colours::DarkGreen, WID_HP_PANEL), SetFill(1, 1), SetResize(1, 1), SetScrollbar(WID_HP_SCROLLBAR), EndContainer(),
				NWidget(NWID_VSCROLLBAR, Colours::DarkGreen, WID_HP_SCROLLBAR),
			EndContainer(),
			NWidget(WWT_TEXT, Colours::Invalid, WID_HP_SUMMARY), SetFill(1, 0),
			NWidget(WWT_TEXT, Colours::Invalid, WID_HP_HINT), SetFill(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_SPACER), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, Colours::DarkGreen),
	EndContainer(),
};

static WindowDesc _housepad_desc(
	WindowPosition::Automatic, "housepad", 360, 280,
	WindowClass::HousePad, WindowClass::None,
	{},
	_nested_housepad_widgets
);

/** Fork: Immobilien-Pad oeffnen. */
void ShowHousePadWindow()
{
	AllocateWindowDescFront<HousePadWindow>(_housepad_desc, 0);
}
