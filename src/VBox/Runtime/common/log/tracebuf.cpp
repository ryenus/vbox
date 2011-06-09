
/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include "internal/iprt.h"
#include <iprt/trace.h>


#include <iprt/assert.h>
#include <iprt/asm.h>
#include <iprt/err.h>
#include <iprt/log.h>
#ifndef IN_RC
# include <iprt/mem.h>
#endif
#include <iprt/string.h>
#include <iprt/time.h>

#include "internal/magics.h"


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
/** Alignment used to place the trace buffer members, this should be a multiple
 * of the cache line size if possible.  (We should dynamically determine it.) */
#define RTTRACEBUF_ALIGNMENT    64
AssertCompile(RTTRACEBUF_ALIGNMENT >= sizeof(uint64_t) * 2);


/**
 * The volatile trace buffer members.
 */
typedef struct RTTRACEBUFVOLATILE
{
    /** Reference counter. */
    uint32_t volatile   cRefs;
    /** The current entry. */
    uint32_t volatile   iEntry;
} RTTRACEBUFVOLATILE;
/** Pointer to the volatile parts of a trace buffer. */
typedef RTTRACEBUFVOLATILE *PRTTRACEBUFVOLATILE;


/**
 * Trace buffer entry.
 */
typedef struct RTTRACEBUFENTRY
{
    /** The nano second entry time stamp. */
    uint64_t            NanoTS;
    /** The message. */
    char                szMsg[RTTRACEBUF_ALIGNMENT - sizeof(uint64_t)];
} RTTRACEBUFENTRY;
AssertCompile(sizeof(RTTRACEBUFENTRY) <= RTTRACEBUF_ALIGNMENT);
/** Pointer to a trace buffer entry. */
typedef RTTRACEBUFENTRY *PRTTRACEBUFENTRY;



/**
 * Trace buffer structure.
 *
 * @remarks     This structure must be context agnostic, i.e. no pointers or
 *              other types that may differ between contexts (R3/R0/RC).
 */
typedef struct RTTRACEBUFINT
{
    /** Magic value (RTTRACEBUF_MAGIC). */
    uint32_t            u32Magic;
    /** The entry size. */
    uint32_t            cbEntry;
    /** The number of entries. */
    uint32_t            cEntries;
    /** Flags (always zero for now).  */
    uint32_t            fFlags;
    /** The offset to the volatile members (RTTRACEBUFVOLATILE) (relative to
     *  the start of this structure). */
    uint32_t            offVolatile;
    /** The offset to the entries (relative to the start of this structure). */
    uint32_t            offEntries;
    /** Reserved entries. */
    uint32_t            au32Reserved[2];
} RTTRACEBUFINT;
/** Pointer to a const trace buffer. */
typedef RTTRACEBUFINT const *PCRTTRACEBUFINT;


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
/** Calculates the address of the volatile trace buffer members. */
#define RTTRACEBUF_TO_VOLATILE(a_pThis)     ((PRTTRACEBUFVOLATILE)((uint8_t *)(a_pThis) + (a_pThis)->offVolatile))

/** Calculates the address of a trace buffer entry. */
#define RTTRACEBUF_TO_ENTRY(a_pThis, a_iEntry) \
    ((PRTTRACEBUFENTRY)( (uint8_t *)(a_pThis) + (a_pThis)->offEntries + (a_iEntry) * (a_pThis)->cbEntry ))

/** Validates a trace buffer handle and returns rc if not valid. */
#define RTTRACEBUF_VALID_RETURN_RC(a_pThis, a_rc) \
    do { \
        AssertPtrReturn((a_pThis), (a_rc)); \
        AssertReturn((a_pThis)->u32Magic == RTTRACEBUF_MAGIC, (a_rc)); \
        AssertReturn((a_pThis)->offVolatile < RTTRACEBUF_ALIGNMENT * 2, (a_rc)); \
        AssertReturn(RTTRACEBUF_TO_VOLATILE(a_pThis)->cRefs > 0, (a_rc)); \
    } while (0)

