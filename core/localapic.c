/*
 * Copyright (c) 2007, 2008 University of Tsukuba
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the University of Tsukuba nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "asm.h"
#include "current.h"
#include "initfunc.h"
#include "localapic.h"
#include "mm.h"
#include "mmio.h"
#include "panic.h"

#define APIC_BASE	0xFEE00000
#define APIC_LEN	0x1000
#define APIC_ID		0xFEE00020
#define APIC_ID_AID_SHIFT	24
#define APIC_ICR_LOW	0xFEE00300
#define APIC_ICR_LOW_VEC_MASK	0xFF
#define APIC_ICR_LOW_MT_MASK	0x700
#define APIC_ICR_LOW_MT_STARTUP	0x600
#define APIC_ICR_LOW_DM_LOGICAL_BIT	0x800
#define APIC_ICR_LOW_DSH_MASK	0xC0000
#define APIC_ICR_LOW_DSH_DEST	0x00000
#define APIC_ICR_LOW_DSH_SELF	0x40000
#define APIC_ICR_LOW_DSH_ALL	0x80000
#define APIC_ICR_LOW_DSH_OTHERS	0xC0000
#define APIC_ICR_HIGH	0xFEE00310
#define APIC_ICR_HIGH_DES_SHIFT	24

struct do_startup_data {
	struct vcpu *vcpu0;
	u32 sipi_vector, apic_id;
};

static bool
do_startup (struct vcpu *p, void *q)
{
	struct do_startup_data *d;
	u32 sipi_vector;

	d = q;
	if (p->vcpu0 != d->vcpu0)
		return false;
	if (d->apic_id == 0xFF || p->localapic.apic_id == d->apic_id) {
		sipi_vector = p->localapic.sipi_vector;
		asm_lock_cmpxchgl (&p->localapic.sipi_vector, &sipi_vector,
				   d->sipi_vector);
	}
	return false;
}

static void
apic_startup (u32 icr_low, u32 icr_high)
{
	struct do_startup_data d;

	switch (icr_low & APIC_ICR_LOW_DSH_MASK) {
	case APIC_ICR_LOW_DSH_DEST:
		if (icr_low & APIC_ICR_LOW_DM_LOGICAL_BIT)
			panic ("Start-up IPI with a logical ID destination"
			       " is not yet supported");
		d.apic_id = icr_high >> APIC_ICR_HIGH_DES_SHIFT;
		break;
	case APIC_ICR_LOW_DSH_SELF:
		panic ("Delivering start-up IPI to self");
	case APIC_ICR_LOW_DSH_ALL:
		panic ("Delivering start-up IPI to all including self");
	case APIC_ICR_LOW_DSH_OTHERS:
		d.apic_id = 0xFF; /* APIC ID 0xFF means a broadcast */
		break;
	}
	d.vcpu0 = current->vcpu0;
	d.sipi_vector = icr_low & APIC_ICR_LOW_VEC_MASK;
	vcpu_list_foreach (do_startup, &d);
}

static int
mmio_apic (void *data, phys_t gphys, bool wr, void *buf, uint len, u32 f)
{
	u32 *apic_icr_low, *apic_icr_high;

	if (!wr)
		return 0;
	if (gphys == APIC_ICR_LOW) {
		if (len != 4)
			panic ("APIC_ICR len %u", len);
	} else if (gphys > APIC_ICR_LOW) {
		if (gphys < APIC_ICR_LOW + 4)
			panic ("APIC_ICR gphys 0x%llX", gphys);
		else
			return 0;
	} else {
		if (gphys + len > APIC_ICR_LOW)
			panic ("APIC_ICR gphys 0x%llX len %u", gphys, len);
		else
			return 0;
	}

	apic_icr_low = buf;
	switch (*apic_icr_low & APIC_ICR_LOW_MT_MASK) {
	default:
		return 0;
	case APIC_ICR_LOW_MT_STARTUP:
		apic_icr_high = mapmem_hphys (APIC_ICR_HIGH,
					      sizeof *apic_icr_high,
					      MAPMEM_PCD | MAPMEM_PWT);
		apic_startup (*apic_icr_low, *apic_icr_high);
		unmapmem (apic_icr_high, sizeof *apic_icr_high);
		return 0;
	}
	return 0;
}

u32
localapic_wait_for_sipi (void)
{
	u32 *apic_id, sipi_vector;

	apic_id = mapmem_hphys (APIC_ID, sizeof *apic_id,
				MAPMEM_PCD | MAPMEM_PWT);
	current->localapic.apic_id = *apic_id >> APIC_ID_AID_SHIFT;
	unmapmem (apic_id, sizeof *apic_id);
	sipi_vector = current->localapic.sipi_vector;
	asm_lock_cmpxchgl (&current->localapic.sipi_vector, &sipi_vector, ~0U);
	do {
		asm_pause ();
		asm_lock_cmpxchgl (&current->localapic.sipi_vector,
				   &sipi_vector, ~0U);
	} while (sipi_vector == ~0U);
	return sipi_vector;
}

void
localapic_change_base_msr (u64 msrdata)
{
	if (!current->vcpu0->localapic.registered)
		return;
	if (!(msrdata & MSR_IA32_APIC_BASE_MSR_APIC_GLOBAL_ENABLE_BIT))
		return;
	if ((msrdata & 0xFFFFFFFF00000000ULL) ||
	    (msrdata & MSR_IA32_APIC_BASE_MSR_APIC_BASE_MASK) != APIC_BASE)
		panic ("localapic_wait_for_sipi: Bad APIC base 0x%llX",
		       msrdata);
}

void
localapic_mmio_register (void)
{
	if (!current->vcpu0->localapic.registered) {
		mmio_register (APIC_BASE, APIC_LEN, mmio_apic, NULL);
		current->vcpu0->localapic.registered = true;
	}
}

static void
localapic_init (void)
{
	if (current == current->vcpu0)
		current->localapic.registered = false;
}

INITFUNC ("vcpu0", localapic_init);
