# If kversion isn't defined on the rpmbuild line, define it here.
%{!?kversion: %define kversion %(uname -r)}

%{!?with_oot_debug:  %define with_oot_debug  0}

%define kmod_name modules-signkey

%define debug_package %{nil}

Name: %{kmod_name}
Version: 1.0
Release:        1%{?dist}
Summary: signkey for out of tree kernel modules

License: GPLv2
Source0: %{name}-%{version}.tar.gz

BuildRequires: kernel-automotive-devel-uname-r = %{kversion}
Requires: kernel-automotive-core-uname-r = %{kversion}

%description
This is rpm contains sign key for out of tree kernel modules.

%prep
%setup -qn %{name}

%build

%install
%if %{with_oot_debug}
install_mod_path=%{buildroot}%{_usrsrc}/kernels/%{kversion}+debug
%else
install_mod_path=%{buildroot}%{_usrsrc}/kernels/%{kversion}
%endif
mkdir -p $install_mod_path/certs/
cp signing_key.pem $install_mod_path/certs/
cp signing_key.priv $install_mod_path/certs/
cp x509.genkey $install_mod_path/certs/

%clean
rm -rf $RPM_BUILD_ROOT

%files
%if %{with_oot_debug}
%define kernel_module_path %{_usrsrc}/kernels/%{kversion}+debug
%else
%define kernel_module_path %{_usrsrc}/kernels/%{kversion}
%endif
%{kernel_module_path}/certs/signing_key.pem
%{kernel_module_path}/certs/signing_key.priv
%{kernel_module_path}/certs/x509.genkey

%changelog
* Fri Jul 27 2023 Venkatakrishnaiah Pari <quic_vpari@quicinc.com> 1.0
- First commit!
