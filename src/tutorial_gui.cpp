/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file tutorial_gui.cpp Gefuehrter Einstieg (Fork-Feature).
 *
 * Beim ersten Spiel begleitet ein kleines Fenster durch die ersten
 * Minuten: Auto-Verbindung oeffnen, zwei Staedte verbinden, Zeitraffer
 * anwerfen, Geld verdienen. Der Begleiter erkennt selbst, wann ein
 * Schritt erledigt ist, und hebt den passenden Knopf in der
 * Werkzeugleiste hervor. Er zeigt sich nur einmal; ueber das
 * Hilfe-Menue laesst er sich jederzeit wieder oeffnen.
 */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "station_base.h"
#include "vehicle_base.h"
#include "company_base.h"
#include "company_func.h"
#include "fileio_func.h"
#include "openttd.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "zoom_func.h"

#include "widgets/tutorial_widget.h"
#include "widgets/toolbar_widget.h"

#include "table/strings.h"

#include <fstream>

#ifdef __EMSCRIPTEN__
#	include <emscripten.h>
#endif

#include "safeguards.h"

void ShowTutorialWindow();

/** Die Schritte des gefuehrten Einstiegs. */
enum TutorialStep : uint8_t {
	TUT_WELCOME,     ///< Begruessung, was uns erwartet.
	TUT_AUTOCONNECT, ///< Auto-Verbindung oeffnen.
	TUT_BUILD,       ///< Zwei Staedte waehlen und bauen lassen.
	TUT_SPEED,       ///< Zeitraffer anwerfen.
	TUT_DONE,        ///< Geschafft - wie es weitergeht.
	TUT_STEP_END,
};

/** Beschreibung eines Schrittes. */
struct TutorialStepInfo {
	StringID title;      ///< Ueberschrift.
	StringID body;       ///< Erklaerender Text.
	WidgetID highlight;  ///< Knopf der Werkzeugleiste, der blinken soll (-1 = keiner).
	StringID action;     ///< Beschriftung des Aktionsknopfs (STR_NULL = keiner).
};

static const TutorialStepInfo _tutorial_steps[] = {
	{STR_TUTORIAL_S1_TITLE, STR_TUTORIAL_S1_BODY, -1,                  STR_TUTORIAL_ACT_START},
	{STR_TUTORIAL_S2_TITLE, STR_TUTORIAL_S2_BODY, WID_TN_AUTOCONNECT,  STR_TUTORIAL_ACT_OPEN},
	{STR_TUTORIAL_S3_TITLE, STR_TUTORIAL_S3_BODY, -1,                  STR_TUTORIAL_ACT_OPEN},
	{STR_TUTORIAL_S4_TITLE, STR_TUTORIAL_S4_BODY, WID_TN_FAST_FORWARD, STR_TUTORIAL_ACT_SPEED},
	{STR_TUTORIAL_S5_TITLE, STR_TUTORIAL_S5_BODY, -1,                  STR_TUTORIAL_ACT_MORE},
};
static_assert(lengthof(_tutorial_steps) == TUT_STEP_END);

/* ---------------- Persistenz (tutorial.dat) ---------------- */

static bool _tut_seen = false;  ///< Wurde der Einstieg schon einmal gezeigt?
static bool _tut_loaded = false;

static std::string TutorialFilePath()
{
	return _personal_dir + "tutorial.dat";
}

static void TutorialLoad()
{
	if (_tut_loaded) return;
	_tut_loaded = true;
	std::ifstream f(TutorialFilePath(), std::ios::binary);
	if (!f.is_open()) return;
	char magic[4];
	uint8_t data[1];
	f.read(magic, 4);
	f.read(reinterpret_cast<char *>(data), sizeof(data));
	if (!f.good() || std::string_view(magic, 4) != "TUT1") return;
	_tut_seen = data[0] != 0;
}

