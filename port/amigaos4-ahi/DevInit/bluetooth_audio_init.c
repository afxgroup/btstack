/*
 * ROMTAG and library initialisation for bluetooth.audio.
 *
 * Modelled on the USB Audio driver's, because an AHI sub-driver is a shared
 * library and there is one right way to build one. What differs is what gets
 * opened: this needs no USB and no libusb, only exec, dos and utility - the
 * Bluetooth is somebody else's process and is reached by a message port.
 */

#include "bluetooth_audio.h"

#include <exec/libraries.h>
#include <exec/resident.h>
#include <utility/utility.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

/* opened in libInit, and used from every part of the driver */
struct ExecIFace    * IExec;
struct DOSIFace     * IDOS;
struct UtilityIFace * IUtility;

struct Library * DOSBase;
struct Library * UtilityBase;

/*
 * The CRT stubs linked by -mcrt reference this, and nothing opens it because
 * nothing here calls libc. Defined so the link resolves; never entered.
 */
struct Library   * NewlibBase;
struct Interface * INewlib;

STATIC CONST TEXT USED verstag[] = VERSTAG;

/* -------------------------------------------------------------------------- */

struct BluetoothAudioIFace;

struct BluetoothAudioIFace * VARARGS68K _btaudio_Clone(struct BluetoothAudioIFace * Self){
    (void) Self;
    /* an AHI sub-driver is used by one AHI at a time; nothing to clone */
    return (struct BluetoothAudioIFace *) 0;
}

uint32 VARARGS68K _btaudio_Obtain(struct BluetoothAudioIFace * Self){
    return ((struct Interface *) Self)->Data.RefCount++;
}

uint32 VARARGS68K _btaudio_Release(struct BluetoothAudioIFace * Self){
    return --((struct Interface *) Self)->Data.RefCount;
}

/* -------------------------------------------------------------------------- */

STATIC struct Library * libInit(struct BluetoothAudioBase * libBase, BPTR seglist,
                                struct Interface * exec){
    IExec = (struct ExecIFace *) exec;

    libBase->libNode.lib_Node.ln_Type = NT_LIBRARY;
    libBase->libNode.lib_Node.ln_Pri  = 0;
    libBase->libNode.lib_Node.ln_Name = (STRPTR) LIBNAME;
    libBase->libNode.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    libBase->libNode.lib_Version      = VERSION;
    libBase->libNode.lib_Revision     = REVISION;
    libBase->libNode.lib_IdString     = (STRPTR) VSTRING;
    libBase->segList                  = seglist;
    libBase->iexec                    = IExec;

    DOSBase = IExec->OpenLibrary("dos.library", 50);
    if (DOSBase != NULL){
        IDOS = (struct DOSIFace *) IExec->GetInterface(DOSBase, "main", 1, NULL);
    }
    UtilityBase = IExec->OpenLibrary("utility.library", 50);
    if (UtilityBase != NULL){
        IUtility = (struct UtilityIFace *) IExec->GetInterface(UtilityBase, "main", 1, NULL);
    }

    /*
     * Utility is not optional: AHI hands over its mixing and player routines as
     * Hooks, and CallHookPkt is the only way to call one.
     */
    if ((IDOS == NULL) || (IUtility == NULL)){
        if (IDOS != NULL)     IExec->DropInterface((struct Interface *) IDOS);
        if (IUtility != NULL) IExec->DropInterface((struct Interface *) IUtility);
        if (DOSBase != NULL)     IExec->CloseLibrary(DOSBase);
        if (UtilityBase != NULL) IExec->CloseLibrary(UtilityBase);
        return NULL;
    }

    return &libBase->libNode;
}

STATIC BPTR libExpunge(struct LibraryManagerInterface * Self){
    struct BluetoothAudioBase * libBase = (struct BluetoothAudioBase *) Self->Data.LibBase;

    if (libBase->libNode.lib_OpenCnt != 0){
        libBase->libNode.lib_Flags |= LIBF_DELEXP;
        return ZERO;
    }

    BPTR result = libBase->segList;

    if (IUtility != NULL){ IExec->DropInterface((struct Interface *) IUtility); IUtility = NULL; }
    if (IDOS != NULL)    { IExec->DropInterface((struct Interface *) IDOS);     IDOS     = NULL; }
    if (UtilityBase != NULL){ IExec->CloseLibrary(UtilityBase); UtilityBase = NULL; }
    if (DOSBase != NULL)    { IExec->CloseLibrary(DOSBase);     DOSBase     = NULL; }

    IExec->Remove((struct Node *) libBase);
    IExec->DeleteLibrary((struct Library *) libBase);
    return result;
}

STATIC struct Library * libOpen(struct LibraryManagerInterface * Self, ULONG version){
    struct BluetoothAudioBase * libBase = (struct BluetoothAudioBase *) Self->Data.LibBase;

    if (version > VERSION) return NULL;

    libBase->libNode.lib_OpenCnt++;
    libBase->libNode.lib_Flags &= ~LIBF_DELEXP;
    return &libBase->libNode;
}

STATIC BPTR libClose(struct LibraryManagerInterface * Self){
    struct BluetoothAudioBase * libBase = (struct BluetoothAudioBase *) Self->Data.LibBase;

    libBase->libNode.lib_OpenCnt--;
    if ((libBase->libNode.lib_OpenCnt == 0) && (libBase->libNode.lib_Flags & LIBF_DELEXP)){
        return libExpunge(Self);
    }
    return ZERO;
}

/* -------------------------------------------------------------------------- */

STATIC CONST APTR lib_manager_vectors[] =
{
    _btaudio_Obtain,
    _btaudio_Release,
    NULL,
    NULL,
    libOpen,
    libClose,
    libExpunge,
    NULL,
    (APTR) -1
};

STATIC CONST struct TagItem lib_managerTags[] =
{
    { MIT_Name,        (Tag) "__library"          },
    { MIT_VectorTable, (Tag) lib_manager_vectors  },
    { MIT_Version,     1                          },
    { TAG_DONE,        0                          }
};

#include "bluetooth_audio_vectors.c"

STATIC CONST struct TagItem mainTags[] =
{
    { MIT_Name,        (Tag) "main"       },
    { MIT_VectorTable, (Tag) main_vectors },
    { MIT_Version,     1                  },
    { TAG_DONE,        0                  }
};

STATIC CONST CONST_APTR libInterfaces[] =
{
    lib_managerTags,
    mainTags,
    NULL
};

STATIC CONST struct TagItem libCreateTags[] =
{
    { CLT_DataSize,   sizeof(struct BluetoothAudioBase) },
    { CLT_InitFunc,   (Tag) libInit                     },
    { CLT_Interfaces, (Tag) libInterfaces               },
    { TAG_DONE,       0                                 }
};

STATIC CONST struct Resident lib_res USED =
{
    RTC_MATCHWORD,
    (struct Resident *) &lib_res,
    (APTR) (&lib_res + 1),
    RTF_NATIVE | RTF_AUTOINIT,
    VERSION,
    NT_LIBRARY,
    -120,
    (STRPTR) LIBNAME,
    (STRPTR) VSTRING,
    (APTR) libCreateTags
};

/* a shared library has no startup code, so this is only ever a placeholder */
int32 _start(void){
    return RETURN_FAIL;
}
