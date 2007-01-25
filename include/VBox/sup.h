/** @file
 * SUP - Support Library.
 */

/*
 * Copyright (C) 2006 InnoTek Systemberatung GmbH
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * in version 2 as it comes in the "COPYING" file of the VirtualBox OSE
 * distribution. VirtualBox OSE is distributed in the hope that it will
 * be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * If you received this file as part of a commercial VirtualBox
 * distribution, then only the terms of your commercial VirtualBox
 * license agreement apply instead of the previous paragraph.
 */

#ifndef __VBox_sup_h__
#define __VBox_sup_h__

#include <VBox/cdefs.h>
#include <VBox/types.h>

__BEGIN_DECLS

/** @defgroup   grp_sup     The Support Library API
 * @{
 */

/** Physical page.
 */
#pragma pack(4) /* space is more important. */
typedef struct SUPPAGE
{
    /** Physical memory address. */
    RTHCPHYS        Phys;
    /** Reserved entry for internal use by the caller. */
    RTHCUINTPTR     uReserved;
} SUPPAGE, *PSUPPAGE;
#pragma pack()

/**
 * The paging mode.
 */
typedef enum SUPPAGINGMODE
{
    /** The usual invalid entry.
     * This is returned by SUPGetPagingMode()  */
    SUPPAGINGMODE_INVALID = 0,
    /** Normal 32-bit paging, no global pages */
    SUPPAGINGMODE_32_BIT,
    /** Normal 32-bit paging with global pages. */
    SUPPAGINGMODE_32_BIT_GLOBAL,
    /** PAE mode, no global pages, no NX. */
    SUPPAGINGMODE_PAE,
    /** PAE mode with global pages. */
    SUPPAGINGMODE_PAE_GLOBAL,
    /** PAE mode with NX, no global pages. */
    SUPPAGINGMODE_PAE_NX,
    /** PAE mode with global pages and NX. */
    SUPPAGINGMODE_PAE_GLOBAL_NX,
    /** AMD64 mode, no global pages. */
    SUPPAGINGMODE_AMD64,
    /** AMD64 mode with global pages, no NX. */
    SUPPAGINGMODE_AMD64_GLOBAL,
    /** AMD64 mode with NX, no global pages. */
    SUPPAGINGMODE_AMD64_NX,
    /** AMD64 mode with global pages and NX. */
    SUPPAGINGMODE_AMD64_GLOBAL_NX
} SUPPAGINGMODE;


#pragma pack(1)
/**
 * Global Information Page.
 *
 * This page contains useful information and can be mapped into any
 * process or VM. It can be accessed thru the g_pSUPGlobalInfoPage
 * pointer when a session is open.
 *
 * How to read data from this structure:
 *
 * @code
 *      PSUPGLOBALINFOPAGE pGIP;
 *      uint32_t u32TransactionId;
 *      do
 *      {
 *          pGIP = g_pSUPGlobalInfoPage;
 *          if (!pGIP)
 *              return VERR_GIP_NOT_PRESENT;
 *          u32TransactionId = pGIP->u32TransactionId;
 *          ... read stuff to local variables ...
 *      } while (   pGIP->u32TransactionId != u32TransactionId
 *               || (u32TransactionId & 1));
 * @endcode
 *
 * @remark This is currently an optional feature.
 */
