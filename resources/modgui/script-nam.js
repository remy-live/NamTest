function(event, funcs) {
    // NAM TEST — la correction trouvee par l'auto-gain, favori par favori.
    //
    // Un port de SORTIE ne se lit pas dans un gabarit : le plugin fait donc
    // tourner un favori par seconde. `auto_db_slot` porte la correction,
    // `db_slot` le numero du favori qu'elle concerne — et il arrive un dixieme
    // de seconde APRES elle, ce qui rend l'ordre sur : c'est sur lui qu'on
    // dessine.
    //
    // Les NOMS ne passent plus par ici. Ils sont choisis dans une liste
    // deroulante, c'est-a-dire dans un port de controle dont mod-ui s'occupe
    // seul et que l'hote range avec la pedalboard. La saisie libre a ete
    // retiree : sur l'image Starless, une chaine ne redescend pas jusqu'au
    // plugin, et il fallait la remonter caractere par caractere.
    //
    // Les cellules sont cherchees dans TOUTE la page, et non dans event.icon :
    // le panneau de reglages n'est pas un descendant de l'icone, et c'est la
    // qu'elles vivent.

    var etat = window.__namMesure;

    if (!etat) {
        etat = { db: 0 };
        window.__namMesure = etat;
    }

    if (event.type !== 'change') { return; }

    if (event.symbol === 'auto_db_slot') {
        etat.db = event.value;
        return;
    }

    if (event.symbol !== 'db_slot') { return; }

    var slot = Math.round(event.value);

    if (!slot) { return; }

    var etiq = jQuery('.nam-auto[data-slot="' + slot + '"]');

    if (!etiq.length) { return; }

    var db = etat.db || 0;

    etiq.text((db >= 0 ? '+' : '') + db.toFixed(2) + ' dB');
    etiq.toggleClass('nam-auto-vif', Math.abs(db) > 0.01);
}
