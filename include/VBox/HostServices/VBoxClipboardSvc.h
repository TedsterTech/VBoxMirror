/** @file
 * Shared Clipboard - Common header for host service and guest clients.
 */

/*
 * Copyright (C) 2006-2019 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */

#ifndef VBOX_INCLUDED_HostServices_VBoxClipboardSvc_h
#define VBOX_INCLUDED_HostServices_VBoxClipboardSvc_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <VBox/VMMDevCoreTypes.h>
#include <VBox/VBoxGuestCoreTypes.h>
#include <VBox/hgcmsvc.h>

/*
 * The mode of operations.
 */
#define VBOX_SHARED_CLIPBOARD_MODE_OFF           0
#define VBOX_SHARED_CLIPBOARD_MODE_HOST_TO_GUEST 1
#define VBOX_SHARED_CLIPBOARD_MODE_GUEST_TO_HOST 2
#define VBOX_SHARED_CLIPBOARD_MODE_BIDIRECTIONAL 3

/*
 * The service functions which are callable by host.
 */
#define VBOX_SHARED_CLIPBOARD_HOST_FN_SET_MODE           1
/** Run headless on the host, i.e. do not touch the host clipboard. */
#define VBOX_SHARED_CLIPBOARD_HOST_FN_SET_HEADLESS       2
/** Reports cancellation of the current operation to the guest. */
#define VBOX_SHARED_CLIPBOARD_HOST_FN_CANCEL             3
/** Reports an error to the guest. */
#define VBOX_SHARED_CLIPBOARD_HOST_FN_ERROR              4
/** Reports that a new clipboard area has been registered. */
#define VBOX_SHARED_CLIPBOARD_HOST_FN_AREA_REGISTER      5
/** Reports that a clipboard area has been unregistered. */
#define VBOX_SHARED_CLIPBOARD_HOST_FN_AREA_UNREGISTER    6
/** Reports that a client (host / guest) has attached to a clipboard area. */
#define VBOX_SHARED_CLIPBOARD_HOST_FN_AREA_ATTACH        7
/** Reports that a client (host / guest) has detached from a clipboard area. */
#define VBOX_SHARED_CLIPBOARD_HOST_FN_AREA_DETACH        8

/*
 * The service functions which are called by guest.
 */
/** Calls the host and waits (blocking) for an host event VBOX_SHARED_CLIPBOARD_HOST_MSG_*. */
#define VBOX_SHARED_CLIPBOARD_FN_GET_HOST_MSG      1
/** Sends a list of available formats to the host. */
#define VBOX_SHARED_CLIPBOARD_FN_REPORT_FORMATS    2
/** Reads data in specified format from the host. */
#define VBOX_SHARED_CLIPBOARD_FN_READ_DATA         3
/** Writes data in requested format to the host. */
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA        4
/** Reads the data header at the beginning of a (new) data transfer from the host.
 *  New since URI handling was implemented. */
#define VBOX_SHARED_CLIPBOARD_FN_READ_DATA_HDR     5
/** Writes data in requested format to the host. */
#define VBOX_SHARED_CLIPBOARD_FN_READ_DATA_CHUNK   6
/** Writes the data header at the beginning of a (new) data transfer to the host.
 *  New since URI handling was implemented. */
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA_HDR    7
/** Writes data in requested format to the host. */
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA_CHUNK  8
/** Reads a new directory entry from the host.
 *  New since URI handling was implemented. */
#define VBOX_SHARED_CLIPBOARD_FN_READ_DIR          9
/** Writes a new directory entry to the host.
 *  New since URI handling was implemented. */
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_DIR         10
/** Reads a new file header entry from the host.
 *  New since URI handling was implemented. */
#define VBOX_SHARED_CLIPBOARD_FN_READ_FILE_HDR     11
/** Writes a new file header entry to the host.
     *  New since URI handling was implemented. */
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_FILE_HDR    12
/** Reads a new file data chunk entry from the host.
 *  New since URI handling was implemented. */
