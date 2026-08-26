#ifndef BLUETOOTH_SERVICE_CAT_H
#define BLUETOOTH_SERVICE_CAT_H

/*
 * String IDs and their built-in English text, in the shape CatComp produces.
 *
 * Written by hand for the same reason as the GUI's: CatComp is an Amiga tool
 * and this cross builds on a host that has none. The layout is kept identical,
 * so running CatComp over BluetoothService.cd should produce a drop-in
 * replacement.
 *
 * The text here is the fallback handed to GetCatalogStr(), so the service is
 * fully readable with no catalog installed, with one that does not cover an ID,
 * or with no locale.library at all.
 *
 * IDs start at 2000 to stay clear of the GUI's, which start at 1000. The two
 * have separate catalogs and could overlap harmlessly, but keeping them apart
 * means a string seen in a log is unambiguous about where it came from.
 */

#include <exec/types.h>

#define MSG_NOTIFY_TITLE          2000
#define MSG_NOTIFY_PAIRED         2001
#define MSG_NOTIFY_CONNECTED      2002
#define MSG_NOTIFY_DISCONNECTED   2003
#define MSG_NOTIFY_FILE           2004
#define MSG_NOTIFY_UNNAMED        2005

#define MSG_NOTIFY_TITLE_STR        "Bluetooth"
#define MSG_NOTIFY_PAIRED_STR       "%s is paired and ready to use"
#define MSG_NOTIFY_CONNECTED_STR    "%s is connected"
#define MSG_NOTIFY_DISCONNECTED_STR "%s has disconnected"
#define MSG_NOTIFY_FILE_STR         "Received %s"
#define MSG_NOTIFY_UNNAMED_STR      "A Bluetooth device"

struct CatCompArrayType {
    LONG   cca_ID;
    STRPTR cca_Str;
};

static const struct CatCompArrayType CatCompArray[] = {
    { MSG_NOTIFY_TITLE,        (STRPTR) MSG_NOTIFY_TITLE_STR },
    { MSG_NOTIFY_PAIRED,       (STRPTR) MSG_NOTIFY_PAIRED_STR },
    { MSG_NOTIFY_CONNECTED,    (STRPTR) MSG_NOTIFY_CONNECTED_STR },
    { MSG_NOTIFY_DISCONNECTED, (STRPTR) MSG_NOTIFY_DISCONNECTED_STR },
    { MSG_NOTIFY_FILE,         (STRPTR) MSG_NOTIFY_FILE_STR },
    { MSG_NOTIFY_UNNAMED,      (STRPTR) MSG_NOTIFY_UNNAMED_STR },
};

#define CATCOMP_ARRAY_SIZE (sizeof(CatCompArray) / sizeof(CatCompArray[0]))

#endif /* BLUETOOTH_SERVICE_CAT_H */
