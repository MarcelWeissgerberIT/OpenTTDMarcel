/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file assistant_gui.cpp Persoenlicher Assistent (Fork-Feature).
 *
 * Ein Fenster mit Schaltern und Slidern: Der Assistent modernisiert
 * Fahrzeuge, erneuert alternde Fahrzeuge (Vanilla-Autorenew), pflegt
 * die Stimmung der Staedte (Baeume, Werbung, notfalls Schmiergeld)
 * und bucht Werbekampagnen - jeweils im Rahmen des eingestellten
 * Monatsbudgets. Alle Aktionen landen in einem Protokoll. Dazu gibt
 * es einen Fernseher, in dem der eigene Werbespot laeuft.
 */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "command_func.h"
#include "core/geometry_func.hpp"
#include "gfx_func.h"
#include "strings_func.h"
#include "slider_func.h"
#include "company_base.h"
#include "company_func.h"
#include "town.h"
#include "town_cmd.h"
#include "settings_cmd.h"
#include "station_base.h"
#include "engine_base.h"
#include "vehicle_base.h"
#include "fileio_func.h"
#include "core/backup_type.hpp"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "zoom_func.h"

#include "widgets/assistant_widget.h"

#include "table/strings.h"
#include "table/sprites.h"

#include <deque>
#include <fstream>

#ifdef __EMSCRIPTEN__
#	include <emscripten.h>
#endif

#include "safeguards.h"

/* ---------------- Einstellungen des Assistenten ---------------- */

static bool _as_modernize = true;    ///< Fahrzeuge monatlich modernisieren (Auto-Modus-Timer).
static bool _as_renew = false;       ///< Vanilla-Autorenew einschalten.
static int _as_renew_months = 6;     ///< Monate vor Lebensende erneuern (0..12).
static int _as_town_step = 0;        ///< Stadtpflege-Budget in 5000er-Schritten (0..10).
static int _as_ad_step = 0;          ///< Werbebudget in 5000er-Schritten (0..10).
static std::deque<std::string> _as_log; ///< Letzte Aktionen (neueste vorn).

/** Vom Auto-Modus-Timer abgefragt: Modernisierung gewuenscht? */
bool AssistantModernizeEnabled()
{
	return _as_modernize;
}

static void AsLog(std::string &&entry)
{
	_as_log.push_front(std::move(entry));
	while (_as_log.size() > 8) _as_log.pop_back();
	SetWindowDirty(WindowClass::Assistant, 0);
}

/* ---------------- Persistenz (assistant.dat) ---------------- */

static std::string AssistantFilePath()
{
	return _personal_dir + "assistant.dat";
}

static void AssistantSave()
{
	std::ofstream f(AssistantFilePath(), std::ios::binary | std::ios::trunc);
	if (!f.is_open()) return;
	f.write("AST1", 4);
	uint8_t data[5] = {
		(uint8_t)(_as_modernize ? 1 : 0),
		(uint8_t)(_as_renew ? 1 : 0),
		(uint8_t)_as_renew_months,
		(uint8_t)_as_town_step,
		(uint8_t)_as_ad_step,
	};
	f.write(reinterpret_cast<const char *>(data), sizeof(data));
	f.close();
#ifdef __EMSCRIPTEN__
	EM_ASM(if (window["openttd_syncfs"]) openttd_syncfs());
#endif
}

static void AssistantLoad()
{
	static bool loaded = false;
	if (loaded) return;
	loaded = true;
	std::ifstream f(AssistantFilePath(), std::ios::binary);
	if (!f.is_open()) return;
	char magic[4];
	uint8_t data[5];
	f.read(magic, 4);
	f.read(reinterpret_cast<char *>(data), sizeof(data));
	if (!f.good() || std::string_view(magic, 4) != "AST1") return;
	_as_modernize = data[0] != 0;
	_as_renew = data[1] != 0;
	_as_renew_months = std::min<int>(data[2], 12);
	_as_town_step = std::min<int>(data[3], 10);
	_as_ad_step = std::min<int>(data[4], 10);
}