#define VBOX_SHARED_CLIPBOARD_FN_READ_FILE_DATA    13
/** Writes a new file data chunk entry to the host.
 *  New since URI handling was implemented. */
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_FILE_DATA   14
/** Reports cancellation of the current operation to the host.
 *  New since URI handling was implemented. */
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_CANCEL      15
/** Reports an error to the host.
 *  New since URI handling was implemented. */
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_ERROR       16

/** The maximum default chunk size for a single data transfer. */
#define VBOX_SHARED_CLIPBOARD_MAX_CHUNK_SIZE       _64K

/*
 * HGCM parameter structures.
 */
#pragma pack(1)
typedef struct _VBoxClipboardGetHostMsg
{
    VBGLIOCHGCMCALL hdr;

    /* VBOX_SHARED_CLIPBOARD_HOST_MSG_* */
    HGCMFunctionParameter msg;     /* OUT uint32_t */

    /* VBOX_SHARED_CLIPBOARD_FMT_*, depends on the 'msg'. */
    HGCMFunctionParameter formats; /* OUT uint32_t */
} VBoxClipboardGetHostMsg;

#define VBOX_SHARED_CLIPBOARD_CPARMS_GET_HOST_MSG 2

typedef struct _VBoxClipboardWriteFormatsMsg
{
    VBGLIOCHGCMCALL hdr;

    /* VBOX_SHARED_CLIPBOARD_FMT_* */
    HGCMFunctionParameter formats; /* OUT uint32_t */
} VBoxClipboardWriteFormatsMsg;

#define VBOX_SHARED_CLIPBOARD_CPARMS_FORMATS 1

typedef struct _VBoxClipboardReadDataMsg
{
    VBGLIOCHGCMCALL hdr;

    /* Requested format. */
    HGCMFunctionParameter format; /* IN uint32_t */

    /* The data buffer. */
    HGCMFunctionParameter ptr;    /* IN linear pointer. */

    /* Size of returned data, if > ptr->cb, then no data was
     * actually transferred and the guest must repeat the call.
     */
    HGCMFunctionParameter size;   /* OUT uint32_t */

} VBoxClipboardReadDataMsg;

#define VBOX_SHARED_CLIPBOARD_CPARMS_READ_DATA 3

typedef struct _VBoxClipboardWriteDataMsg
{
    VBGLIOCHGCMCALL hdr;

    /* Returned format as requested in the VBOX_SHARED_CLIPBOARD_HOST_MSG_READ_DATA message. */
    HGCMFunctionParameter format; /* IN uint32_t */

    /* Data.  */
    HGCMFunctionParameter ptr;    /* IN linear pointer. */
} VBoxClipboardWriteDataMsg;

#define VBOX_SHARED_CLIPBOARD_CPARMS_WRITE_DATA 2

/**
 * Reads / writes the (meta) data header.
 *
 * Used by:
 * VBOX_SHARED_CLIPBOARD_FN_READ_DATA_HDR
 * VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA_HDR
 */
typedef struct _VBoxClipboardDataHdrMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;        /* OUT uint32_t */
    /** Data transfer flags. Not yet used and must be 0. */
    HGCMFunctionParameter uFlags;          /* OUT uint32_t */
    /** Screen ID where the data originates from. */
    HGCMFunctionParameter uScreenId;       /* OUT uint32_t */
    /** Total size (in bytes) to transfer. */
    HGCMFunctionParameter cbTotal;         /* OUT uint64_t */
    /**
     * Total meta data size (in bytes) to transfer.
     * This size also is part of cbTotal already, so:
     *
     * cbTotal = cbMeta + additional size for files etc.
     */
    HGCMFunctionParameter cbMeta;          /* OUT uint64_t */
    /** Meta data format. */
    HGCMFunctionParameter pvMetaFmt;       /* OUT ptr */
    /** Size (in bytes) of meta data format. */
    HGCMFunctionParameter cbMetaFmt;       /* OUT uint32_t */
    /* Number of objects (files/directories) to transfer. */
    HGCMFunctionParameter cObjects;        /* OUT uint64_t */
    /** Compression type. */
    HGCMFunctionParameter enmCompression;  /* OUT uint32_t */
    /** Checksum type. */
    HGCMFunctionParameter enmChecksumType; /* OUT uint32_t */
    /** Checksum buffer for the entire data to be transferred. */
    HGCMFunctionParameter pvChecksum;      /* OUT ptr */
    /** Size (in bytes) of checksum. */
    HGCMFunctionParameter cbChecksum;      /* OUT uint32_t */
} VBoxClipboardReadDataHdrMsg, VBoxClipboardWriteDataHdrMsg;

