/* $Id$ */
/** @file
 * Shared Clipboard: Windows-specific functions for clipboard handling.
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
 */

#include <iprt/alloc.h>
#include <iprt/assert.h>
#include <iprt/errcore.h>
#include <iprt/ldr.h>
#include <iprt/thread.h>

#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
# include <iprt/utf16.h>
#endif

#define LOG_GROUP LOG_GROUP_SHARED_CLIPBOARD
#include <VBox/log.h>

#include <VBox/GuestHost/SharedClipboard.h>
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
# include <VBox/GuestHost/SharedClipboard-uri.h>
#endif
#include <VBox/GuestHost/SharedClipboard-win.h>
#include <VBox/GuestHost/clipboard-helper.h>

/**
 * Opens the clipboard of a specific window.
 *
 * @returns VBox status code.
 * @param   hWnd                Handle of window to open clipboard for.
 */
int VBoxClipboardWinOpen(HWND hWnd)
{
    /* "OpenClipboard fails if another window has the clipboard open."
     * So try a few times and wait up to 1 second.
     */
    BOOL fOpened = FALSE;

    LogFlowFunc(("hWnd=%p\n", hWnd));

    int i = 0;
    for (;;)
    {
        if (OpenClipboard(hWnd))
        {
            fOpened = TRUE;
            break;
        }

        if (i >= 10) /* sleep interval = [1..512] ms */
            break;

        RTThreadSleep(1 << i);
        ++i;
    }

#ifdef LOG_ENABLED
    if (i > 0)
        LogFlowFunc(("%d times tried to open clipboard\n", i + 1));
#endif

    int rc;
    if (fOpened)
        rc = VINF_SUCCESS;
    else
    {
        const DWORD dwLastErr = GetLastError();
        rc = RTErrConvertFromWin32(dwLastErr);
        LogFunc(("Failed to open clipboard, rc=%Rrc (0x%x)\n", rc, dwLastErr));
    }

    return rc;
}

/**
 * Closes the clipboard for the current thread.
 *
 * @returns VBox status code.
 */
int VBoxClipboardWinClose(void)
{
    int rc;

    LogFlowFuncEnter();

    const BOOL fRc = CloseClipboard();
    if (RT_UNLIKELY(!fRc))
    {
        const DWORD dwLastErr = GetLastError();
        if (dwLastErr == ERROR_CLIPBOARD_NOT_OPEN)
            rc = VERR_INVALID_STATE;
        else
            rc = RTErrConvertFromWin32(dwLastErr);

        LogFunc(("Failed with %Rrc (0x%x)\n", rc, dwLastErr));
    }
    else
        rc = VINF_SUCCESS;

    return rc;
}

/**
 * Clears the clipboard for the current thread.
 *
 * @returns VBox status code.
 */
int VBoxClipboardWinClear(void)
{
    int rc;

    LogFlowFuncEnter();

    const BOOL fRc = EmptyClipboard();
    if (RT_UNLIKELY(!fRc))
    {
        const DWORD dwLastErr = GetLastError();
        if (dwLastErr == ERROR_CLIPBOARD_NOT_OPEN)
            rc = VERR_INVALID_STATE;
        else
            rc = RTErrConvertFromWin32(dwLastErr);

        LogFunc(("Failed with %Rrc (0x%x)\n", rc, dwLastErr));
    }
    else
        rc = VINF_SUCCESS;

    return rc;
}

/**
 * Checks and initializes function pointer which are required for using
 * the new clipboard API.
 *
 * @returns VBox status code.
 * @param   pAPI                Where to store the retrieved function pointers.
 *                              Will be set to NULL if the new API is not available.
 */
