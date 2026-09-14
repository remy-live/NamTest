function(event, funcs) {
    // NAM TEST — saisie des noms de favoris depuis l'interface web.
    //
    // Le texte passe par deux ports de controle : on pose le code du caractere
    // dans web_char, puis on FAIT CHANGER web_strobe. Chaque changement vaut
    // une frappe. Aucune liaison d'evenement : mod-ui recopie l'icone apres
    // l'avoir construite et les liaisons disparaissent avec la copie ; un champ
    // texte, lui, garde ce qu'on tape sans rien demander. On le relit au
    // minuteur.
    //
    // Les champs sont cherches dans TOUTE LA PAGE et non dans event.icon : le
    // panneau de reglages n'est pas un descendant de l'icone, et c'est la
    // qu'ils vivent.

    var etat = window.__namSaisie;

    if (!etat) {
        etat = { connus: {}, file: [], strobe: 0, minuteur: null, slot: 1,
                 retourSlot: 0, retourChars: {}, retourDb: 0, autoEtat: 0,
                 envoi: function () {} };
        window.__namSaisie = etat;
    }

    if (event.type === 'change' && event.symbol === 'web_slot') {
        etat.slot = Math.round(event.value);
    }

    // --- canal de retour : le plugin renvoie un nom par seconde -----------
    if (event.type === 'change') {
        if (event.symbol === 'name_slot') {
            etat.retourSlot = Math.round(event.value);
        }

        var mc = /^n([1-7])$/.exec(event.symbol);
        if (mc) {
            if (!etat.retourChars) { etat.retourChars = {}; }
            etat.retourChars[parseInt(mc[1], 10)] = Math.round(event.value);
        }

        if (event.symbol === 'auto_db_slot') {
            etat.retourDb = event.value;
        }

        if (event.symbol === 'auto_state') {
            etat.autoEtat = Math.round(event.value);
        }

        afficherRetour();
    }

    function envoyerProchain() {
        if (etat.file.length === 0) { return; }

        var ordre = etat.file.shift();

        // UNE seule valeur porte tout : compteur, emplacement et caractere.
        // Deux ports separes ne marchaient que pour le favori 1 -- rien ne
        // garantit l'ordre d'arrivee, et l'emplacement arrivait apres.
        etat.strobe = (etat.strobe + 1) % 15;

        var paquet = (etat.strobe + 1) * 4096 + ordre.slot * 256 + ordre.code;

        // on passe par etat.envoi, refait a chaque demarrage : ecrire par les
        // funcs capturees au premier appel revient a parler a une interface
        // que mod-ui a deja remplacee
        etat.envoi('web_char', paquet);
    }

    function afficherRetour() {
        var slot = etat.retourSlot;

        if (!slot || !etat.retourChars) { return; }

        var nom = '';

        for (var k = 1; k <= 7; k++) {
            var c = etat.retourChars[k] || 0;
            if (c >= 32 && c < 127) { nom += String.fromCharCode(c); }
        }

        // remplir le champ SEULEMENT s'il est vide : ne jamais ecraser ce que
        // l'utilisateur est en train de taper
        var champ = jQuery('.nam-typed-input[data-slot="' + slot + '"]');

        if (champ.length && !champ.val()) {
            champ.val(nom);
            etat.connus[String(slot)] = nom;
        }

        // la correction mesuree, a cote du champ
        var etiq = jQuery('.nam-auto[data-slot="' + slot + '"]');

        if (etiq.length) {
            var db = etat.retourDb || 0;
            etiq.text((db >= 0 ? '+' : '') + db.toFixed(2) + ' dB');
            etiq.toggleClass('nam-auto-vif', Math.abs(db) > 0.01);
        }
    }

    function surveiller() {
        jQuery('.nam-typed-input').each(function () {
            var champ = jQuery(this);
            var attr = champ.attr('data-slot');
            var slot = (attr === 'actif') ? etat.slot : parseInt(attr, 10);
            var texte = champ.val() || '';
            var cle = attr;
            var avant = etat.connus[cle];

            if (avant === undefined) { etat.connus[cle] = texte; return; }
            if (texte === avant) { return; }

            etat.connus[cle] = texte;

            // on renvoie le mot entier : effacer puis retaper
            etat.file.push({ slot: slot, code: 1 });

            for (var i = 0; i < texte.length && i < 20; i++) {
                var c = texte.charCodeAt(i);
                if (c >= 32 && c < 127) { etat.file.push({ slot: slot, code: c }); }
            }
        });

        envoyerProchain();
    }

    if (event.type === 'start') {
        etat.envoi = function (symbole, valeur) {
            funcs.set_port_value(symbole, valeur);
        };

        // temoin visible : si le script tourne, les champs l'annoncent
        jQuery('.nam-typed-input').attr('placeholder', 'script actif - taper ici');

        if (etat.minuteur) { clearInterval(etat.minuteur); }
        etat.minuteur = setInterval(surveiller, 120);
    }
}