#define VBOX_SHARED_CLIPBOARD_CPARMS_READ_DATA_HDR  12
#define VBOX_SHARED_CLIPBOARD_CPARMS_WRITE_DATA_HDR 12

/**
 * Reads / writes a (meta) data block.
 *
 * Used by:
 * VBOX_SHARED_CLIPBOARD_FN_READ_DATA_CHUNK
 * VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA_CHUNK
 */
typedef struct _VBoxClipboardDataChunkMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;     /* OUT uint32_t */
    /** Data block to send. */
    HGCMFunctionParameter pvData;       /* OUT ptr */
    /** Size (in bytes) of data block to send. */
    HGCMFunctionParameter cbData;       /* OUT uint32_t */
    /** (Rolling) Checksum, based on checksum type in data header. */
    HGCMFunctionParameter pvChecksum;   /* OUT ptr */
    /** Size (in bytes) of checksum. */
    HGCMFunctionParameter cbChecksum;   /* OUT uint32_t */
} VBoxClipboardReadDataChunkMsg, VBoxClipboardWriteDataChunkMsg;

#define VBOX_SHARED_CLIPBOARD_CPARMS_READ_DATA_CHUNK  5
#define VBOX_SHARED_CLIPBOARD_CPARMS_WRITE_DATA_CHUNK 5

/**
 * Reads / writes a directory entry.
 *
 * Used by:
 * VBOX_SHARED_CLIPBOARD_FN_READ_DIR
 * VBOX_SHARED_CLIPBOARD_FN_WRITE_DIR
 */
typedef struct _VBoxClipboardDirMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;     /* OUT uint32_t */
    /** Directory name. */
    HGCMFunctionParameter pvName;       /* OUT ptr */
    /** Size (in bytes) of directory name. */
    HGCMFunctionParameter cbName;       /* OUT uint32_t */
    /** Directory mode. */
    HGCMFunctionParameter fMode;        /* OUT uint32_t */
} VBoxClipboardReadDirMsg, VBoxClipboardWriteDirMsg;

#define VBOX_SHARED_CLIPBOARD_CPARMS_READ_DIR  4
#define VBOX_SHARED_CLIPBOARD_CPARMS_WRITE_DIR 4

/**
 * File header message, marking the start of transferring a new file.
 *
 * Used by:
 * VBOX_SHARED_CLIPBOARD_FN_READ_FILE_HDR
 * VBOX_SHARED_CLIPBOARD_FN_WRITE_FILE_HDR
 */
typedef struct _VBoxClipboardFileHdrMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;     /* OUT uint32_t */
    /** File path. */
    HGCMFunctionParameter pvName;       /* OUT ptr */
    /** Size (in bytes) of file path. */
    HGCMFunctionParameter cbName;       /* OUT uint32_t */
    /** Optional flags; unused at the moment. */
    HGCMFunctionParameter uFlags;       /* OUT uint32_t */
    /** File creation mode. */
    HGCMFunctionParameter fMode;        /* OUT uint32_t */
    /** Total size (in bytes). */
    HGCMFunctionParameter cbTotal;      /* OUT uint64_t */
} VBoxClipboardReadFileHdrMsg, VBoxClipboardWriteFileHdrMsg;

#define VBOX_SHARED_CLIPBOARD_CPARMS_READ_FILE_HDR  6
#define VBOX_SHARED_CLIPBOARD_CPARMS_WRITE_FILE_HDR 6