/* ---------------- Buergermeister (Politik) ----------------
 * Einmal kraeftig investieren, und die Stadt ist dauerhaft auf der
 * Seite der Firma: Das Rating wird monatlich auf Schmier-Maximum
 * gehalten, womit auch Laerm- und Anzahl-Verbote entfallen
 * (station_cmd laesst ab Rating 800 alles durch). Gemerkt wird der
 * Posten je Kartenstand (generation_seed) in mayor.dat. */

static std::set<TownID> _mayor_towns;

static std::string MayorFilePath()
{
	return _personal_dir + "mayor.dat";
}

static void MayorLoad()
{
	static uint32_t loaded_seed = UINT32_MAX;
	uint32_t seed = _settings_game.game_creation.generation_seed;
	if (loaded_seed == seed) return;
	loaded_seed = seed;
	_mayor_towns.clear();
	std::ifstream f(MayorFilePath(), std::ios::binary);
	if (!f.is_open()) return;
	char magic[4];
	uint32_t file_seed = 0;
	uint16_t count = 0;
	f.read(magic, 4);
	f.read(reinterpret_cast<char *>(&file_seed), 4);
	f.read(reinterpret_cast<char *>(&count), 2);
	if (!f.good() || std::string_view(magic, 4) != "MAY1" || file_seed != seed) return;
	for (uint i = 0; i < count; i++) {
		uint16_t id;
		f.read(reinterpret_cast<char *>(&id), 2);
		if (!f.good()) break;
		_mayor_towns.insert(static_cast<TownID>(id));
	}
}

static void MayorSave()
{
	std::ofstream f(MayorFilePath(), std::ios::binary | std::ios::trunc);
	if (!f.is_open()) return;
	uint32_t seed = _settings_game.game_creation.generation_seed;
	uint16_t count = static_cast<uint16_t>(_mayor_towns.size());
	f.write("MAY1", 4);
	f.write(reinterpret_cast<const char *>(&seed), 4);
	f.write(reinterpret_cast<const char *>(&count), 2);
	for (TownID id : _mayor_towns) {
		uint16_t raw = id.base();
		f.write(reinterpret_cast<const char *>(&raw), 2);
	}
	f.close();
#ifdef __EMSCRIPTEN__
	EM_ASM(if (window["openttd_syncfs"]) openttd_syncfs());
#endif
}

bool MayorInstalled(TownID town)
{
	MayorLoad();
	return _mayor_towns.count(town) != 0;
}

Money MayorPrice(const Town *t)
{
	return 100000 + (Money)t->cache.population * 100;
}

/** Buergermeister einsetzen; false = nicht genug Geld. */
bool MayorInstall(Town *t)
{
	MayorLoad();
	if (_mayor_towns.count(t->index) != 0) return true;
	Money price = MayorPrice(t);
	if (Company::Get(_local_company)->money < price) return false;
	Backup<CompanyID> cur_company(_current_company, _local_company);
	SubtractMoneyFromCompany(_local_company, CommandCost(ExpensesType::Property, price));
	cur_company.Restore();
	t->ratings[_local_company] = RATING_BRIBE_MAXIMUM;
	_mayor_towns.insert(t->index);
	MayorSave();
	AsLog(GetString(STR_ASSISTANT_LOG_MAYOR, t->index));
	SetWindowDirty(WindowClass::TownAuthority, t->index.base());
	return true;
}

/* ---------------- Verlorene Fahrzeuge melden ----------------
 * Wenn ein Fahrzeug keinen Weg mehr findet ("lost"), traegt der
 * Assistent das sofort ins Protokoll ein - so geht es nicht mehr im
 * Nachrichtenstrom unter. */
static std::set<VehicleID> _as_lost_reported;

static const IntervalTimer<TimerGameCalendar> _as_lost_timer = {{TimerGameCalendar::Trigger::Day, TimerGameCalendar::Priority::None}, [](auto) {
	if (!_settings_client.gui.fork_assistant) return;
	if (_game_mode != GameMode::Normal) return;
	if (!Company::IsValidID(_local_company)) return;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->owner != _local_company || !v->IsPrimaryVehicle()) continue;
		if (v->vehicle_flags.Test(VehicleFlag::PathfinderLost)) {
			if (_as_lost_reported.insert(v->index).second) {
				AsLog(GetString(STR_ASSISTANT_LOG_LOST, v->index));
			}
		} else {
			_as_lost_reported.erase(v->index);
		}
	}
}};

