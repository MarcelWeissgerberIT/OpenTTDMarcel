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

/* town_gui.cpp: Haus samt Mehrfach-Kacheln im GUI zeichnen. */
void DrawHouseInGUI(int x, int y, HouseID house_id, int view);

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
};
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
	f.write("HOW1", 4);
	for (const OwnedHouse &h : _owned_houses) {
		f.write(reinterpret_cast<const char *>(&h.seed), 4);
		f.write(reinterpret_cast<const char *>(&h.tile), 4);
		f.write(reinterpret_cast<const char *>(&h.house_type), 2);
		f.write(reinterpret_cast<const char *>(&h.price), 8);
		f.write(reinterpret_cast<const char *>(&h.rent), 8);
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
	if (!f.good() || std::string_view(magic, 4) != "HOW1") return;
	for (;;) {
		OwnedHouse h;
		f.read(reinterpret_cast<char *>(&h.seed), 4);
		if (!f.good()) break;
		f.read(reinterpret_cast<char *>(&h.tile), 4);
		f.read(reinterpret_cast<char *>(&h.house_type), 2);
		f.read(reinterpret_cast<char *>(&h.price), 8);
		f.read(reinterpret_cast<char *>(&h.rent), 8);
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

/** Fork: Gekaufte Haeuser reisst die Stadt nicht ab (town_cmd.cpp fragt hier an). */
bool HouseOwnIsProtected(TileIndex tile)
{
	HouseOwnLoad();
	if (_owned_tiles.empty()) return false;
	HouseID house = GetHouseType(tile);
	TileIndex north = tile + GetHouseNorthPart(house);
	return _owned_tiles.contains(north.base());
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

/* ---------- Der Haus-Dialog ---------- */

struct HouseInfoWindow : Window {
	TileIndex tile; ///< Nordkachel des Hauses.

	HouseInfoWindow(WindowDesc &desc, WindowNumber number) : Window(desc), tile(number)
	{
		this->InitNested(number);
	}

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
		return this->Window::GetWidgetString(widget, stringid);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
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
		this->DrawWidgets();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_HO_VIEW:
				ScrollMainWindowToTile(this->tile);
				break;

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
	if (!IsValidTile(tile) || !IsTileType(tile, TileType::House)) return false;
	TileIndex north = HouseNorthTile(tile);
	AllocateWindowDescFront<HouseInfoWindow>(_houseown_desc, north.base());
	return true;
}