int VBoxClipboardWinCheckAndInitNewAPI(PVBOXCLIPBOARDWINAPINEW pAPI)
{
    RTLDRMOD hUser32 = NIL_RTLDRMOD;
    int rc = RTLdrLoadSystem("User32.dll", /* fNoUnload = */ true, &hUser32);
    if (RT_SUCCESS(rc))
    {
        rc = RTLdrGetSymbol(hUser32, "AddClipboardFormatListener", (void **)&pAPI->pfnAddClipboardFormatListener);
        if (RT_SUCCESS(rc))
        {
            rc = RTLdrGetSymbol(hUser32, "RemoveClipboardFormatListener", (void **)&pAPI->pfnRemoveClipboardFormatListener);
        }

        RTLdrClose(hUser32);
    }

    if (RT_SUCCESS(rc))
    {
        LogFunc(("New Clipboard API enabled\n"));
    }
    else
    {
        RT_BZERO(pAPI, sizeof(VBOXCLIPBOARDWINAPINEW));
        LogFunc(("New Clipboard API not available; rc=%Rrc\n", rc));
    }

    return rc;
}

/**
 * Returns if the new clipboard API is available or not.
 *
 * @returns @c true if the new API is available, or @c false if not.
 * @param   pAPI                Structure used for checking if the new clipboard API is available or not.
 */
bool VBoxClipboardWinIsNewAPI(PVBOXCLIPBOARDWINAPINEW pAPI)
{
    if (!pAPI)
        return false;
    return pAPI->pfnAddClipboardFormatListener != NULL;
}

/**
 * Adds ourselves into the chain of cliboard listeners.
 *
 * @returns VBox status code.
 * @param   pCtx                Windows clipboard context to use to add ourselves.
 */
int VBoxClipboardWinAddToCBChain(PVBOXCLIPBOARDWINCTX pCtx)
{
    const PVBOXCLIPBOARDWINAPINEW pAPI = &pCtx->newAPI;

    BOOL fRc;
    if (VBoxClipboardWinIsNewAPI(pAPI))
    {
        fRc = pAPI->pfnAddClipboardFormatListener(pCtx->hWnd);
    }
    else
    {
        pCtx->hWndNextInChain = SetClipboardViewer(pCtx->hWnd);
        fRc = pCtx->hWndNextInChain != NULL;
    }

    int rc = VINF_SUCCESS;

    if (!fRc)
    {
        const DWORD dwLastErr = GetLastError();
        rc = RTErrConvertFromWin32(dwLastErr);
        LogFunc(("Failed with %Rrc (0x%x)\n", rc, dwLastErr));
    }

    return rc;
}

/**
 * Remove ourselves from the chain of cliboard listeners
 *
 * @returns VBox status code.
 * @param   pCtx                Windows clipboard context to use to remove ourselves.
 */
int VBoxClipboardWinRemoveFromCBChain(PVBOXCLIPBOARDWINCTX pCtx)
{
    if (!pCtx->hWnd)
        return VINF_SUCCESS;

    const PVBOXCLIPBOARDWINAPINEW pAPI = &pCtx->newAPI;

    BOOL fRc;
    if (VBoxClipboardWinIsNewAPI(pAPI))
    {
        fRc = pAPI->pfnRemoveClipboardFormatListener(pCtx->hWnd);
    }
    else
    {
        fRc = ChangeClipboardChain(pCtx->hWnd, pCtx->hWndNextInChain);
        if (fRc)
            pCtx->hWndNextInChain = NULL;
    }

    int rc = VINF_SUCCESS;

    if (!fRc)
    {
        const DWORD dwLastErr = GetLastError();
        rc = RTErrConvertFromWin32(dwLastErr);
        LogFunc(("Failed with %Rrc (0x%x)\n", rc, dwLastErr));
    }

    return rc;
}

/**
 * Callback which is invoked when we have successfully pinged ourselves down the
 * clipboard chain.  We simply unset a boolean flag to say that we are responding.
 * There is a race if a ping returns after the next one is initiated, but nothing
 * very bad is likely to happen.
 *
 * @param   hWnd                Window handle to use for this callback. Not used currently.
 * @param   uMsg                Message to handle. Not used currently.
 * @param   dwData              Pointer to user-provided data. Contains our Windows clipboard context.
 * @param   lResult             Additional data to pass. Not used currently.
 */