static void TutorialSave()
{
	std::ofstream f(TutorialFilePath(), std::ios::binary | std::ios::trunc);
	if (!f.is_open()) return;
	f.write("TUT1", 4);
	uint8_t data[1] = {(uint8_t)(_tut_seen ? 1 : 0)};
	f.write(reinterpret_cast<const char *>(data), sizeof(data));
	f.close();
#ifdef __EMSCRIPTEN__
	EM_ASM(if (window["openttd_syncfs"]) openttd_syncfs());
#endif
}

/* ---------------- Erkennung erledigter Schritte ---------------- */

/** Steht schon eine eigene Station? Dann hat der Bau geklappt. */
static bool TutorialHasStation()
{
	for (const Station *st : Station::Iterate()) {
		if (st->owner == _local_company) return true;
	}
	return false;
}

/** Faehrt schon ein eigenes Fahrzeug? */
static bool TutorialHasVehicle()
{
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->owner == _local_company && v->IsPrimaryVehicle()) return true;
	}
	return false;
}

/** Blinkenden Rahmen in der Werkzeugleiste setzen oder loeschen. */
static void TutorialHighlight(WidgetID widget)
{
	Window *tb = FindWindowById(WindowClass::MainToolbar, 0);
	if (tb == nullptr) return;
	for (const TutorialStepInfo &info : _tutorial_steps) {
		if (info.highlight < 0 || info.highlight == widget) continue;
		tb->SetWidgetHighlight(info.highlight, TextColour::Invalid);
	}
	if (widget >= 0) tb->SetWidgetHighlight(widget, TextColour::White);
}

struct TutorialWindow : Window {
	TutorialStep step = TUT_WELCOME;

	TutorialWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->InitNested(number);
		this->PlaceLeft();
		this->ApplyStep();
	}

	/**
	 * Unten links, ueber der Statusleiste: dort stoert der Begleiter
	 * weder die Werkzeugleiste noch die Dialoge, die mittig aufgehen.
	 */
	void PlaceLeft()
	{
		this->left = ScaleGUITrad(8);
		this->top = std::max<int>(ScaleGUITrad(30), _screen.height - this->height - ScaleGUITrad(30));
		this->SetDirty();
	}

	const TutorialStepInfo &Info() const
	{
		return _tutorial_steps[this->step];
	}

	/** Hervorhebung und Knopfbeschriftungen an den Schritt anpassen. */
	void ApplyStep()
	{
		TutorialHighlight(this->Info().highlight);
		this->SetWidgetDisabledState(WID_TU_BACK, this->step == TUT_WELCOME);
		this->ReInit();
		this->SetDirty();
	}

	void SetStep(TutorialStep s)
	{
		if (s >= TUT_STEP_END) { this->Close(); return; }
		this->step = s;
		this->ApplyStep();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_TU_STEPS:
				return GetString(STR_TUTORIAL_STEP_OF, this->step + 1, (int)TUT_STEP_END);
			case WID_TU_ACTION:
				return GetString(this->Info().action);
			case WID_TU_NEXT:
				return GetString(this->step == TUT_DONE ? STR_TUTORIAL_FINISH : STR_TUTORIAL_SKIP);
			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_TU_PANEL) return;
		size.width = std::max<uint>(size.width, ScaleGUITrad(300));
		uint max_h = 0;
		for (const TutorialStepInfo &info : _tutorial_steps) {
			uint h = GetStringHeight(info.title, size.width) + WidgetDimensions::scaled.vsep_normal + GetStringHeight(info.body, size.width);
			max_h = std::max(max_h, h);
		}
		size.height = max_h;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_TU_PANEL) return;
		Rect ir = r;
		int y = DrawStringMultiLine(ir.left, ir.right, ir.top, ir.bottom, this->Info().title, TextColour::White);
		y += WidgetDimensions::scaled.vsep_normal;
		DrawStringMultiLine(ir.left, ir.right, y, ir.bottom, this->Info().body, TextColour::Black);
	}

	/** Der Aktionsknopf nimmt dem Einsteiger den Schritt ab. */
	void DoAction()
	{
		switch (this->step) {
			case TUT_WELCOME:
				this->SetStep(TUT_AUTOCONNECT);
				break;

			case TUT_AUTOCONNECT:
			case TUT_BUILD:
			case TUT_DONE: {
				extern void ShowAutoConnectWindowForBeginners();
				ShowAutoConnectWindowForBeginners();
				break;
			}

			case TUT_SPEED:
				/* Zwei Stufen, damit man den Unterschied sofort sieht. */
				if (_game_speed == 100) _game_speed = 400;
				SetWindowDirty(WindowClass::MainToolbar, 0);
				break;

			default:
				break;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_TU_BACK:
				if (this->step > TUT_WELCOME) this->SetStep((TutorialStep)(this->step - 1));
				break;

			case WID_TU_ACTION:
				this->DoAction();
				break;

			case WID_TU_NEXT:
				this->SetStep((TutorialStep)(this->step + 1));
				break;
		}
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		TutorialHighlight(-1);
		_tut_seen = true;
		TutorialSave();
		this->Window::Close(data);
	}

	/**
	 * Zweimal pro Sekunde pruefen, ob der Spieler den Schritt erledigt
	 * hat - dann rueckt der Begleiter von selbst weiter.
	 */
	const IntervalTimer<TimerWindow> watch = {std::chrono::milliseconds(500), [this](auto) {
		/* Die Werkzeugleiste verliert ihre Hervorhebung beim Neuaufbau. */
		TutorialHighlight(this->Info().highlight);

		switch (this->step) {
			case TUT_AUTOCONNECT:
				if (FindWindowById(WindowClass::AutoConnect, 0) != nullptr) this->SetStep(TUT_BUILD);
				break;

			case TUT_BUILD:
				if (TutorialHasStation() && TutorialHasVehicle()) this->SetStep(TUT_SPEED);
				break;

			case TUT_SPEED:
				if (_game_speed != 100) this->SetStep(TUT_DONE);
				break;

			default:
				break;
		}
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_tutorial_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_TU_CAPTION), SetStringTip(STR_TUTORIAL_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(WWT_TEXT, Colours::Invalid, WID_TU_STEPS), SetFill(1, 0),
			NWidget(WWT_EMPTY, Colours::Invalid, WID_TU_PANEL), SetFill(1, 1),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_TU_BACK), SetFill(1, 0), SetStringTip(STR_TUTORIAL_BACK, STR_TUTORIAL_BACK_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_TU_ACTION), SetFill(1, 0), SetToolTip(STR_TUTORIAL_ACTION_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_TU_NEXT), SetFill(1, 0), SetToolTip(STR_TUTORIAL_SKIP_TOOLTIP),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _tutorial_desc(
	WindowPosition::Manual, {}, 0, 0,
	WindowClass::Tutorial, WindowClass::None,
	{},
	_nested_tutorial_widgets
);

/** Gefuehrten Einstieg oeffnen (Hilfe-Menue, Konsole). */
void ShowTutorialWindow()
{
	if (_game_mode != GameMode::Normal) return;
	CloseWindowById(WindowClass::Tutorial, 0);
	AllocateWindowDescFront<TutorialWindow>(_tutorial_desc, 0);
}

/**
 * Nach einem frisch erzeugten Spiel aufgerufen: der Einstieg zeigt sich
 * genau einmal - danach nur noch auf Wunsch ueber das Hilfe-Menue.
 */
void ShowTutorialIfFirstGame()
{
	TutorialLoad();
	if (_tut_seen) return;
	ShowTutorialWindow();
}

/** Fork: Konsolenbefehl "tutorial" - Einstieg erneut zeigen. */
void TutorialReset()
{
	_tut_seen = false;
	_tut_loaded = true;
	TutorialSave();
}