typedef struct SUPGLOBALINFOPAGE
{
    /** Magic (SUPGLOBALINFOPAGE_MAGIC). */
    uint32_t            u32Magic;

    /** The update frequency of the of the NanoTS. */
    volatile uint32_t   u32UpdateHz;
    /** The update interval in nanoseconds. (10^9 / u32UpdateHz) */
    volatile uint32_t   u32UpdateIntervalNS;

    /** Padding / reserved space for future const variables. */
    uint32_t            au32Padding0[5];

    /** Update transaction number.
     * This number is incremented at the start and end of each update. It follows
     * thusly that odd numbers indicates update in progress, while even numbers
     * indicate stable data. Use this to make sure that the data items you fetch
     * are consistent.
     */
    volatile uint32_t   u32TransactionId;
    /** The interval in TSC ticks between two NanoTS updates.
     * This is the average interval over the last 2, 4 or 8 updates + a little slack.
     * The slack makes the time go a tiny tiny bit slower and extends the interval enough
     * to avoid getting ending up with too many 1ns increments. */
    volatile uint32_t   u32UpdateIntervalTSC;
    /** Current nanosecond timestamp. */
    volatile uint64_t   u64NanoTS;
    /** The TSC at the time of u64NanoTS. */
    volatile uint64_t   u64TSC;
    /** Current CPU Frequency. */
    volatile uint64_t   u64CpuHz;
    /** Number of errors during updating.
     * Typical errors are under/overflows. */
    volatile uint32_t   cErrors;
    /** Index of the head item in au32TSCHistory. */
    volatile uint32_t   iTSCHistoryHead;
    /** Array of recent TSC interval deltas.
     * The most recent item is at index iTSCHistoryHead.
     * This history is used to calculate u32UpdateIntervalTSC.
     */
    volatile uint32_t   au32TSCHistory[8];
    /** The timestamp of the last time we update the update frequency. */
    volatile uint64_t   u64NanoTSLastUpdateHz;
} SUPGLOBALINFOPAGE;
#pragma pack()
/** Pointer to the global info page. */
typedef SUPGLOBALINFOPAGE *PSUPGLOBALINFOPAGE;
/** Const pointer to the global info page. */
typedef const SUPGLOBALINFOPAGE *PCSUPGLOBALINFOPAGE;

/** Current value of the SUPGLOBALINFOPAGE::u32Magic field. */
#define SUPGLOBALINFOPAGE_MAGIC     (0xbeef0001)

/** Pointer to the Global Information Page.
 *
 * This pointer is valid as long as SUPLib has a open session. Anyone using
 * the page must treat this pointer as higly volatile and not trust it beyond
 * one transaction.
 */
#if defined(IN_SUP_R0) || defined(IN_SUP_R3) || defined(IN_SUP_GC)
extern DECLEXPORT(PCSUPGLOBALINFOPAGE)  g_pSUPGlobalInfoPage;
#elif defined(IN_RING0)
extern DECLIMPORT(const SUPGLOBALINFOPAGE) g_SUPGlobalInfoPage;
# if defined(__GNUC__) && !defined(__DARWIN__) && defined(__AMD64__)
/** Workaround for ELF+GCC problem on 64-bit hosts.
 * (GCC emits a mov with a R_X86_64_32 reloc, we need R_X86_64_64.) */
DECLINLINE(PCSUPGLOBALINFOPAGE) SUPGetGIP(void)
{
    PCSUPGLOBALINFOPAGE pGIP;
    __asm__ __volatile__ ("movabs g_SUPGlobalInfoPage,%0\n\t" 
                          : "=a" (pGIP));
    return pGIP;
}
#  define g_pSUPGlobalInfoPage         (SUPGetGIP())
# else
#  define g_pSUPGlobalInfoPage         (&g_SUPGlobalInfoPage)
#endif 
#else
extern DECLIMPORT(PCSUPGLOBALINFOPAGE)  g_pSUPGlobalInfoPage;
#endif



#ifdef IN_RING3

/** @defgroup   grp_sup_r3     SUP Host Context Ring 3 API
 * @ingroup grp_sup
 * @{
 */

/**
 * Installs the support library.
 *
 * @returns VBox status code.
 */
SUPR3DECL(int) SUPInstall(void);

/**
 * Uninstalls the support library.
 *
 * @returns VBox status code.
 */
SUPR3DECL(int) SUPUninstall(void);

/**
 * Initializes the support library.
 * Each succesful call to SUPInit() must be countered by a
 * call to SUPTerm(false).
 *
 * @returns VBox status code.
 * @param   ppSession       Where to store the session handle. Defaults to NULL.
 * @param   cbReserve       The number of bytes of contiguous memory that should be reserved by
 *                          the runtime / support library.
 *                          Set this to 0 if no reservation is required. (default)
 *                          Set this to ~0 if the maximum amount supported by the VM is to be
 *                          attempted reserved, or the maximum available.
 */