/**
 * Resolves and validates a trace buffer handle and returns rc if not valid.
 *
 * @param   a_hTraceBuf     The trace buffer handle passed by the user.
 * @param   a_pThis         Where to store the trace buffer pointer.
 */
#define RTTRACEBUF_RESOLVE_VALIDATE_RETAIN_RETURN(a_hTraceBuf, a_pThis) \
    do { \
        uint32_t cRefs; \
        if ((a_hTraceBuf) == RTTRACEBUF_DEFAULT) \
        { \
            (a_pThis) = RTTraceGetDefaultBuf(); \
            if (!RT_VALID_PTR(a_pThis)) \
                return VERR_NOT_FOUND; \
        } \
        else \
        { \
            (a_pThis) = (a_hTraceBuf); \
            AssertPtrReturn((a_pThis), VERR_INVALID_HANDLE); \
        } \
        AssertReturn((a_pThis)->u32Magic == RTTRACEBUF_MAGIC, VERR_INVALID_HANDLE); \
        AssertReturn((a_pThis)->offVolatile < RTTRACEBUF_ALIGNMENT * 2, VERR_INVALID_HANDLE); \
        \
        cRefs = ASMAtomicIncU32(&RTTRACEBUF_TO_VOLATILE(a_pThis)->cRefs); \
        if (RT_UNLIKELY(cRefs > 1 && cRefs < _1M)) \
        { \
            ASMAtomicDecU32(&RTTRACEBUF_TO_VOLATILE(a_pThis)->cRefs); \
            AssertFailedReturn(VERR_INVALID_HANDLE); \
        } \
    } while (0)


/**
 * Drops a trace buffer reference.
 *
 * @param   a_pThis     Pointer to the trace buffer.
 */
#define RTTRACEBUF_DROP_REFERENCE(a_pThis) \
    do { \
        uint32_t cRefs = ASMAtomicDecU32(&RTTRACEBUF_TO_VOLATILE(a_pThis)->cRefs); \
        if (!cRefs) \
            rtTraceBufDestroy((RTTRACEBUFINT *)a_pThis); \
    } while (0)


/**
 * The prologue code for a RTTraceAddSomething function.
 *
 * Resolves a trace buffer handle, grabs a reference to it and allocates the
 * next entry.  Return with an appropriate error status on failure.
 *
 * @param   a_hTraceBuf     The trace buffer handle passed by the user.
 *
 * @remarks This is kind of ugly, sorry.
 */
#define RTTRACEBUF_ADD_PROLOGUE(a_hTraceBuf) \
    int                 rc; \
    uint32_t            cRefs; \
    uint32_t            iEntry; \
    PCRTTRACEBUFINT     pThis; \
    PRTTRACEBUFVOLATILE pVolatile; \
    PRTTRACEBUFENTRY    pEntry; \
    char               *pszBuf; \
    size_t              cchBuf; \
    \
    /* Resolve and validate the handle. */ \
    if ((a_hTraceBuf) == RTTRACEBUF_DEFAULT) \
    { \
        pThis = RTTraceGetDefaultBuf(); \
        if (!RT_VALID_PTR(pThis)) \
            return VERR_NOT_FOUND; \
    } \
    else \
    { \
        pThis = (a_hTraceBuf); \
        AssertPtrReturn(pThis, VERR_INVALID_HANDLE); \
    } \
    AssertReturn(pThis->u32Magic == RTTRACEBUF_MAGIC, VERR_INVALID_HANDLE); \
    AssertReturn(pThis->offVolatile < RTTRACEBUF_ALIGNMENT * 2, VERR_INVALID_HANDLE); \
    pVolatile = RTTRACEBUF_TO_VOLATILE(pThis); \
    \
    /* Grab a reference. */ \
    cRefs = ASMAtomicIncU32(&pVolatile->cRefs); \
    if (RT_UNLIKELY(cRefs > 1 && cRefs < _1M)) \
    { \
        ASMAtomicDecU32(&pVolatile->cRefs); \
        AssertFailedReturn(VERR_INVALID_HANDLE); \
    } \
    \
    /* Grab the next entry and set the time stamp. */ \
    iEntry  = ASMAtomicIncU32(&pVolatile->iEntry); \
    iEntry %= pThis->cEntries; \
    pEntry = RTTRACEBUF_TO_ENTRY(pThis, iEntry); \
    pEntry->NanoTS = RTTimeNanoTS(); \
    pszBuf = &pEntry->szMsg[0]; \
    *pszBuf = '\0'; \
    cchBuf = pThis->cbEntry - RT_OFFSETOF(RTTRACEBUFENTRY, szMsg) - 1; \
    rc     = VINF_SUCCESS


