# If kversion isn't defined on the rpmbuild line, define it here.
%{!?kversion: %define kversion %(uname -r)}

%{!?with_oot_debug: %define with_oot_debug 0}

%define kmod_name safelinux-modules

%define debug_package %{nil}

Name: %{kmod_name}
Version: 1.0
Release:        1%{?dist}
Summary: kium kernel module

License: GPLv2
Source0: %{name}-%{version}.tar.gz

BuildRequires: modules-signkey
BuildRequires: kernel-automotive-devel-uname-r = %{kversion}
Requires: kernel-automotive-core-uname-r = %{kversion}

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
%if %{with_oot_debug}
KERNEL_SRC=%{_usrsrc}/kernels/%{kversion}+debug
%else
KERNEL_SRC=%{_usrsrc}/kernels/%{kversion}
%endif
make KDIR=${KERNEL_SRC} modules

%post
depmod %{kversion}

%postun
depmod %{kversion}

%install
%if %{with_oot_debug}
KERNEL_SRC=%{_usrsrc}/kernels/%{kversion}+debug
%else
KERNEL_SRC=%{_usrsrc}/kernels/%{kversion}
%endif
make KDIR=${KERNEL_SRC} INSTALL_MOD_PATH=$RPM_BUILD_ROOT modules_install
make KDIR=${KERNEL_SRC} HDR_INSTAL_PATH=$RPM_BUILD_ROOT/usr/include headers_install
%if %{with_oot_debug}
rm -rf "$RPM_BUILD_ROOT/lib/modules/%{kversion}+debug/modules."*
%else
rm -rf "$RPM_BUILD_ROOT/lib/modules/%{kversion}/modules."*
%endif

%clean
rm -rf $RPM_BUILD_ROOT

%files uapi-headers
%{_includedir}/linux/iommu_iova_map.h
%{_includedir}/uapi/misc/iommu_iova_map_user.h
%{_includedir}/uapi/misc/kiumd.h
%{_includedir}/uapi/misc/scm_user_intf.h
%{_includedir}/uapi/misc/qcom_uscmi.h

%files
%if %{with_oot_debug}
%define kernel_module_path /lib/modules/%{kversion}+debug
%else
%define kernel_module_path /lib/modules/%{kversion}
%endif
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
