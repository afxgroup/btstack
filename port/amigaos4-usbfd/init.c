/*
 * bt.usbfd - Bluetooth USB hotplug driver for AmigaOS 4
 *
 * Intercepts Bluetooth USB controller attach/detach events
 * and makes sure the Bluetooth service is running, so a dongle
 * plugged in just works.
 */

#include <exec/exec.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <usb/system.h>
#include <proto/usbfd.h>

#include <dos/startup.h>
#include <stdarg.h>

#include "fdmain.h"

#define VERSION  1
#define REVISION 0
#define VSTRING  "bt.usbfd 1.0 (01.01.2026)"
#define VERSTAG  "\0$VER: bt.usbfd 1.0 (01.01.2026)"

static const char verstag[] __attribute__((used)) = VERSTAG;

struct BTFDLibrary
{
    struct Library  LibNode;
    int16           pad;
    BPTR            SegList;
};

/* Global interfaces */
struct ExecIFace       *IExec;
#ifdef NEWLIB
struct Library         *NewlibBase;
struct Interface       *INewlib;
#else
struct Library         *Clib4Base;
struct Interface       *IClib4;
#endif
struct Library         *UtilityBase;
struct UtilityIFace    *IUtility;
struct USBResourceIFace *IUSBResource;
static struct Library  *USBResourceBase;

/* Start entry point — it's a library, not an executable */
LONG _start(STRPTR argstring __attribute__((unused)),
            LONG arglen __attribute__((unused)),
            struct ExecBase *sysbase __attribute__((unused)))
{
    return RETURN_FAIL;
}

/* Open the library */
static struct Library *libOpen(struct LibraryManagerInterface *Self, ULONG version)
{
    struct BTFDLibrary *libBase = (struct BTFDLibrary *)Self->Data.LibBase;

    if (version > VERSION)
        return NULL;

    libBase->LibNode.lib_OpenCnt++;
    return &libBase->LibNode;
}

/* Close the library */
static BPTR libClose(struct LibraryManagerInterface *Self)
{
    struct BTFDLibrary *libBase = (struct BTFDLibrary *)Self->Data.LibBase;

    if (libBase->LibNode.lib_OpenCnt > 0)
        libBase->LibNode.lib_OpenCnt--;

    return ZERO;
}

/* Expunge the library */
static BPTR libExpunge(struct LibraryManagerInterface *Self)
{
    BPTR result = ZERO;
    struct BTFDLibrary *libBase = (struct BTFDLibrary *)Self->Data.LibBase;

    if (libBase->LibNode.lib_OpenCnt == 0)
    {
        result = libBase->SegList;

        if (USBResourceBase != NULL)
        {
            fdmain_unregister();

            IExec->DropInterface((struct Interface *)IUSBResource);
            IExec->CloseLibrary(USBResourceBase);
            USBResourceBase = NULL;
            IUSBResource = NULL;
        }

        if (UtilityBase)
        {
            IExec->DropInterface((struct Interface *)IUtility);
            IExec->CloseLibrary(UtilityBase);
            UtilityBase = NULL;
            IUtility = NULL;
        }

#ifdef NEWLIB
        IExec->DropInterface(INewlib);
        INewlib = NULL;
        IExec->CloseLibrary(NewlibBase);
        NewlibBase = NULL;
#else
        IExec->DropInterface(IClib4);
        IClib4 = NULL;
        IExec->CloseLibrary(Clib4Base);
        Clib4Base = NULL;
#endif

        IExec->Remove((struct Node *)libBase);
        IExec->DeleteLibrary(&libBase->LibNode);
    }
    else
    {
        libBase->LibNode.lib_Flags |= LIBF_DELEXP;
    }

    return result;
}

/* The ROMTAG Init Function */
static struct Library *libInit(struct BTFDLibrary *libBase, BPTR seglist, struct Interface *exec)
{
    IExec = (struct ExecIFace *)exec;

#ifdef NEWLIB
    NewlibBase = IExec->OpenLibrary("newlib.library", 53);
    if (NewlibBase)
        INewlib = IExec->GetInterface(NewlibBase, "main", 1, NULL);

    if (INewlib == NULL)
        return NULL;
#else
    Clib4Base = IExec->OpenLibrary("clib4.library", 2);
    if (Clib4Base)
        IClib4 = IExec->GetInterface(Clib4Base, "main", 1, NULL);
    if (IClib4 == NULL)
        return NULL;
#endif

