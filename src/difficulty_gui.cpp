/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file difficulty_gui.cpp Schwierigkeitsstufen (Fork-Feature).
 *
 * Vier Stufen von "leicht" bis "fast unmoeglich" statt eines Dutzends
 * Einzelschalter. Jede Stufe setzt Kreditrahmen, Zinsen, Bau- und
 * Betriebskosten, Subventionen, Pannen, Wirtschaftsschwankungen,
 * Katastrophen und die Zahl der Mitbewerber auf einen Schlag.
 *
 * ACHTUNG: Einige dieser Werte traegt OpenTTD als "nur fuer neue
 * Spiele" - Kreditrahmen, Zinsen, Bau- und Betriebskosten. Sie landen
 * deshalb in den Einstellungen fuer das naechste Spiel; alles andere
 * wirkt sofort. Der Dialog sagt das auch.
 */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "settings_type.h"
#include "settings_func.h"
#include "economy_func.h"
#include "network/network.h"
#include "company_base.h"
#include "company_func.h"
#include "core/geometry_func.hpp"
#include "zoom_func.h"

#include "widgets/difficulty_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/** Die vier Stufen. */
enum class DiffLevel : uint8_t {
	Easy,   ///< Wie bisher: viel Geld, keine Pannen, keine Konkurrenz.
	Medium, ///< Die Vorgabe von OpenTTD.
	Hard,   ///< Teurer Bau, schwankende Wirtschaft, Konkurrenz.
	Brutal, ///< Wenig Kapital, hohe Zinsen, Katastrophen, volles Feld.
	End,
};

/** Alle Werte einer Stufe an einem Ort. */
struct DiffPreset {
	uint32_t max_loan;        ///< Kreditrahmen (= Startkapital).
	uint8_t interest;         ///< Zinssatz in Prozent.
	uint8_t vehicle_costs;    ///< Betriebskosten (0 niedrig .. 2 hoch).
	uint8_t construction;     ///< Baukosten (0 niedrig .. 2 hoch).
	uint8_t subsidy;          ///< Subventionsfaktor (0 keine .. 3 hoch).
	VehicleBreakdowns breakdowns; ///< Pannenhaeufigkeit.
	uint8_t competitors;      ///< Zahl der Mitbewerber.
	bool volatile_economy;    ///< Schwankende Wirtschaft.
	bool disasters;           ///< Katastrophen.
	bool inflation;           ///< Inflation.
	StringID name;            ///< Anzeigename.
	StringID details;         ///< Beschreibung.
};

static const DiffPreset _diff_presets[] = {
	{500000, 2, 0, 0, 3, VehicleBreakdowns::None,    0, false, false, false, STR_DIFFICULTY_EASY,   STR_DIFFICULTY_EASY_DETAILS},
	{300000, 2, 1, 1, 2, VehicleBreakdowns::Reduced, 2, false, false, false, STR_DIFFICULTY_MEDIUM, STR_DIFFICULTY_MEDIUM_DETAILS},
	{200000, 3, 2, 2, 1, VehicleBreakdowns::Normal,  5, true,  true,  true,  STR_DIFFICULTY_HARD,   STR_DIFFICULTY_HARD_DETAILS},
	{100000, 4, 2, 2, 0, VehicleBreakdowns::Normal, 10, true,  true,  true,  STR_DIFFICULTY_BRUTAL, STR_DIFFICULTY_BRUTAL_DETAILS},
};
static_assert(lengthof(_diff_presets) == (size_t)DiffLevel::End);

/**
 * Welche Stufe passt am ehesten zu den aktuellen Einstellungen? So
 * zeigt der Dialog beim Oeffnen den tatsaechlichen Stand, ohne dass wir
 * die Wahl irgendwo zusaetzlich speichern muessten.
 */
static DiffLevel GuessCurrentLevel()
{
	const DifficultySettings &d = _settings_game.difficulty;
	int best = 0;
	int best_score = -1;
	for (size_t i = 0; i < lengthof(_diff_presets); i++) {
		const DiffPreset &p = _diff_presets[i];
		int score = 0;
		if (d.vehicle_costs == p.vehicle_costs) score++;
		if (d.construction_cost == p.construction) score++;
		if (d.subsidy_multiplier == p.subsidy) score++;
		if (d.vehicle_breakdowns == p.breakdowns) score++;
		if (d.economy == p.volatile_economy) score++;
		if (d.disasters == p.disasters) score++;
		if (score > best_score) { best_score = score; best = (int)i; }
	}
	return (DiffLevel)best;
}