/**
 * Used by a RTTraceAddPosSomething to store the source position in the entry
 * prior to adding the actual trace message text.
 *
 * Both pszBuf and cchBuf will be adjusted such that pszBuf points and the zero
 * terminator after the source position part.
 */
#define RTTRACEBUF_ADD_STORE_SRC_POS() \
    do { \
        /* file(line): - no path */ \
        size_t cchPos = RTStrPrintf(pszBuf, cchBuf, "%Rfn(%d): ", pszFile, iLine); \
        pszBuf += cchPos; \
        cchBuf -= cchPos; \
    } while (0)


/**
 * The epilogue code for a RTTraceAddSomething function.
 *
 * This will release the trace buffer reference.
 */
#define RTTRACEBUF_ADD_EPILOGUE() \
    cRefs = ASMAtomicDecU32(&pVolatile->cRefs); \
    if (!cRefs) \
        rtTraceBufDestroy((RTTRACEBUFINT *)pThis); \
    return rc


#ifndef IN_RC /* Drop this in RC context (too lazy to split the file). */

RTDECL(int) RTTraceBufCreate(PRTTRACEBUF phTraceBuf, uint32_t cEntries, uint32_t cbEntry, uint32_t fFlags)
{
    AssertPtrReturn(phTraceBuf, VERR_INVALID_POINTER);
    AssertReturn(!fFlags, VERR_INVALID_PARAMETER);
    AssertMsgReturn(cbEntry <= _64K, ("%#x\n", cbEntry), VERR_OUT_OF_RANGE);
    AssertMsgReturn(cEntries <= _1M, ("%#x\n", cEntries), VERR_OUT_OF_RANGE);

    /*
     * Apply default and alignment adjustments.
     */
    if (!cbEntry)
        cbEntry = RT_ALIGN_Z(256, RTTRACEBUF_ALIGNMENT);
    else
        cbEntry = RT_ALIGN_Z(cbEntry, RTTRACEBUF_ALIGNMENT);

    if (!cEntries)
        cEntries = 64;
    else if (cEntries < 4)
        cEntries = 4;

    /*
     * Calculate the required buffer size, allocte it and hand it on to the
     * carver API.
     */
    size_t  cbBlock = cbEntry * cEntries
                    + RT_ALIGN_Z(sizeof(RTTRACEBUFINT),      RTTRACEBUF_ALIGNMENT)
                    + RT_ALIGN_Z(sizeof(RTTRACEBUFVOLATILE), RTTRACEBUF_ALIGNMENT);
    void   *pvBlock = RTMemAlloc(cbBlock);
    if (!((uintptr_t)pvBlock & (RTTRACEBUF_ALIGNMENT - 1)))
    {
        RTMemFree(pvBlock);
        cbBlock += RTTRACEBUF_ALIGNMENT - 1;
        pvBlock = RTMemAlloc(cbBlock);
    }
    int rc;
    if (pvBlock)
    {
        rc = RTTraceBufCarve(phTraceBuf, cEntries, cbEntry, fFlags, pvBlock, &cbBlock);
        if (RT_FAILURE(rc))
            RTMemFree(pvBlock);
    }
    else
        rc = VERR_NO_MEMORY;
    return rc;
}


