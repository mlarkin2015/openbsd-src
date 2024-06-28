#include <sys/param.h>
#include <sys/proc.h>
#include <sys/device.h>

#include <machine/vmmvar.h>

#include <dev/vmm/vmm.h>

int
vmmioctl_machdep(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc * p)
{
	return 0;
}

void
vmm_attach_machdep(struct device *parent, struct device *self, void *aux)
{
}

void
vmm_activate_machdep(struct device *self, int act)
{
}

int
pledge_ioctl_vmm_machdep(struct proc *p, long com)
{
	return 0;
}

int
vm_impl_init(struct vm *vm, struct proc *p)
{
	return 0;
}

void
vm_impl_deinit(struct vm *vm)
{
}

int
vcpu_init(struct vcpu *vcpu)
{
	return 0;
}

void
vcpu_deinit(struct vcpu *vcpu)
{
}

int
vcpu_reset_regs(struct vcpu *vcpu, struct vcpu_reg_state *vrs)
{
	return 0;
}

int
vm_rwvmparams(struct vm_rwvmparams_params *vrp, int dir)
{
	return 0;
}

int
vm_rwregs(struct vm_rwregs_params *vrp, int dir)
{
	return 0;
}

int
vm_run(struct vm_run_params *vrp)
{
	return 0;
}

int
vmm_start(void)
{
	return 0;
}

int
vmm_stop(void)
{
	return 0;
}