/* ---------------- Monatliche Arbeit des Assistenten ---------------- */

/** Staedte, in denen die Firma mit einer Station praesent ist. */
static std::vector<Town *> AsOwnTowns()
{
	std::set<TownID> seen;
	std::vector<Town *> towns;
	for (const Station *st : Station::Iterate()) {
		if (st->owner != _local_company || st->town == nullptr) continue;
		if (!seen.insert(st->town->index).second) continue;
		towns.push_back(st->town);
	}
	return towns;
}

/** Eine Stadtaktion ausfuehren, wenn Budget und Verfuegbarkeit passen. */
static bool AsTryTownAction(Town *t, TownAction action, Money &budget, StringID log_str)
{
	if (!GetMaskOfTownActions(_local_company, t).Test(action)) return false;
	CommandCost probe = Command<Commands::TownAction>::Do({}, t->index, action);
	if (probe.Failed() || probe.GetCost() > budget) return false;
	CommandCost c = Command<Commands::TownAction>::Do(DoCommandFlag::Execute, t->index, action);
	if (c.Failed()) return false;
	budget -= c.GetCost();
	AsLog(GetString(log_str, t->index));
	return true;
}

static void AssistantMonthly()
{
	if (!_settings_client.gui.fork_assistant) return;
	if (_game_mode != GameMode::Normal) return;
	if (!Company::IsValidID(_local_company)) return;
	AssistantLoad();

	Backup<CompanyID> cur_company(_current_company, _local_company);

	/* Buergermeister-Staedte bleiben dauerhaft bestens gestimmt. */
	MayorLoad();
	if (_settings_client.gui.fork_politics) for (TownID id : _mayor_towns) {
		Town *t = Town::GetIfValid(id);
		if (t != nullptr && t->ratings[_local_company] < RATING_BRIBE_MAXIMUM) {
			t->ratings[_local_company] = RATING_BRIBE_MAXIMUM;
		}
	}

	/* Stadtpflege: die am schlechtesten gestimmten eigenen Staedte zuerst. */
	Money town_budget = (Money)_as_town_step * 5000;
	if (town_budget > 0) {
		std::vector<Town *> towns = AsOwnTowns();
		std::sort(towns.begin(), towns.end(), [](const Town *a, const Town *b) {
			return a->ratings[_local_company] < b->ratings[_local_company];
		});
		for (Town *t : towns) {
			int rating = t->ratings[_local_company];
			if (rating >= 400) break; /* "gut" und besser braucht keine Pflege */
			if (rating < 0) {
				/* Richtig sauer: Schmiergeld wirkt am schnellsten. */
				if (AsTryTownAction(t, TownAction::Bribe, town_budget, STR_ASSISTANT_LOG_BRIBE)) continue;
			}
			/* Eine Statue verbessert die Stimmung dauerhaft. */
			if (AsTryTownAction(t, TownAction::BuildStatue, town_budget, STR_ASSISTANT_LOG_STATUE)) continue;
			AsTryTownAction(t, TownAction::AdvertiseSmall, town_budget, STR_ASSISTANT_LOG_AD);
		}
	}

	/* Werbung: haelt die Bewertung der eigenen Stationen hoch. */
	Money ad_budget = (Money)_as_ad_step * 5000;
	if (ad_budget > 0) {
		for (Town *t : AsOwnTowns()) {
			if (AsTryTownAction(t, TownAction::AdvertiseLarge, ad_budget, STR_ASSISTANT_LOG_AD)) continue;
			if (AsTryTownAction(t, TownAction::AdvertiseMedium, ad_budget, STR_ASSISTANT_LOG_AD)) continue;
			if (!AsTryTownAction(t, TownAction::AdvertiseSmall, ad_budget, STR_ASSISTANT_LOG_AD)) break;
		}
	}

	cur_company.Restore();
}

static const IntervalTimer<TimerGameCalendar> _assistant_timer = {{TimerGameCalendar::Trigger::Month, TimerGameCalendar::Priority::None}, [](auto) {
	AssistantMonthly();
}};

/* ---------------- Der Fernseher (Werbespot) ---------------- */