RTDECL(int) RTTraceBufCarve(PRTTRACEBUF phTraceBuf, uint32_t cEntries, uint32_t cbEntry, uint32_t fFlags,
                            void *pvBlock, size_t *pcbBlock)
{
    AssertPtrReturn(phTraceBuf, VERR_INVALID_POINTER);
    AssertReturn(!(fFlags & ~RTTRACEBUF_FLAGS_MASK), VERR_INVALID_PARAMETER);
    AssertMsgReturn(cbEntry <= _64K, ("%#x\n", cbEntry), VERR_OUT_OF_RANGE);
    AssertMsgReturn(cEntries <= _1M, ("%#x\n", cEntries), VERR_OUT_OF_RANGE);
    AssertPtrReturn(pvBlock, VERR_INVALID_POINTER);
    AssertPtrReturn(pcbBlock, VERR_INVALID_POINTER);

    /*
     * Apply defaults, align sizes and check against available buffer space.
     * This code can be made a bit more clever, if someone feels like it.
     */
    size_t const cbBlock    = *pcbBlock;
    size_t const cbHdr      = RT_ALIGN_Z(sizeof(RTTRACEBUFINT),      RTTRACEBUF_ALIGNMENT)
                            + RT_ALIGN_Z(sizeof(RTTRACEBUFVOLATILE), RTTRACEBUF_ALIGNMENT);
    size_t const cbEntryBuf = cbBlock > cbHdr ? cbBlock - cbHdr : 0;
    if (cbEntry)
        cbEntry = RT_ALIGN_Z(cbEntry, RTTRACEBUF_ALIGNMENT);
    else
    {
        if (!cbEntryBuf)
            cbEntry = RT_ALIGN_Z(256, RTTRACEBUF_ALIGNMENT);
        else if (cEntries)
        {
            cbEntry = cbBlock / cEntries;
            cbEntry &= ~(RTTRACEBUF_ALIGNMENT - 1);
            if (cbEntry > _64K)
                cbEntry = _64K;
        }
        else if (cbBlock >= RT_ALIGN_Z(512, RTTRACEBUF_ALIGNMENT) * 256)
            cbEntry = RT_ALIGN_Z(512, RTTRACEBUF_ALIGNMENT);
        else if (cbBlock >= RT_ALIGN_Z(256, RTTRACEBUF_ALIGNMENT) * 64)
            cbEntry = RT_ALIGN_Z(256, RTTRACEBUF_ALIGNMENT);
        else if (cbBlock >= RT_ALIGN_Z(128, RTTRACEBUF_ALIGNMENT) * 32)
            cbEntry = RT_ALIGN_Z(128, RTTRACEBUF_ALIGNMENT);
        else
            cbEntry = sizeof(RTTRACEBUFENTRY);
    }
    Assert(RT_ALIGN_Z(cbEntry, RTTRACEBUF_ALIGNMENT) == cbEntry);

    if (!cEntries)
        cEntries = cbEntry / cbEntryBuf;
    if (cEntries < 4)
        cEntries = 4;

    uint32_t offVolatile = RTTRACEBUF_ALIGNMENT - ((uintptr_t)pvBlock & (RTTRACEBUF_ALIGNMENT - 1));
    if (offVolatile < sizeof(RTTRACEBUFINT))
        offVolatile += RTTRACEBUF_ALIGNMENT;
    size_t cbReqBlock = offVolatile
                      + RT_ALIGN_Z(sizeof(RTTRACEBUFVOLATILE), RTTRACEBUF_ALIGNMENT)
                      + cbEntry * cEntries;
    if (*pcbBlock < cbReqBlock)
    {
        *pcbBlock = cbReqBlock;
        return VERR_BUFFER_OVERFLOW;
    }

    /*
     * Do the carving.
     */
    memset(pvBlock, 0, cbBlock);

    RTTRACEBUFINT *pThis = (RTTRACEBUFINT *)pvBlock;
    pThis->u32Magic         = RTTRACEBUF_MAGIC;
    pThis->cbEntry          = cbEntry;
    pThis->cEntries         = cEntries;
    pThis->fFlags           = fFlags;
    pThis->offVolatile      = offVolatile;
    pThis->offEntries       = offVolatile + RT_ALIGN_Z(sizeof(RTTRACEBUFVOLATILE), RTTRACEBUF_ALIGNMENT);

    PRTTRACEBUFVOLATILE pVolatile = (PRTTRACEBUFVOLATILE)((uint8_t *)pThis + offVolatile);
    pVolatile->cRefs        = 1;
    pVolatile->iEntry       = 0;

    *pcbBlock   = cbBlock - cbReqBlock;
    *phTraceBuf = pThis;
    return VINF_SUCCESS;
}

