/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file newsopt_gui.cpp Meldungen einstellen (Fork-Feature).
 *
 * Ein kompaktes Fenster im Zeitungs-Menue: alle Meldungen mit einem
 * Klick abschalten - oder je Kategorie waehlen, ob sie gar nicht,
 * als Laufband-Kurzmeldung oder als grosse Zeitung erscheint.
 * Dieselben Einstellungen stecken auch tief in den Spieleinstellungen;
 * hier sind sie nur endlich auffindbar.
 */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "settings_type.h"
#include "zoom_func.h"
#include "widgets/newsopt_widget.h"
#include "table/strings.h"

#include "safeguards.h"

/** Eine einstellbare Meldungs-Kategorie. */
struct NewsOptRow {
	uint8_t NewsSettings::*field; ///< Feld in _settings_client.news_display.
	StringID name;                ///< Anzeigename.
};

static const NewsOptRow _newsopt_rows[] = {
	{&NewsSettings::arrival_player,    STR_NEWSOPT_ARRIVAL_PLAYER},
	{&NewsSettings::arrival_other,     STR_NEWSOPT_ARRIVAL_OTHER},
	{&NewsSettings::accident,          STR_NEWSOPT_ACCIDENT},
	{&NewsSettings::accident_other,    STR_NEWSOPT_ACCIDENT_OTHER},
	{&NewsSettings::company_info,      STR_NEWSOPT_COMPANY_INFO},
	{&NewsSettings::open,              STR_NEWSOPT_OPEN},
	{&NewsSettings::close,             STR_NEWSOPT_CLOSE},
	{&NewsSettings::economy,           STR_NEWSOPT_ECONOMY},
	{&NewsSettings::production_player, STR_NEWSOPT_PRODUCTION_PLAYER},
	{&NewsSettings::production_other,  STR_NEWSOPT_PRODUCTION_OTHER},
	{&NewsSettings::production_nobody, STR_NEWSOPT_PRODUCTION_NOBODY},
	{&NewsSettings::advice,            STR_NEWSOPT_ADVICE},
	{&NewsSettings::new_vehicles,      STR_NEWSOPT_NEW_VEHICLES},
	{&NewsSettings::acceptance,        STR_NEWSOPT_ACCEPTANCE},
	{&NewsSettings::subsidies,         STR_NEWSOPT_SUBSIDIES},
	{&NewsSettings::general,           STR_NEWSOPT_GENERAL},
};

struct NewsOptWindow : Window {
	uint line_height = 0;

	NewsOptWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->InitNested(number);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_NO_PANEL) return;
		this->line_height = GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.framerect.Vertical();
		size.height = (uint)std::size(_newsopt_rows) * this->line_height + WidgetDimensions::scaled.framerect.Vertical();
		size.width = std::max<uint>(size.width, ScaleGUITrad(280));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_NO_PANEL) return;
		Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
		int y = ir.top;
		for (const NewsOptRow &row : _newsopt_rows) {
			uint8_t val = _settings_client.news_display.*(row.field);
			static const StringID levels[] = {STR_NEWSOPT_LEVEL_OFF, STR_NEWSOPT_LEVEL_SUMMARY, STR_NEWSOPT_LEVEL_FULL};
			DrawString(ir.left, ir.right, y, GetString(row.name), TextColour::Black);
			DrawString(ir.left, ir.right, y, GetString(levels[std::min<uint8_t>(val, 2)]),
					val == 0 ? TextColour::Grey : (val == 1 ? TextColour::DarkGreen : TextColour::Orange),
					AlignmentH::End);
			y += this->line_height;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_NO_ALL_OFF:
			case WID_NO_ALL_SUM:
			case WID_NO_ALL_FULL: {
				uint8_t val = widget == WID_NO_ALL_OFF ? 0 : (widget == WID_NO_ALL_SUM ? 1 : 2);
				for (const NewsOptRow &row : _newsopt_rows) {
					_settings_client.news_display.*(row.field) = val;
				}
				this->SetDirty();
				break;
			}

			case WID_NO_PANEL: {
				Rect r = this->GetWidget<NWidgetBase>(WID_NO_PANEL)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
				if (pt.y < r.top || this->line_height == 0) break;
				uint index = (pt.y - r.top) / this->line_height;
				if (index >= std::size(_newsopt_rows)) break;
				uint8_t &val = _settings_client.news_display.*(_newsopt_rows[index].field);
				val = (val + 1) % 3;
				this->SetDirty();
				break;
			}
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_newsopt_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Brown),
		NWidget(WWT_CAPTION, Colours::Brown, WID_NO_CAPTION), SetStringTip(STR_NEWSOPT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::Brown),
		NWidget(WWT_STICKYBOX, Colours::Brown),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Brown),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_NO_ALL_OFF), SetFill(1, 0), SetMinimalSize(80, 14), SetStringTip(STR_NEWSOPT_ALL_OFF, STR_NEWSOPT_ALL_OFF_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_NO_ALL_SUM), SetFill(1, 0), SetMinimalSize(80, 14), SetStringTip(STR_NEWSOPT_ALL_SUM, STR_NEWSOPT_ALL_SUM_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Yellow, WID_NO_ALL_FULL), SetFill(1, 0), SetMinimalSize(80, 14), SetStringTip(STR_NEWSOPT_ALL_FULL, STR_NEWSOPT_ALL_FULL_TOOLTIP),
			EndContainer(),
			NWidget(WWT_PANEL, Colours::Brown, WID_NO_PANEL), SetFill(1, 1), SetToolTip(STR_NEWSOPT_ROW_TOOLTIP), EndContainer(),
			NWidget(WWT_TEXT, Colours::Invalid), SetFill(1, 0), SetStringTip(STR_NEWSOPT_FOOTNOTE),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _newsopt_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::NewsOptions, WindowClass::None,
	{},
	_nested_newsopt_widgets
);

/** Fork: Fenster "Meldungen einstellen" oeffnen. */
void ShowNewsOptionsWindow()
{
	AllocateWindowDescFront<NewsOptWindow>(_newsopt_desc, 0);
}
