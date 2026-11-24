// Browser functions the C app calls (declared in main.c); linked into the
// web build with --js-library.
addToLibrary({
    // Show the room's link, /r/<code>, in the address bar without a reload or
    // a history entry. Any ?name= is dropped so a shared link doesn't carry it.
    wobble_show_room__deps: ['$UTF8ToString'],
    wobble_show_room: function(code) {
        history.replaceState(null, "", "/r/" + encodeURIComponent(UTF8ToString(code)));
    },
    // Back to the bare page once we leave the room.
    wobble_show_home: function() {
        history.replaceState(null, "", "/");
    },
    // Copy the room's link. navigator.clipboard needs a secure page
    // (https or localhost); elsewhere fall back to a hidden textarea.
    wobble_copy_text__deps: ['$UTF8ToString'],
    wobble_copy_text: function(ptr) {
        var text = UTF8ToString(ptr);
        function fallback() {
            var area = document.createElement("textarea");
            area.value = text;
            area.style.position = "fixed";
            area.style.opacity = "0";
            document.body.appendChild(area);
            area.select();
            try { document.execCommand("copy"); } catch (e) {}
            document.body.removeChild(area);
        }
        if (navigator.clipboard && window.isSecureContext)
            navigator.clipboard.writeText(text).catch(fallback);
        else
            fallback();
    },
    // A room on another host: that host serves its own page.
    wobble_open__deps: ['$UTF8ToString'],
    wobble_open: function(url) {
        location.assign(UTF8ToString(url));
    },
});