/** Bestes eigenes Strassenfahrzeug fuer den Spot. */
static EngineID AsTvVehicle()
{
	extern SpriteID GetRoadVehBaseSprite(EngineID engine);
	EngineID best = EngineID::Invalid();
	uint best_cap = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (GetRoadVehBaseSprite(e->index) == 0) continue;
		uint cap = e->GetDisplayDefaultCapacity();
		if (cap >= best_cap) {
			best_cap = cap;
			best = e->index;
		}
	}
	return best;
}

struct AssistantTvWindow : Window {
	uint anim_ms = 0; ///< Laufende Spielzeit des Spots.

	AssistantTvWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->InitNested(number);
	}

	void OnRealtimeTick(uint delta_ms) override
	{
		this->anim_ms += delta_ms;
		this->SetWidgetDirty(WID_ATV_SCREEN);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget == WID_ATV_SCREEN) {
			size = maxdim(size, Dimension(ScaleGUITrad(280), ScaleGUITrad(170)));
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_ATV_SCREEN) return;
		int b = ScaleGUITrad(10); /* Gehaeuse-Rand */
		/* Gehaeuse mit Standfuss und zwei Knoepfen. */
		GfxFillRect(r.left, r.top, r.right, r.bottom, PixelColour{GREY_SCALE(3)});
		GfxFillRect(r.left + 2, r.top + 2, r.right - 2, r.bottom - 2, PixelColour{GREY_SCALE(5)});
		int kx = r.right - ScaleGUITrad(6);
		GfxFillRect(kx - 2, r.top + b + 2, kx + 2, r.top + b + 6, PC_BLACK);
		GfxFillRect(kx - 2, r.top + b + 10, kx + 2, r.top + b + 14, PC_BLACK);

		Rect s = {r.left + b, r.top + b, r.right - b, r.bottom - b};
		DrawPixelInfo tmp_dpi;
		if (!FillDrawPixelInfo(&tmp_dpi, s.left, s.top, s.right - s.left + 1, s.bottom - s.top + 1)) return;
		AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
		int w = s.right - s.left + 1;
		int h = s.bottom - s.top + 1;

		const Company *c = Company::GetIfValid(_local_company);
		uint scene = (this->anim_ms / 2600) % 3;
		switch (scene) {
			case 0: {
				/* Titelbild: Firmenfahne und Name. */
				GfxFillRect(0, 0, w - 1, h - 1, PixelColour{GREY_SCALE(13)});
				if (c != nullptr) {
					DrawSprite(SPR_COMPANY_FLAG, GetCompanyPalette(_local_company), w / 2 - ScaleGUITrad(8), h / 3 - ScaleGUITrad(8));
					DrawString(0, w - 1, h * 2 / 3, GetString(STR_ASSISTANT_TV_NAME, _local_company), TextColour::Black, AlignmentH::Centre);
				}
				break;
			}
			case 1: {
				/* Fahrender Bus auf der Strasse. */
				GfxFillRect(0, 0, w - 1, h - 1, PixelColour{GREY_SCALE(12)});
				GfxFillRect(0, h * 2 / 3, w - 1, h - 1, PixelColour{0x57}); /* Wiese */
				GfxFillRect(0, h / 2 + ScaleGUITrad(6), w - 1, h * 2 / 3, PixelColour{GREY_SCALE(6)}); /* Strasse */
				EngineID e = AsTvVehicle();
				if (e != EngineID::Invalid()) {
					extern SpriteID GetRoadVehBaseSprite(EngineID engine);
					int x = (int)((this->anim_ms / 12) % (uint)(w + 60)) - 30;
					DrawSprite(GetRoadVehBaseSprite(e) + 6, GetCompanyPalette(_local_company), x, h / 2 + ScaleGUITrad(4));
				}
				if ((this->anim_ms / 400) % 2 == 0) {
					DrawString(0, w - 1, ScaleGUITrad(6), GetString(STR_ASSISTANT_TV_SLOGAN2), TextColour::Black, AlignmentH::Centre);
				}
				break;
			}
			default: {
				/* Abbinder: Slogan auf dunklem Grund. */
				GfxFillRect(0, 0, w - 1, h - 1, PixelColour{GREY_SCALE(2)});
				if (c != nullptr) {
					DrawString(0, w - 1, h / 2 - GetCharacterHeight(FontSize::Normal), GetString(STR_ASSISTANT_TV_SLOGAN1, _local_company), TextColour::White, AlignmentH::Centre);
				}
				if ((this->anim_ms / 500) % 2 == 0) {
					DrawString(0, w - 1, h / 2 + GetCharacterHeight(FontSize::Normal), GetString(STR_ASSISTANT_TV_SLOGAN2), TextColour::White, AlignmentH::Centre);
				}
				break;
			}
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_assistant_tv_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Brown),
		NWidget(WWT_CAPTION, Colours::Brown, WID_ATV_CAPTION), SetStringTip(STR_ASSISTANT_TV_CAPTION),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Brown),
		NWidget(WWT_EMPTY, Colours::Invalid, WID_ATV_SCREEN), SetPadding(4, 4, 4, 4),
	EndContainer(),
};