#endif /* !IN_RC */


/**
 * Destructor.
 *
 * @param   pThis               The trace buffer to destroy.
 */
static void rtTraceBufDestroy(RTTRACEBUFINT *pThis)
{
    AssertReturnVoid(ASMAtomicCmpXchgU32(&pThis->u32Magic, RTTRACEBUF_MAGIC_DEAD, RTTRACEBUF_MAGIC));
    if (pThis->fFlags & RTTRACEBUF_FLAGS_FREE_ME)
#ifdef IN_RC
        AssertReleaseFailed();
#else
        RTMemFree(pThis);
#endif
}


RTDECL(uint32_t)RTTraceBufRetain(RTTRACEBUF hTraceBuf)
{
    PCRTTRACEBUFINT pThis = hTraceBuf;
    RTTRACEBUF_VALID_RETURN_RC(pThis, UINT32_MAX);
    return ASMAtomicIncU32(&RTTRACEBUF_TO_VOLATILE(pThis)->cRefs);
}


RTDECL(uint32_t) RTTraceBufRelease(RTTRACEBUF hTraceBuf)
{
    if (hTraceBuf == NIL_RTTRACEBUF)
        return 0;

    PCRTTRACEBUFINT pThis = hTraceBuf;
    RTTRACEBUF_VALID_RETURN_RC(pThis, UINT32_MAX);

    uint32_t cRefs = ASMAtomicDecU32(&RTTRACEBUF_TO_VOLATILE(pThis)->cRefs);
    if (!cRefs)
        rtTraceBufDestroy((RTTRACEBUFINT *)pThis);
    return cRefs;
}


RTDECL(int) RTTraceBufAddMsg(RTTRACEBUF hTraceBuf, const char *pszMsg)
{
    RTTRACEBUF_ADD_PROLOGUE(hTraceBuf);
    RTStrCopy(pszBuf, cchBuf, pszMsg);
    RTTRACEBUF_ADD_EPILOGUE();
}


RTDECL(int) RTTraceBufAddMsgEx(  RTTRACEBUF hTraceBuf, const char *pszMsg, size_t cbMaxMsg)
{
    RTTRACEBUF_ADD_PROLOGUE(hTraceBuf);
    RTStrCopyEx(pszBuf, cchBuf, pszMsg, cbMaxMsg);
    RTTRACEBUF_ADD_EPILOGUE();
}


