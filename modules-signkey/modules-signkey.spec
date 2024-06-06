# If kversion isn't defined on the rpmbuild line, define it here.
%{!?kversion: %define kversion %(uname -r)}

%{!?with_oot_debug:  %define with_oot_debug  0}

%define kmod_name modules-signkey

%define debug_package %{nil}
%if %{with_oot_debug}
    %define kpackage kernel-automotive-debug
    %define kversion_with_debug %{kversion}+debug
%else
    %define kversion_with_debug %{kversion}
    %define kpackage kernel-automotive
%endif


Name: %{kmod_name}
Version: 1.0
Release:        1%{?dist}
Summary: signkey for out of tree kernel modules

License: GPLv2
Source0: %{name}-%{version}.tar.gz

BuildRequires: kernel-automotive-devel-uname-r = %{kversion_with_debug}
Requires: %{kpackage}-core-uname-r = %{kversion_with_debug}

%description
This is rpm contains sign key for out of tree kernel modules.

%prep
%setup -qn %{name}

%build

%install
install_mod_path=%{buildroot}%{_usrsrc}/kernels/%{kversion_with_debug}
mkdir -p $install_mod_path/certs/
cp signing_key.pem $install_mod_path/certs/
cp signing_key.priv $install_mod_path/certs/
cp x509.genkey $install_mod_path/certs/

%clean
rm -rf $RPM_BUILD_ROOT

%files
%define kernel_module_path %{_usrsrc}/kernels/%{kversion_with_debug}
%{kernel_module_path}/certs/signing_key.pem
%{kernel_module_path}/certs/signing_key.priv
%{kernel_module_path}/certs/x509.genkey

%changelog
* Fri Jul 27 2023 Venkatakrishnaiah Pari <quic_vpari@quicinc.com> 1.0
- First commit!