VOID CALLBACK VBoxClipboardWinChainPingProc(HWND hWnd, UINT uMsg, ULONG_PTR dwData, LRESULT lResult)
{
    RT_NOREF(hWnd);
    RT_NOREF(uMsg);
    RT_NOREF(lResult);

    /** @todo r=andy Why not using SetWindowLongPtr for keeping the context? */
    PVBOXCLIPBOARDWINCTX pCtx = (PVBOXCLIPBOARDWINCTX)dwData;
    AssertPtrReturnVoid(pCtx);

    pCtx->oldAPI.fCBChainPingInProcess = FALSE;
}

/**
 * Converts a (registered or standard) Windows clipboard format to a VBox clipboard format.
 *
 * @returns Converted VBox clipboard format, or VBOX_SHARED_CLIPBOARD_FMT_NONE if not found.
 * @param   uFormat             Windows clipboard format to convert.
 */
VBOXCLIPBOARDFORMAT VBoxClipboardWinClipboardFormatToVBox(UINT uFormat)
{
    /* Insert the requested clipboard format data into the clipboard. */
    VBOXCLIPBOARDFORMAT vboxFormat = VBOX_SHARED_CLIPBOARD_FMT_NONE;

    switch (uFormat)
    {
        case CF_UNICODETEXT:
            vboxFormat = VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT;
            break;

        case CF_DIB:
            vboxFormat = VBOX_SHARED_CLIPBOARD_FMT_BITMAP;
            break;

#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
        /* Handles file system entries which are locally present
         * on source for transferring to the target. */
        case CF_HDROP:
            vboxFormat = VBOX_SHARED_CLIPBOARD_FMT_URI_LIST;
            break;
#endif

        default:
            if (uFormat >= 0xC000) /** Formats registered with RegisterClipboardFormat() start at this index. */
            {
                TCHAR szFormatName[256]; /** @todo r=andy Do we need Unicode support here as well? */
                int cActual = GetClipboardFormatName(uFormat, szFormatName, sizeof(szFormatName) / sizeof(TCHAR));
                if (cActual)
                {
                    LogFlowFunc(("uFormat=%u -> szFormatName=%s\n", uFormat, szFormatName));

                    if (RTStrCmp(szFormatName, VBOX_CLIPBOARD_WIN_REGFMT_HTML) == 0)
                        vboxFormat = VBOX_SHARED_CLIPBOARD_FMT_HTML;
                }
            }
            break;
    }

    LogFlowFunc(("uFormat=%u -> vboxFormat=0x%x\n", uFormat, vboxFormat));
    return vboxFormat;
}

/**
 * Retrieves all supported clipboard formats of a specific clipboard.
 *
 * @returns VBox status code.
 * @param   pCtx                Windows clipboard context to retrieve formats for.
 * @param   pfFormats           Where to store the retrieved formats of type VBOX_SHARED_CLIPBOARD_FMT_ (bitmask).
 */
int VBoxClipboardWinGetFormats(PVBOXCLIPBOARDWINCTX pCtx, PVBOXCLIPBOARDFORMATS pfFormats)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);
    AssertPtrReturn(pfFormats, VERR_INVALID_POINTER);

    VBOXCLIPBOARDFORMATS fFormats = VBOX_SHARED_CLIPBOARD_FMT_NONE;

    /* Query list of available formats and report to host. */
    int rc = VBoxClipboardWinOpen(pCtx->hWnd);
    if (RT_SUCCESS(rc))
    {
        UINT uCurFormat = 0; /* Must be set to zero for EnumClipboardFormats(). */
        while ((uCurFormat = EnumClipboardFormats(uCurFormat)) != 0)
            fFormats |= VBoxClipboardWinClipboardFormatToVBox(uCurFormat);

        int rc2 = VBoxClipboardWinClose();
        AssertRC(rc2);
    }

    if (RT_FAILURE(rc))
    {
        LogFunc(("Failed with rc=%Rrc\n", rc));
    }
    else
    {
        LogFlowFunc(("pfFormats=0x%08X\n", pfFormats));
        *pfFormats = fFormats;
    }

    return rc;
}

