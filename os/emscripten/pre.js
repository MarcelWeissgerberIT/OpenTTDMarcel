/* Fork: Das Spiel laeuft auch weiter, wenn der Tab nicht aktiv ist.
 * Browser drosseln requestAnimationFrame und Seiten-Timer in inaktiven
 * Tabs; ein Timer in einem Web-Worker wird nicht gedrosselt und stoesst
 * die Hauptschleife dann von aussen an (openttd_background_tick). */
(function () {
    if (typeof Worker === 'undefined' || typeof document === 'undefined') return;
    try {
        var src = 'setInterval(function(){postMessage(0);},33);';
        var w = new Worker(URL.createObjectURL(new Blob([src], {type: 'text/javascript'})));
        w.onmessage = function () {
            if (!document.hidden) return; /* Tab aktiv: RequestAnimationFrame uebernimmt. */
            try {
                if (Module['calledRun'] && Module['_openttd_background_tick']) Module['_openttd_background_tick']();
            } catch (e) { /* Spiel (noch) nicht bereit. */ }
        };
    } catch (e) { /* Ohne Worker bleibt das bisherige Verhalten. */ }
})();

/* Sound läuft über SDL-Audio (OpenSFX ist gebündelt); Musik bleibt aus. */
Module.arguments.push('-mnull', '-vsdl');
Module['websocket'] = { url: function(host, port, proto) {
    /* openttd.org hosts a WebSocket proxy for the content service. */
    if (host == "content.openttd.org" && port == 3978 && proto == "tcp") {
        return "wss://bananas-server.openttd.org/";
    }

    /* Everything else just tries to make a default WebSocket connection.
     * If you run your own server you can setup your own WebSocket proxy in
     * front of it and let people connect to your server via the proxy. You
     * are best to add another "if" statement as above for this. */

    if (location.protocol === 'https:') {
        /* Insecure WebSockets do not work over HTTPS, so we force
         * secure ones. */
        return 'wss://';
    } else {
        /* Use the default provided by Emscripten. */
        return null;
    }
} };