#ifdef __cplusplus
SUPR3DECL(int) SUPInit(PSUPDRVSESSION *ppSession = NULL, size_t cbReserve = 0);
#else
SUPR3DECL(int) SUPInit(PSUPDRVSESSION *ppSession, size_t cbReserve);
#endif

/**
 * Terminates the support library.
 *
 * @returns VBox status code.
 * @param   fForced     Forced termination. This means to ignore the
 *                      init call count and just terminated.
 */
#ifdef __cplusplus
SUPR3DECL(int) SUPTerm(bool fForced = false);
#else
SUPR3DECL(int) SUPTerm(int fForced);
#endif

/**
 * Sets the ring-0 VM handle for use with fast IOCtls.
 *
 * @returns VBox status code.
 * @param   pVMR0       The ring-0 VM handle.
 *                      NIL_RTR0PTR can be used to unset the handle when the
 *                      VM is about to be destroyed.
 */
SUPR3DECL(int) SUPSetVMForFastIOCtl(PVMR0 pVMR0);

/**
 * Calls the HC R0 VMM entry point.
 * See VMMR0Entry() for more details.
 *
 * @returns error code specific to uFunction.
 * @param   pVM         Pointer to the Host Context mapping of the VM structure.
 * @param   uOperation  Operation to execute.
 * @param   pvArg       Argument.
 */
SUPR3DECL(int) SUPCallVMMR0(PVM pVM, unsigned uOperation, void *pvArg);

/**
 * Calls the HC R0 VMM entry point, in a safer but slower manner than SUPCallVMMR0.
 * When entering using this call the R0 components can call into the host kernel
 * (i.e. use the SUPR0 and RT APIs).
 *
 * See VMMR0Entry() for more details.
 *
 * @returns error code specific to uFunction.
 * @param   pVM         Pointer to the Host Context mapping of the VM structure.
 * @param   uOperation  Operation to execute.
 * @param   pvArg       Pointer to argument structure or if cbArg is 0 just an value.
 * @param   cbArg       The size of the argument. This is used to copy whatever the argument
 *                      points at into a kernel buffer to avoid problems like the user page
 *                      being invalidated while we're executing the call.
 */
SUPR3DECL(int) SUPCallVMMR0Ex(PVM pVM, unsigned uOperation, void *pvArg, unsigned cbArg);

/**
 * Queries the paging mode of the host OS.
 *
 * @returns The paging mode.
 */
SUPR3DECL(SUPPAGINGMODE) SUPGetPagingMode(void);

/**
 * Allocate zero-filled pages.
 *
 * Use this to allocate a number of pages rather than using RTMem*() and mess with
 * alignment. The returned address is of course page aligned. Call SUPPageFree()
 * to free the pages once done with them.
 *
 * @returns VBox status.
 * @param   cPages      Number of x86 4KB pages to allocate.
 *                      Max of 32MB.
 * @param   ppvPages    Where to store the base pointer to the allocated pages.
 */
SUPR3DECL(int) SUPPageAlloc(size_t cPages, void **ppvPages);

/**
 * Frees pages allocated with SUPPageAlloc().
 *
 * @returns VBox status.
 * @param   pvPages     Pointer returned by SUPPageAlloc().
 */
SUPR3DECL(int) SUPPageFree(void *pvPages);

/**
 * Locks down the physical memory backing a virtual memory
 * range in the current process.
 *
 * @returns VBox status code.
 * @param   pvStart         Start of virtual memory range.
 *                          Must be page aligned.
 * @param   cbMemory        Number of bytes.
 *                          Must be page aligned.
 * @param   paPages         Where to store the physical page addresses returned.
 *                          On entry this will point to an array of with cbMemory >> PAGE_SHIFT entries.
 */
SUPR3DECL(int) SUPPageLock(void *pvStart, size_t cbMemory, PSUPPAGE paPages);

/**
 * Releases locked down pages.
 *
 * @returns VBox status code.
 * @param   pvStart         Start of virtual memory range previously locked
 *                          down by SUPPageLock().
 */
