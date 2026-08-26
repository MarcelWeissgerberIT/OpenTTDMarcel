/*
 * Zweisprachigkeit fuer die Konto-Seiten.
 *
 * Dieselbe Quelle wie beim Spiel: die Sprache des Browsers. Wer sie von
 * Hand umstellt, behaelt seine Wahl - sie liegt im selben Speicher wie
 * die des Spiels ("ottdm_lang"), damit Seite und Spiel zusammenpassen.
 *
 * Texte stehen nicht im HTML verstreut, sondern hier an einer Stelle.
 * Ein Element bekommt data-t="schluessel" fuer seinen Text,
 * data-t-ph="schluessel" fuer den Platzhalter eines Eingabefelds und
 * data-t-title="schluessel" fuer den Fenstertitel.
 */
(function () {
	var TEXTS = {
		de: {
			/* Startseite */
			home_title: 'OpenTTD Marcel',
			home_intro: 'Die Eisenbahn-Wirtschaftssimulation im Browser — mit Candyland-Welt, Auto-Verbindung und deutscher Oberfläche.',
			home_play: 'Jetzt gratis spielen',
			home_login: 'Anmelden',
			home_register: 'Konto erstellen',
			home_hint: 'Mit Konto werden Spielstände automatisch in der Cloud gesichert.',
			/* Anmelden */
			login_title: 'Anmelden',
			login_page: 'Anmelden – OpenTTD Marcel',
			email: 'E-Mail',
			password: 'Passwort',
			login_submit: 'Anmelden',
			login_working: 'Anmelden…',
			login_noaccount: 'Noch kein Konto?',
			login_toregister: 'Registrieren',
			back: 'Zurück',
			/* Registrieren */
			reg_title: 'Konto erstellen',
			reg_page: 'Konto erstellen – OpenTTD Marcel',
			reg_submit: 'Konto erstellen',
			reg_working: 'Erstelle Konto…',
			reg_haveaccount: 'Schon ein Konto?',
			reg_tologin: 'Anmelden',
			reg_pwhint: 'Mindestens 8 Zeichen.',
			/* Konto */
			acc_title: 'Dein Konto',
			acc_page: 'Konto – OpenTTD Marcel',
			acc_loading: 'Lade…',
			acc_signedin: 'Angemeldet als',
			acc_cloud_on: 'Cloud-Spielstände sind freigeschaltet.',
			acc_cloud_off: 'Cloud-Spielstände gehören zur Marcel Edition.',
			acc_buy: 'Marcel Edition holen – 39 €',
			acc_play: 'Zum Spiel',
			acc_logout: 'Abmelden',
			acc_admin: 'Kauf-Verwaltung',
			acc_saves: 'Cloud-Spielstände',
			acc_nosaves: 'Noch keine Spielstände in der Cloud. Im Spiel unten rechts auf "Cloud" klicken und anmelden – dann wird jeder Spielstand automatisch gesichert.',
			acc_slot: 'Platz',
			acc_home: 'Zurück zur Startseite',
			pw_min: 'Passwort (mind. 8 Zeichen)',
			/* gemeinsam */
			err_generic: 'Fehler.',
			err_network: 'Netzwerkfehler.',
		},
		en: {
			home_title: 'OpenTTD Marcel',
			home_intro: 'The railway business simulation in your browser — with a candyland world, auto-connect and a friendly interface.',
			home_play: 'Play for free',
			home_login: 'Sign in',
			home_register: 'Create account',
			home_hint: 'With an account your saves are backed up to the cloud automatically.',
			login_title: 'Sign in',
			login_page: 'Sign in – OpenTTD Marcel',
			email: 'Email',
			password: 'Password',
			login_submit: 'Sign in',
			login_working: 'Signing in…',
			login_noaccount: 'No account yet?',
			login_toregister: 'Create one',
			back: 'Back',
			reg_title: 'Create account',
			reg_page: 'Create account – OpenTTD Marcel',
			reg_submit: 'Create account',
			reg_working: 'Creating account…',
			reg_haveaccount: 'Already have an account?',
			reg_tologin: 'Sign in',
			reg_pwhint: 'At least 8 characters.',
			acc_title: 'Your account',
			acc_page: 'Account – OpenTTD Marcel',
			acc_loading: 'Loading…',
			acc_signedin: 'Signed in as',
			acc_cloud_on: 'Cloud saves are unlocked.',
			acc_cloud_off: 'Cloud saves come with the Marcel Edition.',
			acc_buy: 'Get the Marcel Edition – 39 €',
			acc_play: 'To the game',
			acc_logout: 'Sign out',
			acc_admin: 'Purchase admin',
			acc_saves: 'Cloud saves',
			acc_nosaves: 'No cloud saves yet. In the game, click "Cloud" at the bottom right and sign in – every save is then backed up automatically.',
			acc_slot: 'Slot',
			acc_home: 'Back to start page',
			pw_min: 'Password (at least 8 characters)',
			err_generic: 'Something went wrong.',
			err_network: 'Network error.',
		},
	};

	function pick() {
		var q = new URLSearchParams(location.search).get('lang');
		if (q === 'de' || q === 'en') return q;
		try {
			var saved = localStorage.getItem('ottdm_lang');
			if (saved === 'de' || saved === 'en') return saved;
		} catch (e) {}
		return (navigator.language || 'de').toLowerCase().indexOf('de') === 0 ? 'de' : 'en';
	}

	var lang = pick();
	var T = TEXTS[lang];

	window.OTTD_T = function (key) { return T[key] || key; };
	window.OTTD_LANG = lang;

	function apply() {
		document.documentElement.lang = lang;
		document.querySelectorAll('[data-t]').forEach(function (el) {
			el.textContent = T[el.dataset.t] || el.dataset.t;
		});
		document.querySelectorAll('[data-t-ph]').forEach(function (el) {
			el.placeholder = T[el.dataset.tPh] || '';
		});
		var t = document.querySelector('[data-t-title]');
		if (t) document.title = T[t.dataset.tTitle] || document.title;
		var sw = document.getElementById('langswitch');
		if (sw) {
			sw.innerHTML = '';
			['de', 'en'].forEach(function (l) {
				var a = document.createElement('a');
				a.href = '?lang=' + l;
				a.textContent = l.toUpperCase();
				a.style.cssText = 'color:#ffe9b0;margin:0 4px;text-decoration:' + (l === lang ? 'underline' : 'none');
				a.addEventListener('click', function () { try { localStorage.setItem('ottdm_lang', l); } catch (e) {} });
				sw.appendChild(a);
			});
		}
	}

	if (document.readyState === 'loading') {
		document.addEventListener('DOMContentLoaded', apply);
	} else {
		apply();
	}
})();