/** Eine Stufe uebernehmen. */
static void ApplyDifficulty(DiffLevel level)
{
	const DiffPreset &p = _diff_presets[(size_t)level];

	/* Sofort wirksam - diese Werte darf das laufende Spiel aendern. */
	for (GameSettings *gs : {&_settings_game, &_settings_newgame}) {
		gs->difficulty.subsidy_multiplier = p.subsidy;
		gs->difficulty.vehicle_breakdowns = p.breakdowns;
		gs->difficulty.max_no_competitors = p.competitors;
		gs->difficulty.economy = p.volatile_economy;
		gs->difficulty.disasters = p.disasters;
		gs->economy.inflation = p.inflation;
	}

	/* Nur fuer neue Spiele: OpenTTD friert Kreditrahmen, Zinsen sowie
	 * Bau- und Betriebskosten beim Spielstart ein. */
	_settings_newgame.difficulty.max_loan = p.max_loan;
	_settings_newgame.difficulty.initial_interest = p.interest;
	_settings_newgame.difficulty.vehicle_costs = p.vehicle_costs;
	_settings_newgame.difficulty.construction_cost = p.construction;

	SetWindowClassesDirty(WindowClass::Difficulty);
	SaveToConfig();
}

struct DifficultyWindow : Window {
	DiffLevel level = DiffLevel::Easy;

	DifficultyWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->level = GuessCurrentLevel();
		this->InitNested(number);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_DL_EASY:   return GetString(STR_DIFFICULTY_EASY);
			case WID_DL_MEDIUM: return GetString(STR_DIFFICULTY_MEDIUM);
			case WID_DL_HARD:   return GetString(STR_DIFFICULTY_HARD);
			case WID_DL_BRUTAL: return GetString(STR_DIFFICULTY_BRUTAL);
			default: return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_DL_DETAILS) return;
		size.width = std::max<uint>(size.width, ScaleGUITrad(330));
		uint max_h = 0;
		for (const DiffPreset &p : _diff_presets) {
			max_h = std::max<uint>(max_h, GetStringHeight(p.details, size.width));
		}
		/* Der Hinweis braucht seinen eigenen Platz - pauschal zwei Zeilen
		 * reichten nicht, er wurde abgeschnitten. */
		size.height = max_h + WidgetDimensions::scaled.vsep_normal
				+ GetStringHeight(STR_DIFFICULTY_NEWGAME_NOTE, size.width);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_DL_DETAILS) return;
		const DiffPreset &p = _diff_presets[(size_t)this->level];
		int y = DrawStringMultiLine(r.left, r.right, r.top, r.bottom, p.details, TextColour::Black);
		/* Der Hinweis gehoert dazu: sonst wundert man sich, warum das
		 * Geld im laufenden Spiel gleich bleibt. */
		DrawStringMultiLine(r.left, r.right, y + WidgetDimensions::scaled.vsep_normal, r.bottom,
				STR_DIFFICULTY_NEWGAME_NOTE, TextColour::Orange);
	}

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_DL_EASY, this->level == DiffLevel::Easy);
		this->SetWidgetLoweredState(WID_DL_MEDIUM, this->level == DiffLevel::Medium);
		this->SetWidgetLoweredState(WID_DL_HARD, this->level == DiffLevel::Hard);
		this->SetWidgetLoweredState(WID_DL_BRUTAL, this->level == DiffLevel::Brutal);
		this->DrawWidgets();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_DL_EASY:
			case WID_DL_MEDIUM:
			case WID_DL_HARD:
			case WID_DL_BRUTAL:
				this->level = (DiffLevel)(widget - WID_DL_EASY);
				this->SetDirty();
				break;

			case WID_DL_APPLY:
				ApplyDifficulty(this->level);
				this->Close();
				break;
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_difficulty_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_DL_CAPTION), SetStringTip(STR_DIFFICULTY_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_DL_EASY), SetFill(1, 0), SetToolTip(STR_DIFFICULTY_PICK_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_DL_MEDIUM), SetFill(1, 0), SetToolTip(STR_DIFFICULTY_PICK_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_DL_HARD), SetFill(1, 0), SetToolTip(STR_DIFFICULTY_PICK_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Yellow, WID_DL_BRUTAL), SetFill(1, 0), SetToolTip(STR_DIFFICULTY_PICK_TOOLTIP),
			EndContainer(),
			NWidget(WWT_EMPTY, Colours::Invalid, WID_DL_DETAILS), SetFill(1, 1),
			NWidget(WWT_PUSHTXTBTN, Colours::Green, WID_DL_APPLY), SetFill(1, 0), SetMinimalSize(120, 14), SetStringTip(STR_DIFFICULTY_APPLY, STR_DIFFICULTY_APPLY_TOOLTIP),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _difficulty_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::Difficulty, WindowClass::None,
	{},
	_nested_difficulty_widgets
);

/** Fork: Schwierigkeitsstufen-Fenster oeffnen. */
void ShowDifficultyWindow()
{
	CloseWindowById(WindowClass::Difficulty, 0);
	AllocateWindowDescFront<DifficultyWindow>(_difficulty_desc, 0);
}
