/* $Id$ */
/** @file
 * HM - All contexts.
 */

/*
 * Copyright (C) 2006-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_HM
#include <VBox/vmm/hm.h>
#include <VBox/vmm/pgm.h>
#include "HMInternal.h"
#include <VBox/vmm/vm.h>
#include <VBox/vmm/hm_vmx.h>
#include <VBox/vmm/hm_svm.h>
#include <VBox/err.h>
#include <VBox/log.h>
#include <iprt/param.h>
#include <iprt/assert.h>
#include <iprt/asm.h>
#include <iprt/string.h>
#include <iprt/x86.h>


/**
 * Queues a page for invalidation
 *
 * @returns VBox status code.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   GCVirt      Page to invalidate
 */
static void hmQueueInvlPage(PVMCPU pVCpu, RTGCPTR GCVirt)
{
    /* Nothing to do if a TLB flush is already pending */
    if (VMCPU_FF_ISSET(pVCpu, VMCPU_FF_TLB_FLUSH))
        return;
#if 1
    VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
    NOREF(GCVirt);
#else
    /* Be very careful when activating this code! */
    if (iPage == RT_ELEMENTS(pVCpu->hm.s.TlbShootdown.aPages))
        VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
    else
        VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_SHOOTDOWN);
#endif
}

/**
 * Invalidates a guest page
 *
 * @returns VBox status code.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   GCVirt      Page to invalidate
 */
VMM_INT_DECL(int) HMInvalidatePage(PVMCPU pVCpu, RTGCPTR GCVirt)
{
    STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushPageManual);
#ifdef IN_RING0
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (pVM->hm.s.vmx.fSupported)
        return VMXR0InvalidatePage(pVM, pVCpu, GCVirt);

    Assert(pVM->hm.s.svm.fSupported);
    return SVMR0InvalidatePage(pVM, pVCpu, GCVirt);

#else
    hmQueueInvlPage(pVCpu, GCVirt);
    return VINF_SUCCESS;
#endif
}

/**
 * Flushes the guest TLB
 *
 * @returns VBox status code.
 * @param   pVCpu       Pointer to the VMCPU.
 */
VMM_INT_DECL(int) HMFlushTLB(PVMCPU pVCpu)
{
    LogFlow(("HMFlushTLB\n"));

    VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlbManual);
    return VINF_SUCCESS;
}

#ifdef IN_RING0

/**
 * Dummy RTMpOnSpecific handler since RTMpPokeCpu couldn't be used.
 *
 */
static DECLCALLBACK(void) hmFlushHandler(RTCPUID idCpu, void *pvUser1, void *pvUser2)
{
    NOREF(idCpu); NOREF(pvUser1); NOREF(pvUser2);
    return;
}

/**
 * Wrapper for RTMpPokeCpu to deal with VERR_NOT_SUPPORTED.
 */
static void hmR0PokeCpu(PVMCPU pVCpu, RTCPUID idHostCpu)
{
    uint32_t cWorldSwitchExits = ASMAtomicUoReadU32(&pVCpu->hm.s.cWorldSwitchExits);

    STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatPoke, x);
    int rc = RTMpPokeCpu(idHostCpu);
    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatPoke, x);

    /* Not implemented on some platforms (Darwin, Linux kernel < 2.6.19); fall
       back to a less efficient implementation (broadcast). */
    if (rc == VERR_NOT_SUPPORTED)
    {
        STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatSpinPoke, z);
        /* synchronous. */
        RTMpOnSpecific(idHostCpu, hmFlushHandler, 0, 0);
        STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatSpinPoke, z);
    }
    else
    {
        if (rc == VINF_SUCCESS)
            STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatSpinPoke, z);
        else
            STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatSpinPokeFailed, z);

/** @todo If more than one CPU is going to be poked, we could optimize this
 *        operation by poking them first and wait afterwards.  Would require
 *        recording who to poke and their current cWorldSwitchExits values,
 *        that's something not suitable for stack... So, pVCpu->hm.s.something
 *        then. */
        /* Spin until the VCPU has switched back (poking is async). */
        while (   ASMAtomicUoReadBool(&pVCpu->hm.s.fCheckedTLBFlush)
               && cWorldSwitchExits == ASMAtomicUoReadU32(&pVCpu->hm.s.cWorldSwitchExits))
            ASMNopPause();

        if (rc == VINF_SUCCESS)
            STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatSpinPoke, z);
        else
            STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatSpinPokeFailed, z);
    }
}

#endif /* IN_RING0 */
#ifndef IN_RC

/**
 * Poke an EMT so it can perform the appropriate TLB shootdowns.
 *
 * @param   pVCpu               The handle of the virtual CPU to poke.
 * @param   fAccountFlushStat   Whether to account the call to
 *                              StatTlbShootdownFlush or StatTlbShootdown.
 */
static void hmPokeCpuForTlbFlush(PVMCPU pVCpu, bool fAccountFlushStat)
{
    if (ASMAtomicUoReadBool(&pVCpu->hm.s.fCheckedTLBFlush))
    {
        if (fAccountFlushStat)
            STAM_COUNTER_INC(&pVCpu->hm.s.StatTlbShootdownFlush);
        else
            STAM_COUNTER_INC(&pVCpu->hm.s.StatTlbShootdown);
#ifdef IN_RING0
        RTCPUID idHostCpu = pVCpu->hm.s.idEnteredCpu;
        if (idHostCpu != NIL_RTCPUID)
            hmR0PokeCpu(pVCpu, idHostCpu);
#else
        VMR3NotifyCpuFFU(pVCpu->pUVCpu, VMNOTIFYFF_FLAGS_POKE);
#endif
    }
    else
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushPageManual);
}


