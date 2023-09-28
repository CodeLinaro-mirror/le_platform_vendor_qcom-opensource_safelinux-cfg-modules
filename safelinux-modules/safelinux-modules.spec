# If kversion isn't defined on the rpmbuild line, define it here.
%{!?kversion: %define kversion %(uname -r)}

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
KERNEL_SRC=%{_usrsrc}/kernels/%{kversion}
make KDIR=${KERNEL_SRC} modules

%post
depmod %{kversion}

%postun
depmod %{kversion}

%install
KERNEL_SRC=%{_usrsrc}/kernels/%{kversion}
make KDIR=${KERNEL_SRC} INSTALL_MOD_PATH=$RPM_BUILD_ROOT modules_install
make KDIR=${KERNEL_SRC} HDR_INSTAL_PATH=$RPM_BUILD_ROOT/usr/include headers_install
rm -rf "$RPM_BUILD_ROOT/lib/modules/%{kversion}/modules."*

%clean
rm -rf $RPM_BUILD_ROOT

%files uapi-headers
%{_includedir}/linux/iommu_iova_map.h
%{_includedir}/uapi/misc/iommu_iova_map_user.h
%{_includedir}/uapi/misc/kiumd.h
%{_includedir}/uapi/misc/scm_user_intf.h
%{_includedir}/uapi/misc/qcom_uscmi.h

%files
/lib/modules/%{kversion}/extra/apps_pinctrl.ko
/lib/modules/%{kversion}/extra/scm_user_intf.ko
/lib/modules/%{kversion}/extra/vfio_iommu_qcom.ko
/lib/modules/%{kversion}/extra/iommu_iova_map.ko
/lib/modules/%{kversion}/extra/kiumd.ko
/lib/modules/%{kversion}/extra/qcom_uscmi.ko
/lib/modules/%{kversion}/extra/kryo_arm64_edac.ko
/lib/modules/%{kversion}/extra/kiumd_kgsl.ko

%changelog
* Mon Aug 28 2023 Deepti Jaggi <quic_djaggi@quicinc.com> 1.1
- Add Edac driver
* Fri Jul 27 2023 Venkatakrishnaiah Pari <quic_vpari@quicinc.com> 1.0
- First commit!
