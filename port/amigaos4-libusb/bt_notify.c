/*
 * Desktop notifications through application.library.
 *
 * See bt_notify.h for what this is for. The whole file is written so that every
 * failure is silent and harmless: a service that cannot post a notification
 * should carry on running Bluetooth, not stop.
 */

#include <stdio.h>

#include <proto/exec.h>
#include <proto/locale.h>
#include <proto/application.h>
#include <libraries/application.h>

#include "bt_notify.h"
#include "bluetooth_service_cat.h"

/*
 * These are the globals the proto/ headers declare: the inline stubs reach
 * through them, so they are defined here rather than kept private. clib4 opens
 * neither library for us, so both start NULL and stay that way if the open
 * fails - which is exactly the "silent and harmless" behaviour wanted.
 */
struct Library          * ApplicationBase;
struct ApplicationIFace * IApplication;
struct Library          * LocaleBase;
struct LocaleIFace      * ILocale;

static struct Catalog * catalog;
static uint32           appID;

/*
 * The text for a string ID, translated when there is a translation.
 *
 * The built-in English is the fallback GetCatalogStr() uses, so every ID has an
 * answer whether or not a catalog is installed, whether or not it covers this
 * ID, and whether or not locale.library opened. Nothing calling this has to
 * check anything.
 */
static const char * notify_string(LONG id){
    const char * text = "";
    uint32 i;

    for (i = 0; i < CATCOMP_ARRAY_SIZE; i++){
        if (CatCompArray[i].cca_ID == id){
            text = (const char *) CatCompArray[i].cca_Str;
            break;
        }
    }
    if (ILocale == NULL) return text;
    return (const char *) GetCatalogStr(catalog, id, (CONST_STRPTR) text);
}

void bt_notify_open(void){

    /* a missing catalog is not a failure: notify_string() falls back to English */
    LocaleBase = OpenLibrary("locale.library", 52);
    if (LocaleBase != NULL){
        ILocale = (struct LocaleIFace *)
            GetInterface(LocaleBase, "main", 1, NULL);
        if (ILocale != NULL){
            catalog = OpenCatalogA(NULL,
                                                  (CONST_STRPTR) "BluetoothService.catalog",
                                                  NULL);
        }
    }

    ApplicationBase = OpenLibrary("application.library", 52);
    if (ApplicationBase == NULL) return;

    IApplication = (struct ApplicationIFace *)
        GetInterface(ApplicationBase, "application", 2, NULL);
    if (IApplication == NULL){
        CloseLibrary(ApplicationBase);
        ApplicationBase = NULL;
        return;
    }

    /*
     * Registering is what makes notifications possible at all - Notify() takes
     * an application ID and refuses an unregistered one. The name is the one
     * the user will see attributed to each message.
     */
    appID = RegisterApplication((CONST_STRPTR) "BluetoothService",
                REGAPP_URLIdentifier,  "none",
                REGAPP_Description,    "Bluetooth Stack For AmigaOS4",
                TAG_END);
}

void bt_notify_close(void){

    if (IApplication != NULL){
        if (appID != 0){
            UnregisterApplication(appID, TAG_END);
            appID = 0;
        }
        DropInterface((struct Interface *) IApplication);
        IApplication = NULL;
    }
    if (ApplicationBase != NULL){
        CloseLibrary(ApplicationBase);
        ApplicationBase = NULL;
    }

    if (ILocale != NULL){
        if (catalog != NULL){
            CloseCatalog(catalog);
            catalog = NULL;
        }
        DropInterface((struct Interface *) ILocale);
        ILocale = NULL;
    }
    if (LocaleBase != NULL){
        CloseLibrary(LocaleBase);
        LocaleBase = NULL;
    }
}

/*
 * Post one notification.
 *
 * The message is built here rather than passed in already formatted, because
 * the format string is the translated one and only this file knows it.
 */
static void notify(LONG message_id, const char * argument){
    char text[160];

    if ((IApplication == NULL) || (appID == 0)) return;

    snprintf(text, sizeof(text), notify_string(message_id), argument);

    /*
     * The result is deliberately ignored. Every reason Notify() can fail - no
     * notification server running, the queue full, game mode active - is a
     * reason not to show a message, and none of them is a reason to disturb a
     * Bluetooth service that is otherwise working.
     */
    Notify(appID,
        APPNOTIFY_Title,     notify_string(MSG_NOTIFY_TITLE),
        APPNOTIFY_Text,      text,
        APPNOTIFY_CloseOnDC, TRUE,
        TAG_END);
}

/* A device with no name yet still deserves a readable sentence. */
static const char * or_unnamed(const char * name){
    if ((name == NULL) || (name[0] == 0)) return notify_string(MSG_NOTIFY_UNNAMED);
    return name;
}

void bt_notify_paired(const char * name){
    notify(MSG_NOTIFY_PAIRED, or_unnamed(name));
}

void bt_notify_connected(const char * name){
    notify(MSG_NOTIFY_CONNECTED, or_unnamed(name));
}

void bt_notify_disconnected(const char * name){
    notify(MSG_NOTIFY_DISCONNECTED, or_unnamed(name));
}

void bt_notify_file_received(const char * filename){
    if ((filename == NULL) || (filename[0] == 0)) return;
    notify(MSG_NOTIFY_FILE, filename);
}