/**
 * Invalidates a guest page on all VCPUs.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   GCVirt      Page to invalidate
 */
VMM_INT_DECL(int) HMInvalidatePageOnAllVCpus(PVM pVM, RTGCPTR GCPtr)
{
    VMCPUID idCurCpu = VMMGetCpuId(pVM);
    STAM_COUNTER_INC(&pVM->aCpus[idCurCpu].hm.s.StatFlushPage);

    for (VMCPUID idCpu = 0; idCpu < pVM->cCpus; idCpu++)
    {
        PVMCPU pVCpu = &pVM->aCpus[idCpu];

        /* Nothing to do if a TLB flush is already pending; the VCPU should
           have already been poked if it were active. */
        if (VMCPU_FF_ISSET(pVCpu, VMCPU_FF_TLB_FLUSH))
            continue;

        if (pVCpu->idCpu == idCurCpu)
            HMInvalidatePage(pVCpu, GCPtr);
        else
        {
            hmQueueInvlPage(pVCpu, GCPtr);
            hmPokeCpuForTlbFlush(pVCpu, false /*fAccountFlushStat*/);
        }
    }

    return VINF_SUCCESS;
}


/**
 * Flush the TLBs of all VCPUs
 *
 * @returns VBox status code.
 * @param   pVM       Pointer to the VM.
 */
VMM_INT_DECL(int) HMFlushTLBOnAllVCpus(PVM pVM)
{
    if (pVM->cCpus == 1)
        return HMFlushTLB(&pVM->aCpus[0]);

    VMCPUID idThisCpu = VMMGetCpuId(pVM);

    STAM_COUNTER_INC(&pVM->aCpus[idThisCpu].hm.s.StatFlushTlb);

    for (VMCPUID idCpu = 0; idCpu < pVM->cCpus; idCpu++)
    {
        PVMCPU pVCpu = &pVM->aCpus[idCpu];

        /* Nothing to do if a TLB flush is already pending; the VCPU should
           have already been poked if it were active. */
        if (!VMCPU_FF_ISSET(pVCpu, VMCPU_FF_TLB_FLUSH))
        {
            VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
            if (idThisCpu != idCpu)
                hmPokeCpuForTlbFlush(pVCpu, true /*fAccountFlushStat*/);
        }
    }

    return VINF_SUCCESS;
}

#endif /* !IN_RC */

/**
 * Checks if nested paging is enabled
 *
 * @returns boolean
 * @param   pVM         Pointer to the VM.
 */
VMM_INT_DECL(bool) HMIsNestedPagingActive(PVM pVM)
{
    return HMIsEnabled(pVM) && pVM->hm.s.fNestedPaging;
}

/**
 * Return the shadow paging mode for nested paging/ept
 *
 * @returns shadow paging mode
 * @param   pVM         Pointer to the VM.
 */
VMM_INT_DECL(PGMMODE) HMGetShwPagingMode(PVM pVM)
{
    Assert(HMIsNestedPagingActive(pVM));
    if (pVM->hm.s.svm.fSupported)
        return PGMMODE_NESTED;

    Assert(pVM->hm.s.vmx.fSupported);
    return PGMMODE_EPT;
}

/**
 * Invalidates a guest page by physical address
 *
 * NOTE: Assumes the current instruction references this physical page though a virtual address!!
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   GCPhys      Page to invalidate
 */
VMM_INT_DECL(int) HMInvalidatePhysPage(PVM pVM, RTGCPHYS GCPhys)
{
    if (!HMIsNestedPagingActive(pVM))
        return VINF_SUCCESS;

#ifdef IN_RING0
    if (pVM->hm.s.vmx.fSupported)
    {
        VMCPUID idThisCpu = VMMGetCpuId(pVM);

        for (VMCPUID idCpu = 0; idCpu < pVM->cCpus; idCpu++)
        {
            PVMCPU pVCpu = &pVM->aCpus[idCpu];

            if (idThisCpu == idCpu)
            {
                /** @todo r=ramshankar: Intel does not support flushing by guest physical
                 *        address either. See comment in VMXR0InvalidatePhysPage(). Fix this. */
                VMXR0InvalidatePhysPage(pVM, pVCpu, GCPhys);
            }
            else
            {
                VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
                hmPokeCpuForTlbFlush(pVCpu, true /*fAccountFlushStat*/);
            }
        }
        return VINF_SUCCESS;
    }

    /* AMD-V doesn't support invalidation with guest physical addresses; see
       comment in SVMR0InvalidatePhysPage. */
    Assert(pVM->hm.s.svm.fSupported);
#else
    NOREF(GCPhys);
#endif

    HMFlushTLBOnAllVCpus(pVM);
    return VINF_SUCCESS;
}

/**
 * Checks if an interrupt event is currently pending.
 *
 * @returns Interrupt event pending state.
 * @param   pVM         Pointer to the VM.
 */
VMM_INT_DECL(bool) HMHasPendingIrq(PVM pVM)
{
    PVMCPU pVCpu = VMMGetCpu(pVM);
    return !!pVCpu->hm.s.Event.fPending;
}


/**
 * Return the PAE PDPE entries.
 *
 * @returns Pointer to the PAE PDPE array.
 * @param   pVCpu       Pointer to the VMCPU.
 */
VMM_INT_DECL(PX86PDPE) HMGetPaePdpes(PVMCPU pVCpu)
{
    return &pVCpu->hm.s.aPdpes[0];
}