/**
 * Extracts a field value from CF_HTML data.
 *
 * @returns VBox status code.
 * @param   pszSrc      source in CF_HTML format.
 * @param   pszOption   Name of CF_HTML field.
 * @param   puValue     Where to return extracted value of CF_HTML field.
 */
int VBoxClipboardWinGetCFHTMLHeaderValue(const char *pszSrc, const char *pszOption, uint32_t *puValue)
{
    AssertPtrReturn(pszSrc, VERR_INVALID_POINTER);
    AssertPtrReturn(pszOption, VERR_INVALID_POINTER);

    int rc = VERR_INVALID_PARAMETER;

    const char *pszOptionValue = RTStrStr(pszSrc, pszOption);
    if (pszOptionValue)
    {
        size_t cchOption = strlen(pszOption);
        Assert(cchOption);

        rc = RTStrToUInt32Ex(pszOptionValue + cchOption, NULL, 10, puValue);
    }
    return rc;
}

/**
 * Check that the source string contains CF_HTML struct.
 *
 * @returns @c true if the @a pszSource string is in CF_HTML format.
 * @param   pszSource   Source string to check.
 */
bool VBoxClipboardWinIsCFHTML(const char *pszSource)
{
    return    RTStrStr(pszSource, "Version:") != NULL
           && RTStrStr(pszSource, "StartHTML:") != NULL;
}

/**
 * Converts clipboard data from CF_HTML format to MIME clipboard format.
 *
 * Returns allocated buffer that contains html converted to text/html mime type
 *
 * @returns VBox status code.
 * @param   pszSource   The input.
 * @param   cch         The length of the input.
 * @param   ppszOutput  Where to return the result.  Free using RTMemFree.
 * @param   pcbOutput   Where to the return length of the result (bytes/chars).
 */
int VBoxClipboardWinConvertCFHTMLToMIME(const char *pszSource, const uint32_t cch, char **ppszOutput, uint32_t *pcbOutput)
{
    Assert(pszSource);
    Assert(cch);
    Assert(ppszOutput);
    Assert(pcbOutput);

    uint32_t offStart;
    int rc = VBoxClipboardWinGetCFHTMLHeaderValue(pszSource, "StartFragment:", &offStart);
    if (RT_SUCCESS(rc))
    {
        uint32_t offEnd;
        rc = VBoxClipboardWinGetCFHTMLHeaderValue(pszSource, "EndFragment:", &offEnd);
        if (RT_SUCCESS(rc))
        {
            if (   offStart > 0
                && offEnd > 0
                && offEnd > offStart
                && offEnd <= cch)
            {
                uint32_t cchSubStr = offEnd - offStart;
                char *pszResult = (char *)RTMemAlloc(cchSubStr + 1);
                if (pszResult)
                {
                    rc = RTStrCopyEx(pszResult, cchSubStr + 1, pszSource + offStart, cchSubStr);
                    if (RT_SUCCESS(rc))
                    {
                        *ppszOutput = pszResult;
                        *pcbOutput  = (uint32_t)(cchSubStr + 1);
                        rc = VINF_SUCCESS;
                    }
                    else
                    {
                        LogRelFlowFunc(("Error: Unknown CF_HTML format. Expected EndFragment. rc = %Rrc\n", rc));
                        RTMemFree(pszResult);
                    }
                }
                else
                {
                    LogRelFlowFunc(("Error: Unknown CF_HTML format. Expected EndFragment\n"));
                    rc = VERR_NO_MEMORY;
                }
            }
            else
            {
                LogRelFlowFunc(("Error: CF_HTML out of bounds - offStart=%#x offEnd=%#x cch=%#x\n", offStart, offEnd, cch));
                rc = VERR_INVALID_PARAMETER;
            }
        }
        else
        {
            LogRelFlowFunc(("Error: Unknown CF_HTML format. Expected EndFragment. rc = %Rrc\n", rc));
            rc = VERR_INVALID_PARAMETER;
        }
    }
    else
    {
        LogRelFlowFunc(("Error: Unknown CF_HTML format. Expected StartFragment. rc = %Rrc\n", rc));
        rc = VERR_INVALID_PARAMETER;
    }

    return rc;
}