    libBase->LibNode.lib_Node.ln_Type = NT_LIBRARY;
    libBase->LibNode.lib_Node.ln_Pri  = 0;
    libBase->LibNode.lib_Node.ln_Name = (STRPTR)"bt.usbfd";
    libBase->LibNode.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    libBase->LibNode.lib_Version      = VERSION;
    libBase->LibNode.lib_Revision     = REVISION;
    libBase->LibNode.lib_IdString     = (STRPTR)VSTRING;

    libBase->SegList = seglist;

    UtilityBase = IExec->OpenLibrary("utility.library", 50);
    if (UtilityBase)
        IUtility = (struct UtilityIFace *)IExec->GetInterface(UtilityBase, "main", 1, NULL);

    if (IUtility == NULL)
        goto fail;

    USBResourceBase = IExec->OpenLibrary("usbresource.library", 53);
    if (USBResourceBase)
        IUSBResource = (struct USBResourceIFace *)IExec->GetInterface(USBResourceBase, "main", 1, NULL);

    if (IUSBResource == NULL)
        goto fail;

    if (fdmain_register())
        return &libBase->LibNode;

fail:
    if (USBResourceBase)
    {
        if (IUSBResource) IExec->DropInterface((struct Interface *)IUSBResource);
        IExec->CloseLibrary(USBResourceBase);
        USBResourceBase = NULL;
        IUSBResource = NULL;
    }

    if (UtilityBase)
    {
        if (IUtility) IExec->DropInterface((struct Interface *)IUtility);
        IExec->CloseLibrary(UtilityBase);
        UtilityBase = NULL;
        IUtility = NULL;
    }

#ifdef NEWLIB
    IExec->DropInterface(INewlib);
    INewlib = NULL;
    IExec->CloseLibrary(NewlibBase);
    NewlibBase = NULL;
#else
    IExec->DropInterface(IClib4);
    IClib4 = NULL;
    IExec->CloseLibrary(Clib4Base);
    Clib4Base = NULL;
#endif

    return NULL;
}

/* ------------------- Manager Interface ------------------------ */

static LONG _manager_Obtain(struct LibraryManagerInterface *Self)
{
    return ++Self->Data.RefCount;
}

static ULONG _manager_Release(struct LibraryManagerInterface *Self)
{
    return --Self->Data.RefCount;
}

STATIC CONST APTR lib_manager_vectors[] =
{
    _manager_Obtain,
    _manager_Release,
    NULL,
    NULL,
    libOpen,
    libClose,
    libExpunge,
    NULL,
    (APTR)-1
};

STATIC CONST struct TagItem lib_managerTags[] =
{
    { MIT_Name,        (Tag)"__library"         },
    { MIT_VectorTable, (Tag)lib_manager_vectors },
    { MIT_Version,     1                        },
    { TAG_END,         0                        }
};

/* ------------------- Main Interface ------------------------ */

#include "bt_vectors.c"

STATIC CONST struct TagItem mainTags[] =
{
    { MIT_Name,        (Tag)"main"       },
    { MIT_VectorTable, (Tag)main_vectors },
    { MIT_Version,     1                 },
    { TAG_END,         0                 }
};

STATIC CONST CONST_APTR libInterfaces[] =
{
    lib_managerTags,
    mainTags,
    NULL
};

STATIC CONST struct TagItem libCreateTags[] =
{
    { CLT_DataSize,   sizeof(struct BTFDLibrary) },
    { CLT_InitFunc,   (Tag)libInit                     },
    { CLT_Interfaces, (Tag)libInterfaces               },
    { TAG_END,        0                                }
};

/* -------------- Library ROM Tag ------------------------ */
STATIC CONST struct Resident lib_res __attribute__((used)) =
{
    RTC_MATCHWORD,
    (struct Resident *)&lib_res,
    (APTR)(&lib_res + 1),
    RTF_NATIVE | RTF_AUTOINIT,
    VERSION,
    NT_LIBRARY,
    0,    /* PRI — loaded by USB stack on demand */
    (STRPTR)"bt.usbfd",
    (STRPTR)VSTRING,
    (APTR)libCreateTags
};
