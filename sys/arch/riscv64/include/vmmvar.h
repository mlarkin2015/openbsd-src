/*	$OpenBSD: vmmvar.h,v 1.89 2023/01/30 02:32:01 dv Exp $	*/
/*
 * Copyright (c) 2014 Mike Larkin <mlarkin@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * CPU capabilities for VMM operation
 */
#ifndef _MACHINE_VMMVAR_H_
#define _MACHINE_VMMVAR_H_

#define VMM_HV_SIGNATURE 	"OpenBSDVMM58"

#define VMM_MAX_MEM_RANGES	16
#define VMM_MAX_DISKS_PER_VM	4
#define VMM_MAX_PATH_DISK	128
#define VMM_MAX_PATH_CDROM	128
#define VMM_MAX_NAME_LEN	64
#define VMM_MAX_KERNEL_PATH	128
#define VMM_MAX_VCPUS		512
#define VMM_MAX_VCPUS_PER_VM	64
#define VMM_MAX_VM_MEM_SIZE	32L * 1024 * 1024 * 1024	/* 32 GiB */
#define VMM_MAX_NICS_PER_VM	4

#ifdef _KERNEL

enum {
	VMM_MODE_UNKNOWN,
	VMM_MODE_CLASSIC,
	VMM_MODE_HMODE,
};

/*
 * Virtual Machine
 */
struct vm;

struct vmm_softc_md {
};

struct vcpu_reg_state {
};

/*
 * Virtual CPU
 *
 * Methods used to vcpu struct members:
 *	a	atomic operations
 *	I	immutable operations
 *	K	kernel lock
 *	r	reference count
 *	v	vcpu rwlock
 *	V	vm struct's vcpu list lock (vm_vcpu_lock)
 */
struct vcpu {
	struct vm *vc_parent;			/* [I] */
	uint32_t vc_id;				/* [I] */
	uint16_t vc_vpid;			/* [I] */
	u_int vc_state;				/* [a] */
	SLIST_ENTRY(vcpu) vc_vcpu_link;		/* [V] */

	struct rwlock vc_lock;

	struct cpu_info *vc_curcpu;		/* [a] */
	struct cpu_info *vc_last_pcpu;		/* [v] */
};

SLIST_HEAD(vcpu_head, vcpu);

struct vm_rwregs_params {
};

struct vm_run_params {
};

struct vm_rwvmparams_params {
};

void	vmm_attach_machdep(struct device *, struct device *, void *);
void	vmm_activate_machdep(struct device *, int);
int	pledge_ioctl_vmm_machdep(struct proc *, long);
int	vm_impl_init(struct vm *, struct proc *);
void	vm_impl_deinit(struct vm *);
int	vcpu_init(struct vcpu *);
void	vcpu_deinit(struct vcpu *);
int	vcpu_reset_regs(struct vcpu *, struct vcpu_reg_state *);
int	vm_rwvmparams(struct vm_rwvmparams_params *, int);
int	vm_rwregs(struct vm_rwregs_params *, int);
int	vm_run(struct vm_run_params *);
int	vmm_start(void);
int	vmm_stop(void);

#endif /* _KERNEL */

#endif /* ! _MACHINE_VMMVAR_H_ */
