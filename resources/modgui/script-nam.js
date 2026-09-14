function(event, funcs) {
    // NAM TEST — the correction the auto-gain found, favorite by favorite.
    //
    // A template cannot read an OUTPUT port, so the plugin rotates one
    // favorite per second. `auto_db_slot` carries the correction, `db_slot`
    // the number of the favorite it belongs to — and that number arrives a
    // tenth of a second AFTER it, which makes the order certain: the drawing
    // happens on it.
    //
    // NAMES no longer travel through here. They are picked from a drop-down,
    // that is to say a control port, which mod-ui handles on its own and the
    // host saves with the pedalboard. Free typing was removed: on the Starless
    // image a string does not come back down to the plugin, so the text had to
    // be sent up one character at a time.
    //
    // The cells are looked up across the WHOLE page, not inside event.icon:
    // the settings panel is not a descendant of the icon, and that is where
    // they live.

    var state = window.__namMeasured;

    if (!state) {
        state = { db: 0 };
        window.__namMeasured = state;
    }

    if (event.type !== 'change') { return; }

    if (event.symbol === 'auto_db_slot') {
        state.db = event.value;
        return;
    }

    if (event.symbol !== 'db_slot') { return; }

    var slot = Math.round(event.value);

    if (!slot) { return; }

    var cell = jQuery('.nam-auto[data-slot="' + slot + '"]');

    if (!cell.length) { return; }

    var db = state.db || 0;

    cell.text((db >= 0 ? '+' : '') + db.toFixed(2) + ' dB');
    cell.toggleClass('nam-auto-live', Math.abs(db) > 0.01);
}
