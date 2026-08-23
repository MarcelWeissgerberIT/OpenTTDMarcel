/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file shareworld.cpp Welt teilen (Fork-Feature).
 *
 * Speichert die laufende Partie, laedt sie in die Cloud und gibt eine
 * kurze Adresse zurueck. Wer sie oeffnet, spielt die Welt sofort im
 * Browser weiter - ohne Konto, ohne Installation. Der Link-Dialog
 * selbst liegt in der Seite (os/emscripten/pre.js), weil sich Text dort
 * markieren und kopieren laesst.
 */

#include "stdafx.h"
#include "openttd.h"
#include "error.h"
#include "strings_func.h"
#include "company_base.h"
#include "company_func.h"
#include "saveload/saveload.h"
#include "timer/timer_game_calendar.h"

#include "table/strings.h"

#ifdef __EMSCRIPTEN__
#	include <emscripten.h>
#endif

#include "safeguards.h"

/** Dateiname der Zwischenkopie im Autosave-Ordner. */
static const char *SHARE_FILE = "geteilte_welt.sav";

/**
 * Fork: laufende Partie teilen. Die Datei geht in den Autosave-Ordner,
 * damit sie die Spielstandsliste nicht zumuellt; die Seite laedt sie
 * von dort hoch und zeigt den fertigen Link.
 */
void ShareCurrentWorld()
{
	if (_game_mode != GameMode::Normal) return;

#ifdef __EMSCRIPTEN__
	if (SaveOrLoad(SHARE_FILE, SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::Autosave, false) != SaveLoadResult::Ok) {
		ShowErrorMessage(GetEncodedString(STR_SHARE_ERR_SAVE), {}, WarningLevel::Error);
		return;
	}

	/* Sprechender Name fuer die Vorschau: Firma und Jahr. */
	std::string title = Company::IsValidID(_local_company)
			? GetString(STR_SHARE_WORLD_NAME, _local_company, TimerGameCalendar::year)
			: GetString(STR_SHARE_WORLD_NAME_ANON, TimerGameCalendar::year);

	EM_ASM({
		if (window["openttd_share_world"]) window["openttd_share_world"](UTF8ToString($0), UTF8ToString($1));
	}, SHARE_FILE, title.c_str());
#else
	ShowErrorMessage(GetEncodedString(STR_SHARE_ERR_BROWSER_ONLY), {}, WarningLevel::Info);
#endif
}
