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
    function cloudStatus(text) {
        if (Module.onCloudStatus) Module.onCloudStatus(text);
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
                if (files.length) cloudStatus('Cloud: gesichert (' + new Date().toLocaleTimeString() + ')');
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
