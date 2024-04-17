# If kversion isn't defined on the rpmbuild line, define it here.
%{!?kversion: %define kversion %(uname -r)}

%{!?with_oot_debug: %define with_oot_debug 0}

%define kmod_name safelinux-modules

%define debug_package %{nil}

%if %{with_oot_debug}
    %define kpackage kernel-automotive-debug
    %define kversion_with_debug %{kversion}+debug
%else
    %define kpackage kernel-automotive
    %define kversion_with_debug %{kversion}
%endif

Name: %{kmod_name}
Version: 1.0
Release:        1%{?dist}
Summary: kium kernel module

License: GPLv2
Source0: %{name}-%{version}.tar.gz

BuildRequires: modules-signkey
BuildRequires: kernel-automotive-devel-uname-r = %{kversion_with_debug}
Requires: %{kpackage}-core-uname-r = %{kversion_with_debug}

%description
This is rpm contains safelinux out of tree kernel modules.

%package uapi-headers
Summary: %{summary} - This rpm contains uapi headers of safelinux modules.
Requires: %{name} = %{version}-%{release}

%description uapi-headers
%{summary}: %{summary}

%prep
%setup -qn %{name}

%build
KERNEL_SRC=%{_usrsrc}/kernels/%{kversion_with_debug}
make KDIR=${KERNEL_SRC} modules

%post
depmod %{kversion_with_debug}

%postun
depmod %{kversion_with_debug}

%install
KERNEL_SRC=%{_usrsrc}/kernels/%{kversion_with_debug}
make KDIR=${KERNEL_SRC} INSTALL_MOD_PATH=$RPM_BUILD_ROOT modules_install
make KDIR=${KERNEL_SRC} HDR_INSTAL_PATH=$RPM_BUILD_ROOT/usr/include headers_install
rm -rf "$RPM_BUILD_ROOT/lib/modules/%{kversion_with_debug}/modules."*

%clean
rm -rf $RPM_BUILD_ROOT

%files uapi-headers
%{_includedir}/linux/iommu_iova_map.h
%{_includedir}/uapi/misc/iommu_iova_map_user.h
%{_includedir}/uapi/misc/kiumd.h
%{_includedir}/uapi/misc/scm_user_intf.h
%{_includedir}/uapi/misc/qcom_uscmi.h

%files
%define kernel_module_path /lib/modules/%{kversion_with_debug}
%{kernel_module_path}/extra/apps_pinctrl.ko
%{kernel_module_path}/extra/scm_user_intf.ko
%{kernel_module_path}/extra/vfio_iommu_qcom.ko
%{kernel_module_path}/extra/iommu_iova_map.ko
%{kernel_module_path}/extra/kiumd.ko
%{kernel_module_path}/extra/qcom_uscmi.ko
%{kernel_module_path}/extra/kryo_arm64_edac.ko
%{kernel_module_path}/extra/kiumd_kgsl.ko
%{kernel_module_path}/extra/qcom_dma_heaps.ko
%{kernel_module_path}/extra/mhi_ep_net.ko

%changelog
* Mon Aug 28 2023 Deepti Jaggi <quic_djaggi@quicinc.com> 1.1
- Add Edac driver
* Fri Jul 27 2023 Venkatakrishnaiah Pari <quic_vpari@quicinc.com> 1.0
- First commit!