SUPR3DECL(int) SUPPageUnlock(void *pvStart);

/**
 * Allocated memory with page aligned memory with a contiguous and locked physical
 * memory backing below 4GB.
 *
 * @returns Pointer to the allocated memory (virtual address).
 *          *pHCPhys is set to the physical address of the memory.
 *          The returned memory must be freed using SUPContFree().
 * @returns NULL on failure.
 * @param   cb          Number of bytes to allocate.
 * @param   pHCPhys     Where to store the physical address of the memory block.
 */
SUPR3DECL(void *) SUPContAlloc(unsigned cb, PRTHCPHYS pHCPhys);

/**
 * Allocated memory with page aligned memory with a contiguous and locked physical
 * memory backing below 4GB.
 *
 * @returns Pointer to the allocated memory (virtual address).
 *          *pHCPhys is set to the physical address of the memory.
 *          If ppvR0 isn't NULL, *ppvR0 is set to the ring-0 mapping.
 *          The returned memory must be freed using SUPContFree().
 * @returns NULL on failure.
 * @param   cb          Number of bytes to allocate.
 * @param   ppvR0       Where to store the ring-0 mapping of the allocation. (optional)
 * @param   pHCPhys     Where to store the physical address of the memory block.
 *
 * @remark  This 2nd version of this API exists because we're not able to map the
 *          ring-3 mapping executable on WIN64. This is a serious problem in regard to
 *          the world switchers.
 */
SUPR3DECL(void *) SUPContAlloc2(unsigned cb, void **ppvR0, PRTHCPHYS pHCPhys);

/**
 * Frees memory allocated with SUPContAlloc().
 *
 * @returns VBox status code.
 * @param   pv      Pointer to the memory block which should be freed.
 */
SUPR3DECL(int) SUPContFree(void *pv);

/**
 * Allocated non contiguous physical memory below 4GB.
 *
 * @returns VBox status code.
 * @returns NULL on failure.
 * @param   cPages      Number of pages to allocate.
 * @param   ppvPages    Where to store the pointer to the allocated memory.
 *                      The pointer stored here on success must be passed to SUPLowFree when
 *                      the memory should be released.
 * @param   paPages     Where to store the physical addresses of the individual pages.
 */
SUPR3DECL(int) SUPLowAlloc(unsigned cPages, void **ppvPages, PSUPPAGE paPages);

/**
 * Frees memory allocated with SUPLowAlloc().
 *
 * @returns VBox status code.
 * @param   pv      Pointer to the memory block which should be freed.
 */
SUPR3DECL(int) SUPLowFree(void *pv);

/**
 * Load a module into R0 HC.
 *
 * @returns VBox status code.
 * @param   pszFilename     The path to the image file.
 * @param   pszModule       The module name. Max 32 bytes.
 */
SUPR3DECL(int) SUPLoadModule(const char *pszFilename, const char *pszModule, void **ppvImageBase);

/**
 * Frees a R0 HC module.
 *
 * @returns VBox status code.
 * @param   pszModule       The module to free.
 * @remark  This will not actually 'free' the module, there are of course usage counting.
 */
SUPR3DECL(int) SUPFreeModule(void *pvImageBase);

/**
 * Get the address of a symbol in a ring-0 module.
 *
 * @returns VBox status code.
 * @param   pszModule       The module name.
 * @param   pszSymbol       Symbol name. If it's value is less than 64k it's treated like a
 *                          ordinal value rather than a string pointer.
 * @param   ppvValue        Where to store the symbol value.
 */
SUPR3DECL(int) SUPGetSymbolR0(void *pvImageBase, const char *pszSymbol, void **ppvValue);

/**
 * Load R0 HC VMM code.
 *
 * @returns VBox status code.
 * @deprecated  Use SUPLoadModule(pszFilename, "VMMR0.r0", &pvImageBase)
 */
SUPR3DECL(int) SUPLoadVMM(const char *pszFilename);

/**
 * Unloads R0 HC VMM code.
 *
 * @returns VBox status code.
 * @deprecated  Use SUPFreeModule().
 */
