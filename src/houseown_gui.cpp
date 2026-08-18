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

#include "widgets/houseown_widget.h"

/* town_gui.cpp: Haus samt Mehrfach-Kacheln im GUI zeichnen. */
void DrawHouseInGUI(int x, int y, HouseID house_id, int view);

#include "table/strings.h"

#include <fstream>

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

static uint32_t CurrentSeed()
{
	return _settings_game.game_creation.generation_seed;
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
		HouseOwnSave();
		SetWindowClassesDirty(WindowClass::HouseInfo);
	}
}

static const IntervalTimer<TimerGameCalendar> _houseown_monthly_timer = {{TimerGameCalendar::Trigger::Month, TimerGameCalendar::Priority::None}, [](auto) {
	HouseOwnMonthly();
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
	HouseOwnSave();
	SetWindowClassesDirty(WindowClass::HouseInfo);
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
		size.width = std::max(size.width, static_cast<uint>(ScaleGUITrad(150 + 240)));
		size.height = std::max({size.height, static_cast<uint>(6 * (GetCharacterHeight(FontSize::Normal) + 2) + ScaleGUITrad(8)), static_cast<uint>(ScaleGUITrad(130))});
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
				DrawHouseInGUI(pw / 2, ph * 3 / 4, GetHouseType(this->tile), view);
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
		if (owned != nullptr) {
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_OWNED, (Money)owned->price));
			y += line;
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_RENT_IN, (Money)owned->rent));
		} else {
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_PRICE, HousePrice(hs)));
			y += line;
			DrawString(tr.left, tr.right, y, GetString(STR_HOUSEOWN_RENT_OUT, HouseRent(hs)));
		}
	}

	void OnPaint() override
	{
		const HouseSpec *hs = this->Spec();
		bool can_buy = hs != nullptr && FindOwned(this->tile) == nullptr &&
				_game_mode == GameMode::Normal && Company::IsValidID(_local_company);
		this->SetWidgetDisabledState(WID_HO_BUY, !can_buy);
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
