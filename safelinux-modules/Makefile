
KDIR=$(KERNEL_SRC)
SAFELINUX_MODULES_ROOT=$(PWD)
KBUILD_OPTIONS += SAFELINUX_MODULES_ROOT=$(SAFELINUX_MODULES_ROOT)

HDR_INSTAL_PATH ?= $(KDIR)/usr/include
INSTALL_HDR=$(shell mkdir -p $(HDR_INSTAL_PATH)/linux; \
             mkdir -p $(HDR_INSTAL_PATH)/uapi/misc; \
			 cd $(KDIR);  \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/linux/iommu_iova_map.h $(HDR_INSTAL_PATH)/linux/iommu_iova_map.h; \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/linux/qtee_shmbridge.h $(HDR_INSTAL_PATH)/linux/qtee_shmbridge.h; \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/uapi/misc/iommu_iova_map_user.h $(HDR_INSTAL_PATH)/uapi/misc/iommu_iova_map_user.h; \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/uapi/misc/qcom_uscmi.h $(HDR_INSTAL_PATH)/uapi/misc/qcom_uscmi.h; \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/uapi/misc/kiumd.h $(HDR_INSTAL_PATH)/uapi/misc/kiumd.h; \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/uapi/misc/scm_user_intf.h $(HDR_INSTAL_PATH)/uapi/misc/scm_user_intf.h; \
			 [ -f $(HDR_INSTAL_PATH)/uapi/misc/kiumd.h ] && [ -f $(HDR_INSTAL_PATH)/uapi/misc/scm_user_intf.h ] && echo "passed" || echo "failed")

all: clean modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) $(KBUILD_OPTIONS) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

headers_install:
	 @echo "header installing $(INSTALL_HDR)"
