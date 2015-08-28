/* $Id$ */
/** @file
 * IPRT - Generic RTCrStoreCreateSnapshotById implementation.
 */

/*
 * Copyright (C) 2006-2015 Oracle Corporation
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


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <iprt/crypto/store.h>
#include "internal/iprt.h"

#include <iprt/assert.h>
#include <iprt/err.h>
#include <iprt/file.h>
#include <iprt/dir.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** Unix root prefix. */
#ifdef RT_OS_OS2
# define UNIX_ROOT "/@unixroot@"
#elif defined(RT_OS_WINDOWS)
# define UNIX_ROOT "C:/cygwin"
#else
# define UNIX_ROOT
#endif


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/** System PEM files worth looking at.
 * @remarks Several of these could be symlinks to one of the others.
 */
static const char *g_apszSystemPemFiles[] =
{
    UNIX_ROOT "/etc/ssl/certs/ca-certificates.crt",
    UNIX_ROOT "/etc/ssl/cert.pem",
    UNIX_ROOT "/etc/ca-certificates/extracted/tls-ca-bundle.pem",
    UNIX_ROOT "/etc/ca-certificates/extracted/email-ca-bundle.pem",
    UNIX_ROOT "/etc/ca-certificates/extracted/objsign-ca-bundle.pem",
    UNIX_ROOT "/etc/ca-certificates/extracted/ca-bundle.trust.crt",
    UNIX_ROOT "/etc/ca-certificates/extracted/ca-bundle.trust.crt",
    UNIX_ROOT "/etc/curl/curlCA",
#if 0 /* Just for reference. */
    UNIX_ROOT"/usr/share/ca-certificates/trust-source/mozilla.trust.crt",
    UNIX_ROOT"/usr/share/ca-certificates/trust-source/mozilla.neutral-trust.crt",
# if defined(RT_OS_SOLARIS) /* the only one on tindersol2... */
    UNIX_ROOT"/usr/share/doc/mutt/samples/ca-bundle.crt",
    VeriSign topic: Provide interface for reading: /usr/jdk/latest/jre/lib/security/cacerts ?
# endif
#endif
};

/**
 * System directories containing lots of pem/crt files.
 */
static const char *g_apszSystemPemDirs[] =
{
    UNIX_ROOT "/etc/openssl/certs/",
    UNIX_ROOT "/etc/ssl/certs/",
    UNIX_ROOT "/etc/ca-certificates/extracted/cadir/",
};


RTDECL(int) RTCrStoreCreateSnapshotById(PRTCRSTORE phStore, RTCRSTOREID enmStoreId, PRTERRINFO pErrInfo)
{
    AssertReturn(enmStoreId > RTCRSTOREID_INVALID && enmStoreId < RTCRSTOREID_END, VERR_INVALID_PARAMETER);

    /*
     * Create an empty in-memory store.
     */
    RTCRSTORE hStore;
    uint32_t cExpected = enmStoreId == RTCRSTOREID_SYSTEM_TRUSTED_CAS_AND_CERTIFICATES ? 256 : 0;
    int rc = RTCrStoreCreateInMem(&hStore, cExpected);
    if (RT_SUCCESS(rc))
    {
        *phStore = hStore;

        /*
         * Add system certificates if part of the given store ID.
         */
        bool fFound = false;
        rc = VINF_SUCCESS;
        if (enmStoreId == RTCRSTOREID_SYSTEM_TRUSTED_CAS_AND_CERTIFICATES)
        {
            for (uint32_t i = 0; i < RT_ELEMENTS(g_apszSystemPemFiles); i++)
                if (RTFileExists(g_apszSystemPemFiles[i]))
                {
                    fFound = true;
                    int rc2 = RTCrStoreCertAddFromFile(hStore,
                                                       RTCRCERTCTX_F_ADD_IF_NOT_FOUND | RTCRCERTCTX_F_ADD_CONTINUE_ON_ERROR,
                                                       g_apszSystemPemFiles[i], pErrInfo);
                    if (RT_FAILURE(rc2))
                        rc = -rc2;
                }

            /*
             * If we didn't find any of the certificate collection files, go hunting
             * for directories containing PEM/CRT files with single certificates.
             */
            if (!fFound)
                for (uint32_t i = 0; i < RT_ELEMENTS(g_apszSystemPemDirs); i++)
                    if (RTDirExists(g_apszSystemPemDirs[i]))
                    {
                        static RTSTRTUPLE const s_aSuffixes[] =
                        {
                            { RT_STR_TUPLE(".crt") },
                            { RT_STR_TUPLE(".pem") },
                            { RT_STR_TUPLE(".PEM") },
                            { RT_STR_TUPLE(".CRT") },
                        };
                        fFound = true;
                        int rc2 = RTCrStoreCertAddFromDir(hStore,
                                                          RTCRCERTCTX_F_ADD_IF_NOT_FOUND | RTCRCERTCTX_F_ADD_CONTINUE_ON_ERROR,
                                                          g_apszSystemPemDirs[i], &s_aSuffixes[0], RT_ELEMENTS(s_aSuffixes),
                                                          pErrInfo);
                        if (RT_FAILURE(rc2))
                            rc = -rc2;
                    }
        }
    }
    else
        RTErrInfoSet(pErrInfo, rc, "RTCrStoreCreateInMem failed");
    return rc;
}
RT_EXPORT_SYMBOL(RTCrStoreCreateSnapshotById);

