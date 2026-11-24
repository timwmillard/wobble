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
});
