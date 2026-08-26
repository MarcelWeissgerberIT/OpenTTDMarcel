/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file forkunlock.cpp Welche Zusatzfunktionen offenstehen (Fork-Feature).
 *
 * Das Spiel selbst bleibt frei - es ist OpenTTD, und daran aendert sich
 * nichts. Die Zusatzfunktionen dieser Ausgabe (Pixel-Studio, Immobilien,
 * Linien-Manager, Ausbau im Betrieb und so fort) gehoeren zum Kauf.
 *
 * Ehrlich gesagt: das hier ist eine Schranke, kein Schloss. Das Spiel
 * laeuft im Browser, sein Quelltext ist offen - wer will, umgeht das.
 * Der Schutz, der wirklich traegt, sitzt auf dem Server: Cloud-
 * Speicherstaende gibt es nur mit Kauf, und darueber entscheidet nicht
 * dieser Code, sondern der Worker.
 *
 * Der Stand kommt aus der Webseite (shell.html fragt beim Server nach)
 * und wird hier hinterlegt. Ohne Antwort - etwa offline - bleibt es beim
 * zuletzt bekannten Stand, damit ein Kaeufer nicht ploetzlich vor
 * verschlossenen Tueren steht.
 */

#include "stdafx.h"
#include "forkunlock.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "error.h"
#include "debug.h"
#include "table/strings.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "safeguards.h"

/** Ist die Vollversion freigeschaltet? */
static bool _fork_unlocked = false;
/** Wurde ueberhaupt schon nachgefragt? */
static bool _fork_known = false;

/**
 * Fork: Den Freischalt-Stand setzen (wird aus der Webseite gerufen).
 * @param unlocked 1 = gekauft, 0 = nicht gekauft.
 */
extern "C" void CDECL em_openttd_set_unlocked(int unlocked)
{
	bool now = unlocked != 0;
	if (_fork_known && now == _fork_unlocked) return;
	_fork_unlocked = now;
	_fork_known = true;
	Debug(misc, 1, "Marcel Edition: {}", now ? "freigeschaltet" : "nicht freigeschaltet");
	/* Alle Fenster neu zeichnen - Knoepfe wechseln ihren Zustand. */
	InvalidateWindowClassesData(WindowClass::SelectGame, 0);
	MarkWholeScreenDirty();
}

/**
 * Fork: Steht die Vollversion zur Verfuegung?
 *
 * Ausserhalb des Browsers (eigener Bau, Server) gibt es niemanden, der
 * den Stand liefern koennte - dort ist alles offen. Der Verkauf laeuft
 * ueber die Browserfassung.
 */
bool ForkUnlocked()
{
#ifdef __EMSCRIPTEN__
	return _fork_unlocked;
#else
	return true;
#endif
}

/**
 * Fork: Hinweis zeigen, dass diese Funktion zum Kauf gehoert.
 *
 * Bewusst kein "verboten", sondern ein Angebot: was es gibt und wo.
 */
void ForkShowLockedHint()
{
	ShowErrorMessage(GetEncodedString(STR_FORKLOCK_HINT), {}, WarningLevel::Info);
#ifdef __EMSCRIPTEN__
	/* Die Werbe-Fahne in der Seite hervorheben und das Angebot oeffnen. */
	EM_ASM(if (window["openttd_open_buy"]) openttd_open_buy(););
#endif
}
