M=$(PWD)

KERNEL_VERSION ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNEL_VERSION)/build

all: clean modules

HDR_INSTAL_PATH ?= $(KDIR)/usr/include
INSTALL_HDR=$(shell mkdir -p $(HDR_INSTAL_PATH)/linux; \
             mkdir -p $(HDR_INSTAL_PATH)/uapi/misc; \
			 cd $(KDIR);  \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/linux/iommu_iova_map.h $(HDR_INSTAL_PATH)/linux/iommu_iova_map.h; \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/uapi/misc/iommu_iova_map_user.h $(HDR_INSTAL_PATH)/uapi/misc/iommu_iova_map_user.h; \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/uapi/misc/qcom_uscmi.h $(HDR_INSTAL_PATH)/uapi/misc/qcom_uscmi.h; \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/uapi/misc/kiumd.h $(HDR_INSTAL_PATH)/uapi/misc/kiumd.h; \
			 $(KDIR)/scripts/headers_install.sh \
			 $(PWD)/include/uapi/misc/scm_user_intf.h $(HDR_INSTAL_PATH)/uapi/misc/scm_user_intf.h; \
			 [ -f $(HDR_INSTAL_PATH)/uapi/misc/kiumd.h ] && [ -f $(HDR_INSTAL_PATH)/uapi/misc/scm_user_intf.h ] && echo "passed" || echo "failed")

modules:
	$(MAKE) -C "$(KDIR)" "M=$(M)" modules

clean:
	$(MAKE) -C "$(KDIR)" "M=$(M)" clean

modules_install:
	$(MAKE) -C "$(KDIR)" "M=$(M)" modules_install

headers_install:
	 @echo "header installing $(INSTALL_HDR)"