Module.preRun.push(function() {
    /* Sprache des Browsers an OpenTTD durchreichen (de-DE -> de_DE.UTF-8);
     * fehlt die passende .lng im Bundle, faellt das Spiel auf Englisch zurueck. */
    if (typeof ENV !== 'undefined' && typeof navigator !== 'undefined' && navigator.language) {
        ENV.LANG = navigator.language.replace('-', '_') + '.UTF-8';
    }
    personal_dir = '/home/web_user/.openttd';
    content_download_dir = personal_dir + '/content_download'

    /* Because of the "-c" above, all user-data is stored in /user_data. */
    FS.mkdir(personal_dir);
    FS.mount(IDBFS, {}, personal_dir);

    /* ---------- Cloud-Sync (Phase 4): Saves + openttd.cfg in R2/D1 ---------- */

    var CLOUD_API = 'https://openttd-api.marcelweissgerber81.workers.dev';
    var save_dir = personal_dir + '/save';

    function cloudToken() {
        try { return localStorage.getItem('ottd_token'); } catch (e) { return null; }
    }
    /* Status geht an das HTML-Panel UND an die Cloud-Zeile im
     * Speichern/Laden-Dialog des Spiels (em_openttd_cloud_status). */
    var cloud_last_status = '';
    var cloud_c_ready = false;
    function cloudStatusToGame() {
        if (!cloud_c_ready || !cloud_last_status) return;
        try { Module.ccall('em_openttd_cloud_status', null, ['string'], [cloud_last_status]); } catch (e) {}
    }
    function cloudStatus(text) {
        cloud_last_status = text;
        if (Module.onCloudStatus) Module.onCloudStatus(text);
        cloudStatusToGame();
    }
    function cloudPushedMap() {
        try { return JSON.parse(localStorage.getItem('ottd_cloud_pushed') || '{}'); } catch (e) { return {}; }
    }
    function cloudPushedSave(map) {
        try { localStorage.setItem('ottd_cloud_pushed', JSON.stringify(map)); } catch (e) {}
    }

    /* Beim Start: Cloud-Spielstaende holen, die lokal fehlen oder deutlich
     * neuer sind (1 Minute Puffer gegen Uhren-Schieflage). Fehler blockieren
     * den Spielstart nie. */
    function cloudPull(done) {
        var token = cloudToken();
        if (!token) { done(); return; }
        cloudStatus('Cloud: prüfe Spielstände...');
        var finished = false;
        var guard = setTimeout(function() { if (!finished) { finished = true; done(); } }, 20000);
        function finish() {
            if (!finished) { finished = true; clearTimeout(guard); }
            else return;
            FS.syncfs(false, function() { done(); });
        }
        fetch(CLOUD_API + '/api/saves', { headers: { 'Authorization': 'Bearer ' + token } })
            .then(function(r) { return r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)); })
            .then(function(d) {
                var saves = (d && d.saves) || [];
                var jobs = saves.filter(function(s) {
                    try {
                        var st = FS.stat(save_dir + '/' + s.name);
                        return st.mtime.getTime() < s.updated_at - 60000;
                    } catch (e) { return true; /* lokal nicht vorhanden */ }
                });
                try { FS.mkdirTree(save_dir); } catch (e) {}
                var idx = 0;
                function next() {
                    if (idx >= jobs.length) {
                        /* Einstellungen nur uebernehmen, wenn lokal noch keine existieren
                         * (frischer Browser) - lokale Aenderungen gewinnen sonst immer. */
                        var haveCfg = true;
                        try { FS.stat(personal_dir + '/openttd.cfg'); } catch (e) { haveCfg = false; }
                        if (haveCfg) { cloudStatus(jobs.length ? 'Cloud: ' + jobs.length + ' Spielstand(e) geladen' : 'Cloud: synchron'); finish(); return; }
                        fetch(CLOUD_API + '/api/settings', { headers: { 'Authorization': 'Bearer ' + token } })
                            .then(function(r) { return r.ok ? r.json() : null; })
                            .then(function(s) {
                                if (s && s.json) FS.writeFile(personal_dir + '/openttd.cfg', s.json);
                                cloudStatus('Cloud: Spielstände & Einstellungen geladen');
                                finish();
                            })
                            .catch(function() { finish(); });
                        return;
                    }
                    var s = jobs[idx++];
                    cloudStatus('Cloud: lade "' + s.name + '"...');
                    fetch(CLOUD_API + '/api/saves/download?slot=' + s.slot, { headers: { 'Authorization': 'Bearer ' + token } })
                        .then(function(r) { return r.ok ? r.arrayBuffer() : Promise.reject(new Error('HTTP ' + r.status)); })
                        .then(function(buf) {
                            FS.writeFile(save_dir + '/' + s.name, new Uint8Array(buf));
                            next();
                        })
                        .catch(function() { next(); });
                }
                next();
            })
            .catch(function(e) { cloudStatus('Cloud: nicht erreichbar'); finish(); });
    }

    /* Nach jedem Speichern: die (bis zu 3) neuesten Saves hochladen, die sich
     * seit dem letzten Push geaendert haben; dazu die openttd.cfg. */
    var cloud_push_timer = null;
    function cloudPushSoon() {
        if (cloud_push_timer) clearTimeout(cloud_push_timer);
        cloud_push_timer = setTimeout(cloudPush, 3000);
    }
    function cloudPush() {
        var token = cloudToken();
        if (!token) return;
        var names = [];
        try {
            names = FS.readdir(save_dir).filter(function(n) { return /\.sav$/i.test(n); });
        } catch (e) { return; }
        var pushed = cloudPushedMap();
        var files = names.map(function(n) {
            var m = 0;
            try { m = FS.stat(save_dir + '/' + n).mtime.getTime(); } catch (e) {}
            return { n: n, m: m };
        }).sort(function(a, b) { return b.m - a.m; }).slice(0, 3)
          .filter(function(f) { return (pushed[f.n] || 0) < f.m; });
        var idx = 0;
        function next() {
            if (idx >= files.length) {
                /* openttd.cfg mitschicken, wenn geaendert. */
                var cfgTime = 0;
                try { cfgTime = FS.stat(personal_dir + '/openttd.cfg').mtime.getTime(); } catch (e) {}
                if (cfgTime > (pushed['openttd.cfg'] || 0)) {
                    var cfg = '';
                    try { cfg = new TextDecoder().decode(FS.readFile(personal_dir + '/openttd.cfg')); } catch (e) {}
                    if (cfg) {
                        fetch(CLOUD_API + '/api/settings', {
                            method: 'PUT',
                            headers: { 'Authorization': 'Bearer ' + token },
                            body: cfg,
                        }).then(function(r) {
                            if (r.ok) { pushed['openttd.cfg'] = cfgTime; cloudPushedSave(pushed); }
                        }).catch(function() {});
                    }
                }
                cloudStatus(files.length
                    ? 'Cloud: gesichert (' + new Date().toLocaleTimeString() + ')'
                    : 'Cloud: synchron');
                return;
            }
            var f = files[idx++];
            var data;
            try { data = FS.readFile(save_dir + '/' + f.n); } catch (e) { next(); return; }
            cloudStatus('Cloud: sichere "' + f.n + '"...');
            fetch(CLOUD_API + '/api/saves/upload?name=' + encodeURIComponent(f.n), {
                method: 'PUT',
                headers: { 'Authorization': 'Bearer ' + token },
                body: data,
            }).then(function(r) {
                if (r.ok) { pushed[f.n] = f.m; cloudPushedSave(pushed); }
                next();
            }).catch(function() { cloudStatus('Cloud: Upload fehlgeschlagen'); next(); });
        }
        next();
    }

    Module.addRunDependency('syncfs');
    FS.syncfs(true, function (err) {
        cloudPull(function() {
            Module.removeRunDependency('syncfs');
        });
    });

    /* Sobald die Laufzeit steht: aktuellen Cloud-Zustand ins Spiel melden. */
    if (!Module.postRun) Module.postRun = [];
    Module.postRun.push(function() {
        cloud_c_ready = true;
        if (!cloud_last_status) {
            var mail = '';
            try { mail = localStorage.getItem('ottd_email') || ''; } catch (e) {}
            cloud_last_status = cloudToken()
                ? 'Cloud: angemeldet' + (mail ? ' als ' + mail : '') + ' - sichert automatisch'
                : 'Cloud: nicht angemeldet';
        }
        cloudStatusToGame();
    });

    /* Vom Spiel aufgerufen (Knopf "Cloud-Sicherung" im Speichern-Dialog). */
    window.openttd_cloud_sync = function() {
        if (!cloudToken()) {
            cloudStatus('Cloud: nicht angemeldet - bitte über "Konto..." anmelden');
            return;
        }
        cloudStatus('Cloud: sichere...');
        FS.syncfs(false, function() { cloudPush(); });
    };

    /* ---------- Pixel-Studio: Zwischenablage (Copy/Paste) ---------- */

    /* Aktuelle Ansicht als PNG in die Zwischenablage. */
    window.openttd_ps_copy = function(ptr, w, h) {
        try {
            var data = new Uint8ClampedArray(HEAPU8.subarray(ptr, ptr + w * h * 4));
            var cnv = document.createElement('canvas');
            cnv.width = w;
            cnv.height = h;
            cnv.getContext('2d').putImageData(new ImageData(data, w, h), 0, 0);
            cnv.toBlob(function(blob) {
                if (!blob || !navigator.clipboard || !navigator.clipboard.write) return;
                navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]).catch(function() {});
            });
        } catch (e) {}
    };

    /* Bild aus der Zwischenablage lesen, auf Sprite-Groesse skalieren und
     * als RGBA an das Spiel geben (dort: Palette treffen + Undo). */
    window.openttd_ps_paste = function(w, h) {
        if (!navigator.clipboard || !navigator.clipboard.read) return;
        navigator.clipboard.read().then(function(items) {
            for (var i = 0; i < items.length; i++) {
                var item = items[i];
                var type = null;
                for (var j = 0; j < item.types.length; j++) {
                    if (item.types[j].indexOf('image/') === 0) { type = item.types[j]; break; }
                }
                if (!type) continue;
                item.getType(type).then(function(blob) {
                    return createImageBitmap(blob);
                }).then(function(bmp) {
                    var cnv = document.createElement('canvas');
                    cnv.width = w;
                    cnv.height = h;
                    var ctx = cnv.getContext('2d');
                    ctx.imageSmoothingEnabled = false;
                    ctx.drawImage(bmp, 0, 0, w, h);
                    var data = ctx.getImageData(0, 0, w, h).data;
                    var buf = Module._malloc(data.length);
                    HEAPU8.set(data, buf);
                    Module.ccall('em_openttd_ps_paste_data', null, ['number', 'number', 'number'], [buf, w, h]);
                    Module._free(buf);
                }).catch(function() {});
                return;
            }
        }).catch(function() {});
    };

    window.openttd_syncfs_shown_warning = false;
    window.openttd_syncfs = function(callback) {
        /* Copy the virtual FS to the persistent storage. */
        FS.syncfs(false, function (err) {
            /* On first time, warn the user about the volatile behaviour of
             * persistent storage. */
            if (!window.openttd_syncfs_shown_warning) {
                window.openttd_syncfs_shown_warning = true;
                Module.onWarningFs();
            }

            /* Nach jedem Speichern zusaetzlich in die Cloud sichern. */
            cloudPushSoon();

            if (callback) callback();
        });
    }

    window.openttd_exit = function() {
        window.openttd_syncfs(Module.onExit);
    }

    window.openttd_abort = function() {
        window.openttd_syncfs(Module.onAbort);
    }

    window.openttd_bootstrap = function(current, total) {
        Module.onBootstrap(current, total);
    }

    window.openttd_bootstrap_failed = function() {
        Module.onBootstrapFailed();
    }

    window.openttd_bootstrap_reload = function() {
        window.openttd_syncfs(function() {
            Module.onBootstrapReload();
            setTimeout(function() {
                location.reload();
            }, 1000);
        });
    }

    window.openttd_server_list = function() {
        add_server = Module.cwrap("em_openttd_add_server", null, ["string"]);

        /* Add servers that support WebSocket here. Examples:
         *  add_server("localhost");
         *  add_server("localhost:3979");
         *  add_server("127.0.0.1:3979");
         *  add_server("[::1]:3979");
         */
    }

    var leftButtonDown = false;
    document.addEventListener("mousedown", e => {
        if (e.button == 0) {
            leftButtonDown = true;
        }
    });
    document.addEventListener("mouseup", e => {
        if (e.button == 0) {
            leftButtonDown = false;
        }
    });
    window.openttd_open_url = function(url, url_len) {
        const url_string = UTF8ToString(url, url_len);
        function openWindow() {
            document.removeEventListener("mouseup", openWindow);
            window.open(url_string, '_blank');
        }
        /* Trying to open the URL while the mouse is down results in the button getting stuck, so wait for the
         * mouse to be released before opening it. However, when OpenTTD is lagging, the mouse can get released
         * before the button click even registers, so check for that, and open the URL immediately if that's the
         * case. */
        if (leftButtonDown) {
            document.addEventListener("mouseup", openWindow);
        } else {
            openWindow();
        }
    }

    /* https://github.com/emscripten-core/emscripten/pull/12995 implements this
    * properly. Till that time, we use a polyfill. */
   SOCKFS.websocket_sock_ops.createPeer_ = SOCKFS.websocket_sock_ops.createPeer;
   SOCKFS.websocket_sock_ops.createPeer = function(sock, addr, port)
   {
       let func = Module['websocket']['url'];
       Module['websocket']['url'] = func(addr, port, (sock.type == 2) ? 'udp' : 'tcp');
       let ret = SOCKFS.websocket_sock_ops.createPeer_(sock, addr, port);
       Module['websocket']['url'] = func;
       return ret;
   }
});