SUPR3DECL(int) SUPUnloadVMM(void);

/**
 * Get the physical address of the GIP.
 *
 * @returns VBox status code.
 * @param   pHCPhys     Where to store the physical address of the GIP.
 */
SUPR3DECL(int) SUPGipGetPhys(PRTHCPHYS pHCPhys);

/** @} */
#endif /* IN_RING3 */


#ifdef IN_RING0
/** @defgroup   grp_sup_r0     SUP Host Context Ring 0 API
 * @ingroup grp_sup
 * @{
 */

/**
 * Security objectype.
 */
typedef enum SUPDRVOBJTYPE
{
    /** The usual invalid object. */
    SUPDRVOBJTYPE_INVALID = 0,
    /** Internal network. */
    SUPDRVOBJTYPE_INTERNAL_NETWORK,
    /** Internal network interface. */
    SUPDRVOBJTYPE_INTERNAL_NETWORK_INTERFACE,
    /** The first invalid object type in this end. */
    SUPDRVOBJTYPE_END,
    /** The usual 32-bit type size hack. */
    SUPDRVOBJTYPE_32_BIT_HACK = 0x7ffffff
} SUPDRVOBJTYPE;

/**
 * Object destructor callback.
 * This is called for reference counted objectes when the count reaches 0.
 *
 * @param   pvObj       The object pointer.
 * @param   pvUser1     The first user argument.
 * @param   pvUser2     The second user argument.
 */
typedef DECLCALLBACK(void) FNSUPDRVDESTRUCTOR(void *pvObj, void *pvUser1, void *pvUser2);
/** Pointer to a FNSUPDRVDESTRUCTOR(). */
typedef FNSUPDRVDESTRUCTOR *PFNSUPDRVDESTRUCTOR;

SUPR0DECL(void *) SUPR0ObjRegister(PSUPDRVSESSION pSession, SUPDRVOBJTYPE enmType, PFNSUPDRVDESTRUCTOR pfnDestructor, void *pvUser1, void *pvUser2);
SUPR0DECL(int) SUPR0ObjAddRef(void *pvObj, PSUPDRVSESSION pSession);
SUPR0DECL(int) SUPR0ObjRelease(void *pvObj, PSUPDRVSESSION pSession);
SUPR0DECL(int) SUPR0ObjVerifyAccess(void *pvObj, PSUPDRVSESSION pSession, const char *pszObjName);

SUPR0DECL(int) SUPR0LockMem(PSUPDRVSESSION pSession, void *pvR3, unsigned cb, PSUPPAGE paPages);
SUPR0DECL(int) SUPR0UnlockMem(PSUPDRVSESSION pSession, void *pvR3);
SUPR0DECL(int) SUPR0ContAlloc(PSUPDRVSESSION pSession, unsigned cb, void **ppvR0, void **ppvR3, PRTHCPHYS pHCPhys);
SUPR0DECL(int) SUPR0ContFree(PSUPDRVSESSION pSession, void *pv);
SUPR0DECL(int) SUPR0LowAlloc(PSUPDRVSESSION pSession, unsigned cPages, void **ppvR3, PSUPPAGE paPages);
SUPR0DECL(int) SUPR0LowFree(PSUPDRVSESSION pSession, void *pv);
SUPR0DECL(int) SUPR0MemAlloc(PSUPDRVSESSION pSession, unsigned cb, void **ppvR0, void **ppvR3);
SUPR0DECL(int) SUPR0MemGetPhys(PSUPDRVSESSION pSession, void *pv, PSUPPAGE paPages);
SUPR0DECL(int) SUPR0MemFree(PSUPDRVSESSION pSession, void *pv);
SUPR0DECL(int) SUPR0GipMap(PSUPDRVSESSION pSession, PCSUPGLOBALINFOPAGE *ppGip, RTHCPHYS *pHCPhysGid);
SUPR0DECL(int) SUPR0GipUnmap(PSUPDRVSESSION pSession);
SUPR0DECL(int) SUPR0Printf(const char *pszFormat, ...);

/** @} */
#endif

/** @} */

__END_DECLS


#endif

