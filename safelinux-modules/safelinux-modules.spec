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
KSRC=%{_usrsrc}/kernels/%{kversion}
make KDIR=${KSRC} modules

%post
depmod %{kversion}

%postun
depmod %{kversion}

%install
KSRC=%{_usrsrc}/kernels/%{kversion}
make KDIR=${KSRC} INSTALL_MOD_PATH=$RPM_BUILD_ROOT modules_install
make KDIR=${KSRC} HDR_INSTAL_PATH=$RPM_BUILD_ROOT/usr/include headers_install
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
/lib/modules/%{kversion}/extra/drivers/apps_pinctrl.ko
/lib/modules/%{kversion}/extra/drivers/scm_user_intf.ko
/lib/modules/%{kversion}/extra/drivers/vfio_iommu_qcom.ko
/lib/modules/%{kversion}/extra/drivers/iommu_iova_map.ko
/lib/modules/%{kversion}/extra/drivers/kiumd.ko
/lib/modules/%{kversion}/extra/drivers/qcom_uscmi.ko
/lib/modules/%{kversion}/extra/drivers/kryo_arm64_edac.ko

%changelog
* Wed Aug 28 2023 Deepti Jaggi <quic_djaggi@quicinc.com> 1.1
- Add Edac driver
* Fri Jul 27 2023 Venkatakrishnaiah Pari <quic_vpari@quicinc.com> 1.0
- First commit!