/**
 * Converts source UTF-8 MIME HTML clipboard data to UTF-8 CF_HTML format.
 *
 * This is just encapsulation work, slapping a header on the data.
 *
 * It allocates [..]
 *
 * Calculations:
 *   Header length = format Length + (2*(10 - 5('%010d'))('digits')) - 2('%s') = format length + 8
 *   EndHtml       = Header length + fragment length
 *   StartHtml     = 105(constant)
 *   StartFragment = 141(constant) may vary if the header html content will be extended
 *   EndFragment   = Header length + fragment length - 38(ending length)
 *
 * @param   pszSource   Source buffer that contains utf-16 string in mime html format
 * @param   cb          Size of source buffer in bytes
 * @param   ppszOutput  Where to return the allocated output buffer to put converted UTF-8
 *                      CF_HTML clipboard data.  This function allocates memory for this.
 * @param   pcbOutput   Where to return the size of allocated result buffer in bytes/chars, including zero terminator
 *
 * @note    output buffer should be free using RTMemFree()
 * @note    Everything inside of fragment can be UTF8. Windows allows it. Everything in header should be Latin1.
 */
int VBoxClipboardWinConvertMIMEToCFHTML(const char *pszSource, size_t cb, char **ppszOutput, uint32_t *pcbOutput)
{
    Assert(ppszOutput);
    Assert(pcbOutput);
    Assert(pszSource);
    Assert(cb);

    /* construct CF_HTML formatted string */
    char *pszResult = NULL;
    size_t cchFragment;
    int rc = RTStrNLenEx(pszSource, cb, &cchFragment);
    if (!RT_SUCCESS(rc))
    {
        LogRelFlowFunc(("Error: invalid source fragment. rc = %Rrc\n"));
        return VERR_INVALID_PARAMETER;
    }

    /*
    @StartHtml - pos before <html>
    @EndHtml - whole size of text excluding ending zero char
    @StartFragment - pos after <!--StartFragment-->
    @EndFragment - pos before <!--EndFragment-->
    @note: all values includes CR\LF inserted into text
    Calculations:
    Header length = format Length + (3*6('digits')) - 2('%s') = format length + 16 (control value - 183)
    EndHtml  = Header length + fragment length
    StartHtml = 105(constant)
    StartFragment = 143(constant)
    EndFragment  = Header length + fragment length - 40(ending length)
    */
    static const char s_szFormatSample[] =
    /*   0:   */ "Version:1.0\r\n"
    /*  13:   */ "StartHTML:000000101\r\n"
    /*  34:   */ "EndHTML:%0000009u\r\n" // END HTML = Header length + fragment length
    /*  53:   */ "StartFragment:000000137\r\n"
    /*  78:   */ "EndFragment:%0000009u\r\n"
    /* 101:   */ "<html>\r\n"
    /* 109:   */ "<body>\r\n"
    /* 117:   */ "<!--StartFragment-->"
    /* 137:   */ "%s"
    /* 137+2: */ "<!--EndFragment-->\r\n"
    /* 157+2: */ "</body>\r\n"
    /* 166+2: */ "</html>\r\n";
    /* 175+2: */
    AssertCompile(sizeof(s_szFormatSample) == 175 + 2 + 1);

    /* calculate parameters of CF_HTML header */
    size_t cchHeader      = sizeof(s_szFormatSample) - 1;
    size_t offEndHtml     = cchHeader + cchFragment;
    size_t offEndFragment = cchHeader + cchFragment - 38; /* 175-137 = 38 */
    pszResult = (char *)RTMemAlloc(offEndHtml + 1);
    if (pszResult == NULL)
    {
        LogRelFlowFunc(("Error: Cannot allocate memory for result buffer. rc = %Rrc\n"));
        return VERR_NO_MEMORY;
    }

    /* format result CF_HTML string */
    size_t cchFormatted = RTStrPrintf(pszResult, offEndHtml + 1,
                                      s_szFormatSample, offEndHtml, offEndFragment, pszSource);
    Assert(offEndHtml == cchFormatted); NOREF(cchFormatted);

#ifdef VBOX_STRICT
    /* Control calculations. check consistency.*/
    static const char s_szStartFragment[] = "<!--StartFragment-->";
    static const char s_szEndFragment[] = "<!--EndFragment-->";

    /* check 'StartFragment:' value */
    const char *pszRealStartFragment = RTStrStr(pszResult, s_szStartFragment);
    Assert(&pszRealStartFragment[sizeof(s_szStartFragment) - 1] - pszResult == 137);

    /* check 'EndFragment:' value */
    const char *pszRealEndFragment = RTStrStr(pszResult, s_szEndFragment);
    Assert((size_t)(pszRealEndFragment - pszResult) == offEndFragment);
#endif

    *ppszOutput = pszResult;
    *pcbOutput = (uint32_t)cchFormatted + 1;
    Assert(*pcbOutput == cchFormatted + 1);

    return VINF_SUCCESS;
}