/**
 * Reads / writes data of a file entry.
 *
 * Used by:
 * VBOX_SHARED_CLIPBOARD_FN_READ_FILE_DATA
 * VBOX_SHARED_CLIPBOARD_FN_WRITE_FILE_DATA
 */
typedef struct _VBoxClipboardFileDataMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;     /* OUT uint32_t */
    /** Current data chunk. */
    HGCMFunctionParameter pvData;       /* OUT ptr */
    /** Size (in bytes) of current data chunk. */
    HGCMFunctionParameter cbData;       /* OUT uint32_t */
    /** Checksum of data block, based on the checksum
     *  type in the data header. Optional. */
    HGCMFunctionParameter pvChecksum;   /* OUT ptr */
    /** Size (in bytes) of curren data chunk checksum. */
    HGCMFunctionParameter cbChecksum;   /* OUT uint32_t */
} VBoxClipboardReadFileDataMsg, VBoxClipboardWriteFileDataMsg;

#define VBOX_SHARED_CLIPBOARD_CPARMS_READ_FILE_DATA  5
#define VBOX_SHARED_CLIPBOARD_CPARMS_WRITE_FILE_DATA 5

/**
 * Sends an error event.
 *
 * Used by:
 * VBOX_SHARED_CLIPBOARD_FN_WRITE_ERROR
 */
typedef struct _VBoxClipboardErrorMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;     /* OUT uint32_t */
    /** The error code (IPRT-style). */
    HGCMFunctionParameter rc;           /* OUT uint32_t */
} VBoxClipboardWriteErrorMsg;

#pragma pack()

# ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST

/** Builds a callback magic out of the function ID and the version
 *  of the callback data. */
#define VBOX_CLIPBOARD_CB_MAGIC_MAKE(uFn, uVer) \
    RT_MAKE_U32(uVer, uFn)

/*
 * Callback magics.
 */
enum eVBoxClipboardCallbackMagics
{
    CB_MAGIC_CLIPBOARD_WRITE_DATA_HDR   = VBOX_CLIPBOARD_CB_MAGIC_MAKE(VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA_HDR, 0),
    CB_MAGIC_CLIPBOARD_WRITE_DATA_CHUNK = VBOX_CLIPBOARD_CB_MAGIC_MAKE(VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA_CHUNK, 0),
    CB_MAGIC_CLIPBOARD_WRITE_DIR        = VBOX_CLIPBOARD_CB_MAGIC_MAKE(VBOX_SHARED_CLIPBOARD_FN_WRITE_DIR, 0),
    CB_MAGIC_CLIPBOARD_WRITE_FILE_HDR   = VBOX_CLIPBOARD_CB_MAGIC_MAKE(VBOX_SHARED_CLIPBOARD_FN_WRITE_FILE_HDR, 0),
    CB_MAGIC_CLIPBOARD_WRITE_FILE_DATA  = VBOX_CLIPBOARD_CB_MAGIC_MAKE(VBOX_SHARED_CLIPBOARD_FN_WRITE_FILE_DATA, 0),
    CB_MAGIC_CLIPBOARD_WRITE_ERROR      = VBOX_CLIPBOARD_CB_MAGIC_MAKE(VBOX_SHARED_CLIPBOARD_FN_WRITE_ERROR, 0)
};

/**
 * Data header.
 */
typedef struct _VBOXCLIPBOARDDATAHDR
{
    /** Data transfer flags. Not yet used and must be 0. */
    uint32_t                    uFlags;
    /** Screen ID where the data originates from. */
    uint32_t                    uScreenId;
    /** Total size (in bytes) to transfer. */
    uint64_t                    cbTotal;
    /** Meta data size (in bytes) to transfer.
     *  This size also is part of cbTotal already. */
    uint32_t                    cbMeta;
    /** Meta format buffer. */
    void                       *pvMetaFmt;
    /** Size (in bytes) of meta format buffer. */
    uint32_t                    cbMetaFmt;
    /** Number of objects (files/directories) to transfer. */
    uint64_t                    cObjects;
    /** Compression type. Currently unused, so specify 0.
     **@todo Add IPRT compression type enumeration as soon as it's available. */
    uint32_t                    enmCompression;
    /** Checksum type. Currently unused, so specify RTDIGESTTYPE_INVALID. */
    RTDIGESTTYPE                enmChecksumType;
    /** The actual checksum buffer for the entire data to be transferred,
     *  based on enmChksumType. If RTDIGESTTYPE_INVALID is specified,
     *  no checksum is being used and pvChecksum will be NULL. */
    void                       *pvChecksum;
    /** Size (in bytes) of checksum. */
    uint32_t                    cbChecksum;
} VBOXCLIPBOARDDATAHDR, *PVBOXCLIPBOARDDATAHDR;