static WindowDesc _assistant_tv_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::AssistantTv, WindowClass::None,
	{},
	_nested_assistant_tv_widgets
);

static void ShowAssistantTv()
{
	AllocateWindowDescFront<AssistantTvWindow>(_assistant_tv_desc, 0);
}

/* ---------------- Das Assistenten-Fenster ---------------- */

struct AssistantWindow : Window {
	AssistantWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		AssistantLoad();
		this->InitNested(number);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_AS_RENEW_LABEL: return GetString(STR_ASSISTANT_RENEW_MONTHS, _as_renew_months);
			case WID_AS_TOWN_LABEL: return GetString(STR_ASSISTANT_TOWN_BUDGET, (Money)_as_town_step * 5000);
			case WID_AS_AD_LABEL: return GetString(STR_ASSISTANT_AD_BUDGET, (Money)_as_ad_step * 5000);
			default: return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_AS_RENEW_SLIDER:
			case WID_AS_TOWN_SLIDER:
			case WID_AS_AD_SLIDER:
				size = maxdim(size, Dimension(ScaleGUITrad(140), ScaleGUITrad(14)));
				break;
			case WID_AS_RENEW_LABEL:
				size.width = std::max(size.width, GetStringBoundingBox(GetString(STR_ASSISTANT_RENEW_MONTHS, 12)).width);
				break;
			case WID_AS_TOWN_LABEL:
				size.width = std::max(size.width, GetStringBoundingBox(GetString(STR_ASSISTANT_TOWN_BUDGET, (Money)50000)).width);
				break;
			case WID_AS_AD_LABEL:
				size.width = std::max(size.width, GetStringBoundingBox(GetString(STR_ASSISTANT_AD_BUDGET, (Money)50000)).width);
				break;
			case WID_AS_LOG:
				size = maxdim(size, Dimension(ScaleGUITrad(240), 8 * GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.framerect.Vertical()));
				break;
			default:
				break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_AS_RENEW_SLIDER:
				DrawSliderWidget(r, Colours::Grey, Colours::Grey, TextColour::Black, 0, 12, 12, _as_renew_months, nullptr);
				break;
			case WID_AS_TOWN_SLIDER:
				DrawSliderWidget(r, Colours::Grey, Colours::Grey, TextColour::Black, 0, 10, 10, _as_town_step, nullptr);
				break;
			case WID_AS_AD_SLIDER:
				DrawSliderWidget(r, Colours::Grey, Colours::Grey, TextColour::Black, 0, 10, 10, _as_ad_step, nullptr);
				break;
			case WID_AS_LOG: {
				Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);
				int y = tr.top;
				if (_as_log.empty()) {
					DrawString(tr.left, tr.right, y, STR_ASSISTANT_LOG_EMPTY, TextColour::Grey);
					break;
				}
				for (const std::string &line : _as_log) {
					DrawString(tr.left, tr.right, y, line, TextColour::Black);
					y += GetCharacterHeight(FontSize::Normal);
					if (y > tr.bottom) break;
				}
				break;
			}
			default:
				break;
		}
	}

	/** Vanilla-Autorenew der Firma an die Slider-Werte anpassen. */
	void ApplyRenewSettings()
	{
		Command<Commands::ChangeCompanySetting>::Post("company.engine_renew", _as_renew ? 1 : 0);
		Command<Commands::ChangeCompanySetting>::Post("company.engine_renew_months", -_as_renew_months);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_AS_MODERNIZE:
				_as_modernize = !_as_modernize;
				AssistantSave();
				this->SetDirty();
				break;

			case WID_AS_RENEW:
				_as_renew = !_as_renew;
				this->ApplyRenewSettings();
				AssistantSave();
				this->SetDirty();
				break;

			case WID_AS_RENEW_SLIDER:
				if (ClickSliderWidget(this->GetWidget<NWidgetBase>(widget)->GetCurrentRect(), pt, 0, 12, 12, _as_renew_months)) {
					this->ApplyRenewSettings();
					AssistantSave();
					this->SetDirty();
				}
				if (click_count > 0) this->mouse_capture_widget = widget;
				break;

			case WID_AS_TOWN_SLIDER:
				if (ClickSliderWidget(this->GetWidget<NWidgetBase>(widget)->GetCurrentRect(), pt, 0, 10, 10, _as_town_step)) {
					AssistantSave();
					this->SetDirty();
				}
				if (click_count > 0) this->mouse_capture_widget = widget;
				break;

			case WID_AS_AD_SLIDER:
				if (ClickSliderWidget(this->GetWidget<NWidgetBase>(widget)->GetCurrentRect(), pt, 0, 10, 10, _as_ad_step)) {
					AssistantSave();
					this->SetDirty();
				}
				if (click_count > 0) this->mouse_capture_widget = widget;
				break;

			case WID_AS_TV:
				ShowAssistantTv();
				break;

			default:
				break;
		}
	}

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_AS_MODERNIZE, _as_modernize);
		this->SetWidgetLoweredState(WID_AS_RENEW, _as_renew);
		this->DrawWidgets();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_assistant_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_AS_CAPTION), SetStringTip(STR_ASSISTANT_CAPTION),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(WWT_TEXTBTN, Colours::Yellow, WID_AS_MODERNIZE), SetFill(1, 0), SetMinimalSize(240, 14), SetStringTip(STR_ASSISTANT_MODERNIZE, STR_ASSISTANT_MODERNIZE_TOOLTIP),
			NWidget(WWT_TEXTBTN, Colours::Yellow, WID_AS_RENEW), SetFill(1, 0), SetMinimalSize(240, 14), SetStringTip(STR_ASSISTANT_RENEW, STR_ASSISTANT_RENEW_TOOLTIP),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXT, Colours::Invalid, WID_AS_RENEW_LABEL), SetFill(1, 0), SetAlignment({AlignmentH::Start, AlignmentV::Middle}),
				NWidget(WWT_EMPTY, Colours::Invalid, WID_AS_RENEW_SLIDER), SetFill(1, 0), SetToolTip(STR_ASSISTANT_RENEW_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXT, Colours::Invalid, WID_AS_TOWN_LABEL), SetFill(1, 0), SetAlignment({AlignmentH::Start, AlignmentV::Middle}),
				NWidget(WWT_EMPTY, Colours::Invalid, WID_AS_TOWN_SLIDER), SetFill(1, 0), SetToolTip(STR_ASSISTANT_TOWN_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXT, Colours::Invalid, WID_AS_AD_LABEL), SetFill(1, 0), SetAlignment({AlignmentH::Start, AlignmentV::Middle}),
				NWidget(WWT_EMPTY, Colours::Invalid, WID_AS_AD_SLIDER), SetFill(1, 0), SetToolTip(STR_ASSISTANT_AD_TOOLTIP),
			EndContainer(),
			NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_AS_TV), SetFill(1, 0), SetMinimalSize(240, 14), SetStringTip(STR_ASSISTANT_TV, STR_ASSISTANT_TV_TOOLTIP),
			NWidget(WWT_INSET, Colours::Grey, WID_AS_LOG), SetFill(1, 1), EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _assistant_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::Assistant, WindowClass::None,
	{},
	_nested_assistant_widgets
);

/** Assistenten-Fenster oeffnen (Fork-Feature). */
void ShowAssistantWindow()
{
	AssistantLoad();
	AllocateWindowDescFront<AssistantWindow>(_assistant_desc, 0);
}
