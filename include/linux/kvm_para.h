#ifndef __LINUX_KVM_PARA_H
#define __LINUX_KVM_PARA_H

#include <uapi/linux/kvm_para.h>


static inline int kvm_para_has_feature(unsigned int feature)
{
	if (kvm_arch_para_features() & (1UL << feature))
		return 1;
	return 0;
}

#ifdef CONFIG_PASR_HYPERCALL

#define KVM_HC_PASR_MIN				10

#define KVM_HC_PASR_MM_PAGE_FREE		11
#define KVM_HC_PASR_MM_PAGE_FREE_BATCHED	12
#define KVM_HC_PASR_MM_PAGE_ALLOC		13
#define KVM_HC_PASR_MM_PAGE_ALLOC_ZONE_LOCKED	14
#define KVM_HC_PASR_MM_PAGE_PCPU_DRAIN		15
#define KVM_HC_PASR_MM_PAGE_ALLOC_EXTFRAG	16

#define KVM_HC_PASR_MAX				100
#endif

#endif /* __LINUX_KVM_PARA_H */