typedef struct _VBOXCLIPBOARDDATACHUNK
{
    /** Data block buffer. */
    void                       *pvData;
    /** Size (in bytes) of data block. */
    uint32_t                    cbData;
    /** (Rolling) Checksum. Not yet implemented. */
    void                       *pvChecksum;
    /** Size (in bytes) of checksum. Not yet implemented. */
    uint32_t                    cbChecksum;
} VBOXCLIPBOARDDATACHUNK, *PVBOXCLIPBOARDDATACHUNK;

typedef struct _VBOXCLIPBOARDDIRDATA
{
    /** Directory path. */
    char                       *pszPath;
    /** Size (in bytes) of path. */
    uint32_t                    cbPath;
    /** Directory creation mode. */
    uint32_t                    fMode;
} VBOXCLIPBOARDDIRDATA, *PVBOXCLIPBOARDDIRDATA;

typedef struct _VBOXCLIPBOARDFILEHDR
{
    /** File path (name). */
    char                       *pszFilePath;
    /** Size (in bytes) of file path. */
    uint32_t                    cbFilePath;
    /** Total size (in bytes) of this file. */
    uint64_t                    cbSize;
    /** File (creation) mode. */
    uint32_t                    fMode;
    /** Additional flags. Not used at the moment. */
    uint32_t                    fFlags;
} VBOXCLIPBOARDFILEHDR, *PVBOXCLIPBOARDFILEHDR;

typedef struct _VBOXCLIPBOARDFILEDATA
{
    /** Current file data chunk. */
    void                       *pvData;
    /** Size (in bytes) of current data chunk. */
    uint32_t                    cbData;
    /** Checksum for current file data chunk. */
    void                       *pvChecksum;
    /** Size (in bytes) of current data chunk. */
    uint32_t                    cbChecksum;
} VBOXCLIPBOARDFILEDATA, *PVBOXCLIPBOARDFILEDATA;

typedef struct _VBOXCLIPBOARDERRORDATA
{
    int32_t                     rc;
} VBOXCLIPBOARDERRORDATA, *PVBOXCLIPBOARDERRORDATA;
# endif /* VBOX_WITH_SHARED_CLIPBOARD_URI_LIST */

bool VBoxSvcClipboardGetHeadless(void);
bool VBoxSvcClipboardLock(void);
void VBoxSvcClipboardUnlock(void);

# ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
bool VBoxSvcClipboardDataHdrIsValid(PVBOXCLIPBOARDDATAHDR pDataHdr);
bool VBoxSvcClipboardDataChunkIsValid(PVBOXCLIPBOARDDATACHUNK pDataChunk);
bool VBoxSvcClipboardDirDataIsValid(PVBOXCLIPBOARDDIRDATA pDirData);
bool VBoxSvcClipboardFileHdrIsValid(PVBOXCLIPBOARDFILEHDR pFileHdr, PVBOXCLIPBOARDDATAHDR pDataHdr);
bool VBoxSvcClipboardFileDataIsValid(PVBOXCLIPBOARDFILEDATA pFileData, PVBOXCLIPBOARDDATAHDR pDataHdr);
# endif /* VBOX_WITH_SHARED_CLIPBOARD_URI_LIST */

#endif /* !VBOX_INCLUDED_HostServices_VBoxClipboardSvc_h */