RTDECL(int) RTTraceBufAddMsgF(RTTRACEBUF hTraceBuf, const char *pszMsgFmt, ...)
{
    int         rc;
    va_list     va;
    va_start(va, pszMsgFmt);
    rc = RTTraceBufAddMsgV(hTraceBuf, pszMsgFmt, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTTraceBufAddMsgV(RTTRACEBUF hTraceBuf, const char *pszMsgFmt, va_list va)
{
    RTTRACEBUF_ADD_PROLOGUE(hTraceBuf);
    RTStrPrintfV(pszBuf, cchBuf, pszMsgFmt, va);
    RTTRACEBUF_ADD_EPILOGUE();
}


RTDECL(int) RTTraceBufAddPos(RTTRACEBUF hTraceBuf, RT_SRC_POS_DECL)
{
    RTTRACEBUF_ADD_PROLOGUE(hTraceBuf);
    RTTRACEBUF_ADD_STORE_SRC_POS();
    RTTRACEBUF_ADD_EPILOGUE();
}


RTDECL(int) RTTraceBufAddPosMsg(RTTRACEBUF hTraceBuf, RT_SRC_POS_DECL, const char *pszMsg)
{
    RTTRACEBUF_ADD_PROLOGUE(hTraceBuf);
    RTTRACEBUF_ADD_STORE_SRC_POS();
    RTStrCopy(pszBuf, cchBuf, pszMsg);
    RTTRACEBUF_ADD_EPILOGUE();
}


RTDECL(int) RTTraceBufAddPosMsgEx(RTTRACEBUF hTraceBuf, RT_SRC_POS_DECL, const char *pszMsg, size_t cbMaxMsg)
{
    RTTRACEBUF_ADD_PROLOGUE(hTraceBuf);
    RTTRACEBUF_ADD_STORE_SRC_POS();
    RTStrCopyEx(pszBuf, cchBuf, pszMsg, cbMaxMsg);
    RTTRACEBUF_ADD_EPILOGUE();
}


RTDECL(int) RTTraceBufAddPosMsgF(RTTRACEBUF hTraceBuf, RT_SRC_POS_DECL, const char *pszMsgFmt, ...)
{
    int         rc;
    va_list     va;
    va_start(va, pszMsgFmt);
    rc = RTTraceBufAddPosMsgV(hTraceBuf, RT_SRC_POS_ARGS, pszMsgFmt, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTTraceBufAddPosMsgV(RTTRACEBUF hTraceBuf, RT_SRC_POS_DECL, const char *pszMsgFmt, va_list va)
{
    RTTRACEBUF_ADD_PROLOGUE(hTraceBuf);
    RTTRACEBUF_ADD_STORE_SRC_POS();
    RTStrPrintfV(pszBuf, cchBuf, pszMsgFmt, va);
    RTTRACEBUF_ADD_EPILOGUE();
}



RTDECL(int) RTTraceBufDumpToLog(RTTRACEBUF hTraceBuf)
{
    uint32_t            iBase;
    uint32_t            cLeft;
    PCRTTRACEBUFINT     pThis;
    RTTRACEBUF_RESOLVE_VALIDATE_RETAIN_RETURN(hTraceBuf, pThis);

    iBase = ASMAtomicReadU32(&RTTRACEBUF_TO_VOLATILE(pThis)->iEntry);
    cLeft = pThis->cEntries;
    while (cLeft--)
    {
        PRTTRACEBUFENTRY pEntry;

        iBase %= pThis->cEntries;
        pEntry = RTTRACEBUF_TO_ENTRY(pThis, iBase);
        if (pEntry->NanoTS)
            RTLogPrintf("%u/%'llu: %s\n", cLeft, pEntry->NanoTS, pEntry->szMsg);

        /* next */
        iBase += 1;
    }

    RTTRACEBUF_DROP_REFERENCE(pThis);
    return VINF_SUCCESS;
}


RTDECL(int) RTTraceBufDumpToAssert(RTTRACEBUF hTraceBuf)
{
    uint32_t            iBase;
    uint32_t            cLeft;
    PCRTTRACEBUFINT     pThis;
    RTTRACEBUF_RESOLVE_VALIDATE_RETAIN_RETURN(hTraceBuf, pThis);

    iBase = ASMAtomicReadU32(&RTTRACEBUF_TO_VOLATILE(pThis)->iEntry);
    cLeft = pThis->cEntries;
    while (cLeft--)
    {
        PRTTRACEBUFENTRY pEntry;

        iBase %= pThis->cEntries;
        pEntry = RTTRACEBUF_TO_ENTRY(pThis, iBase);
        if (pEntry->NanoTS)
            RTAssertMsg2Add("%u/%'llu: %s\n", cLeft, pEntry->NanoTS, pEntry->szMsg);

        /* next */
        iBase += 1;
    }

    RTTRACEBUF_DROP_REFERENCE(pThis);
    return VINF_SUCCESS;
}
