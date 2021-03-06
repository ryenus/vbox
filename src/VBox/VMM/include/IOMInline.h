/* $Id$ */
/** @file
 * IOM - Inlined functions.
 */

/*
 * Copyright (C) 2006-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___IOMInline_h
#define ___IOMInline_h

/** @addtogroup grp_iom_int   Internals
 * @internal
 * @{
 */

/**
 * Gets the I/O port range for the specified I/O port in the current context.
 *
 * @returns Pointer to I/O port range.
 * @returns NULL if no port registered.
 *
 * @param   pVM     Pointer to the VM.
 * @param   Port    The I/O port lookup.
 */
DECLINLINE(CTX_SUFF(PIOMIOPORTRANGE)) iomIOPortGetRange(PVM pVM, RTIOPORT Port)
{
    Assert(PDMCritSectIsOwner(&pVM->iom.s.CritSect));
    return (CTX_SUFF(PIOMIOPORTRANGE))RTAvlroIOPortRangeGet(&pVM->iom.s.CTX_SUFF(pTrees)->CTX_SUFF(IOPortTree), Port);
}


/**
 * Gets the I/O port range for the specified I/O port in the HC.
 *
 * @returns Pointer to I/O port range.
 * @returns NULL if no port registered.
 *
 * @param   pVM     Pointer to the VM.
 * @param   Port    The I/O port to lookup.
 */
DECLINLINE(PIOMIOPORTRANGER3) iomIOPortGetRangeR3(PVM pVM, RTIOPORT Port)
{
    Assert(PDMCritSectIsOwner(&pVM->iom.s.CritSect));
    return (PIOMIOPORTRANGER3)RTAvlroIOPortRangeGet(&pVM->iom.s.CTX_SUFF(pTrees)->IOPortTreeR3, Port);
}


/**
 * Gets the MMIO range for the specified physical address in the current context.
 *
 * @returns Pointer to MMIO range.
 * @returns NULL if address not in a MMIO range.
 *
 * @param   pVM     Pointer to the VM.
 * @param   GCPhys  Physical address to lookup.
 */
DECLINLINE(PIOMMMIORANGE) iomMmioGetRange(PVM pVM, RTGCPHYS GCPhys)
{
    Assert(PDMCritSectIsOwner(&pVM->iom.s.CritSect));
    PIOMMMIORANGE pRange = pVM->iom.s.CTX_SUFF(pMMIORangeLast);
    if (    !pRange
        ||  GCPhys - pRange->GCPhys >= pRange->cb)
        pVM->iom.s.CTX_SUFF(pMMIORangeLast) = pRange
            = (PIOMMMIORANGE)RTAvlroGCPhysRangeGet(&pVM->iom.s.CTX_SUFF(pTrees)->MMIOTree, GCPhys);
    return pRange;
}

/**
 * Retain a MMIO range.
 *
 * @param   pRange  The range to release.
 */
DECLINLINE(void) iomMmioRetainRange(PIOMMMIORANGE pRange)
{
    uint32_t cRefs = ASMAtomicIncU32(&pRange->cRefs);
    Assert(cRefs > 1);
    Assert(cRefs < _1M);
    NOREF(cRefs);
}


/**
 * Gets the referenced MMIO range for the specified physical address in the
 * current context.
 *
 * @returns Pointer to MMIO range.
 * @returns NULL if address not in a MMIO range.
 *
 * @param   pVM     Pointer to the VM.
 * @param   GCPhys  Physical address to lookup.
 */
DECLINLINE(PIOMMMIORANGE) iomMmioGetRangeWithRef(PVM pVM, RTGCPHYS GCPhys)
{
    int rc = PDMCritSectEnter(&pVM->iom.s.CritSect, VINF_SUCCESS);
    AssertRCReturn(rc, NULL);

    PIOMMMIORANGE pRange = pVM->iom.s.CTX_SUFF(pMMIORangeLast);
    if (   !pRange
        || GCPhys - pRange->GCPhys >= pRange->cb)
        pVM->iom.s.CTX_SUFF(pMMIORangeLast) = pRange
            = (PIOMMMIORANGE)RTAvlroGCPhysRangeGet(&pVM->iom.s.CTX_SUFF(pTrees)->MMIOTree, GCPhys);
    if (pRange)
        iomMmioRetainRange(pRange);

    PDMCritSectLeave(&pVM->iom.s.CritSect);
    return pRange;
}


/**
 * Releases a MMIO range.
 *
 * @param   pVM     Pointer to the VM.
 * @param   pRange  The range to release.
 */
DECLINLINE(void) iomMmioReleaseRange(PVM pVM, PIOMMMIORANGE pRange)
{
    uint32_t cRefs = ASMAtomicDecU32(&pRange->cRefs);
    if (!cRefs)
        iomMmioFreeRange(pVM, pRange);
}


#ifdef VBOX_STRICT
/**
 * Gets the MMIO range for the specified physical address in the current context.
 *
 * @returns Pointer to MMIO range.
 * @returns NULL if address not in a MMIO range.
 *
 * @param   pVM     Pointer to the VM.
 * @param   GCPhys  Physical address to lookup.
 */
DECLINLINE(PIOMMMIORANGE) iomMMIOGetRangeUnsafe(PVM pVM, RTGCPHYS GCPhys)
{
    PIOMMMIORANGE pRange = pVM->iom.s.CTX_SUFF(pMMIORangeLast);
    if (    !pRange
        ||  GCPhys - pRange->GCPhys >= pRange->cb)
        pVM->iom.s.CTX_SUFF(pMMIORangeLast) = pRange
            = (PIOMMMIORANGE)RTAvlroGCPhysRangeGet(&pVM->iom.s.CTX_SUFF(pTrees)->MMIOTree, GCPhys);
    return pRange;
}
#endif /* VBOX_STRICT */


#ifdef VBOX_WITH_STATISTICS
/**
 * Gets the MMIO statistics record.
 *
 * In ring-3 this will lazily create missing records, while in GC/R0 the caller has to
 * return the appropriate status to defer the operation to ring-3.
 *
 * @returns Pointer to MMIO stats.
 * @returns NULL if not found (R0/GC), or out of memory (R3).
 *
 * @param   pVM         Pointer to the VM.
 * @param   GCPhys      Physical address to lookup.
 * @param   pRange      The MMIO range.
 */
DECLINLINE(PIOMMMIOSTATS) iomMmioGetStats(PVM pVM, RTGCPHYS GCPhys, PIOMMMIORANGE pRange)
{
    PDMCritSectEnter(&pVM->iom.s.CritSect, VINF_SUCCESS);

    /* For large ranges, we'll put everything on the first byte. */
    if (pRange->cb > PAGE_SIZE)
        GCPhys = pRange->GCPhys;

    PIOMMMIOSTATS pStats = pVM->iom.s.CTX_SUFF(pMMIOStatsLast);
    if (    !pStats
        ||  pStats->Core.Key != GCPhys)
    {
        pStats = (PIOMMMIOSTATS)RTAvloGCPhysGet(&pVM->iom.s.CTX_SUFF(pTrees)->MmioStatTree, GCPhys);
# ifdef IN_RING3
        if (!pStats)
            pStats = iomR3MMIOStatsCreate(pVM, GCPhys, pRange->pszDesc);
# endif
    }

    PDMCritSectLeave(&pVM->iom.s.CritSect);
    return pStats;
}
#endif /* VBOX_WITH_STATISTICS */


/** @}  */

#endif