#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
/**
 * Converts a DROPFILES (HDROP) structure to a string list, separated by \r\n.
 * Does not do any locking on the input data.
 *
 * @returns VBox status code.
 * @param   pDropFiles          Pointer to DROPFILES structure to convert.
 * @param   ppszData            Where to return the converted (allocated) data on success.
 *                              Must be free'd by the caller with RTMemFree().
 * @param   pcbData             Size (in bytes) of the allocated data returned.
 */
int VBoxClipboardWinDropFilesToStringList(DROPFILES *pDropFiles, char **ppszbData, size_t *pcbData)
{
    AssertPtrReturn(pDropFiles, VERR_INVALID_POINTER);
    AssertPtrReturn(ppszbData,  VERR_INVALID_POINTER);
    AssertPtrReturn(pcbData,    VERR_INVALID_POINTER);

    /* Do we need to do Unicode stuff? */
    const bool fUnicode = RT_BOOL(pDropFiles->fWide);

    /* Get the offset of the file list. */
    Assert(pDropFiles->pFiles >= sizeof(DROPFILES));

    /* Note: This is *not* pDropFiles->pFiles! DragQueryFile only
     *       will work with the plain storage medium pointer! */
    HDROP hDrop = (HDROP)(pDropFiles);

    int rc = VINF_SUCCESS;

    /* First, get the file count. */
    /** @todo Does this work on Windows 2000 / NT4? */
    char *pszFiles = NULL;
    uint32_t cchFiles = 0;
    UINT cFiles = DragQueryFile(hDrop, UINT32_MAX /* iFile */, NULL /* lpszFile */, 0 /* cchFile */);

    LogFlowFunc(("Got %RU16 file(s), fUnicode=%RTbool\n", cFiles, fUnicode));

    for (UINT i = 0; i < cFiles; i++)
    {
        UINT cchFile = DragQueryFile(hDrop, i /* File index */, NULL /* Query size first */, 0 /* cchFile */);
        Assert(cchFile);

        if (RT_FAILURE(rc))
            break;

        char *pszFileUtf8 = NULL; /* UTF-8 version. */
        UINT cchFileUtf8 = 0;
        if (fUnicode)
        {
            /* Allocate enough space (including terminator). */
            WCHAR *pwszFile = (WCHAR *)RTMemAlloc((cchFile + 1) * sizeof(WCHAR));
            if (pwszFile)
            {
                const UINT cwcFileUtf16 = DragQueryFileW(hDrop, i /* File index */,
                                                         pwszFile, cchFile + 1 /* Include terminator */);

                AssertMsg(cwcFileUtf16 == cchFile, ("cchFileUtf16 (%RU16) does not match cchFile (%RU16)\n",
                                                    cwcFileUtf16, cchFile));
                RT_NOREF(cwcFileUtf16);

                rc = RTUtf16ToUtf8(pwszFile, &pszFileUtf8);
                if (RT_SUCCESS(rc))
                {
                    cchFileUtf8 = (UINT)strlen(pszFileUtf8);
                    Assert(cchFileUtf8);
                }

                RTMemFree(pwszFile);
            }
            else
                rc = VERR_NO_MEMORY;
        }
        else /* ANSI */
        {
            /* Allocate enough space (including terminator). */
            pszFileUtf8 = (char *)RTMemAlloc((cchFile + 1) * sizeof(char));
            if (pszFileUtf8)
            {
                cchFileUtf8 = DragQueryFileA(hDrop, i /* File index */,
                                             pszFileUtf8, cchFile + 1 /* Include terminator */);

                AssertMsg(cchFileUtf8 == cchFile, ("cchFileUtf8 (%RU16) does not match cchFile (%RU16)\n",
                                                   cchFileUtf8, cchFile));
            }
            else
                rc = VERR_NO_MEMORY;
        }

        if (RT_SUCCESS(rc))
        {
            LogFlowFunc(("\tFile: %s (cchFile=%RU16)\n", pszFileUtf8, cchFileUtf8));

            LogRel(("DnD: Adding guest file '%s'\n", pszFileUtf8));

            rc = RTStrAAppendExN(&pszFiles, 1 /* cPairs */, pszFileUtf8, cchFileUtf8);
            if (RT_SUCCESS(rc))
                cchFiles += cchFileUtf8;
        }
        else
            LogFunc(("Error handling file entry #%u, rc=%Rrc\n", i, rc));

        if (pszFileUtf8)
            RTStrFree(pszFileUtf8);

        if (RT_FAILURE(rc))
            break;

        /* Add separation between filenames.
         * Note: Also do this for the last element of the list. */
        rc = RTStrAAppendExN(&pszFiles, 1 /* cPairs */, "\r\n", 2 /* Bytes */);
        if (RT_SUCCESS(rc))
            cchFiles += 2; /* Include \r\n */
    }

    if (RT_SUCCESS(rc))
    {
        cchFiles += 1; /* Add string termination. */
        uint32_t cbFiles = cchFiles * sizeof(char);

        LogFlowFunc(("cFiles=%u, cchFiles=%RU32, cbFiles=%RU32, pszFiles=0x%p\n",
                     cFiles, cchFiles, cbFiles, pszFiles));

        /* Translate the list into URI elements. */
        SharedClipboardURIList lstURI;
        rc = lstURI.AppendNativePathsFromList(pszFiles, cbFiles,
                                              SHAREDCLIPBOARDURILIST_FLAGS_ABSOLUTE_PATHS);
        if (RT_SUCCESS(rc))
        {
            RTCString strRoot = lstURI.GetRootEntries();
            size_t cbRoot = strRoot.length() + 1; /* Include termination */

            void *pvData = RTMemAlloc(cbRoot);
            if (pvData)
            {
                memcpy(pvData, strRoot.c_str(), cbRoot);

                *ppszbData = (char *)pvData;
                *pcbData   = cbRoot;
            }
            else
                rc = VERR_NO_MEMORY;
        }
    }

    LogFlowFunc(("Building CF_HDROP list rc=%Rrc, pszFiles=0x%p, cFiles=%RU16, cchFiles=%RU32\n",
                 rc, pszFiles, cFiles, cchFiles));

    if (pszFiles)
        RTStrFree(pszFiles);

    return rc;
}
#endif /* VBOX_WITH_SHARED_CLIPBOARD_URI_LIST */

