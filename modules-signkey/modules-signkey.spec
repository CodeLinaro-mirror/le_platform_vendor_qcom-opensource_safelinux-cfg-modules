# If kversion isn't defined on the rpmbuild line, define it here.
%{!?kversion: %define kversion %(uname -r)}

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
mkdir -p %{buildroot}/%{_usrsrc}/kernels/%{kversion}/certs/
cp signing_key.pem %{buildroot}/%{_usrsrc}/kernels/%{kversion}/certs/
cp signing_key.priv %{buildroot}/%{_usrsrc}/kernels/%{kversion}/certs/
cp x509.genkey %{buildroot}/%{_usrsrc}/kernels/%{kversion}/certs/

%clean
rm -rf $RPM_BUILD_ROOT

%files
%{_usrsrc}/kernels/%{kversion}/certs/signing_key.pem
%{_usrsrc}/kernels/%{kversion}/certs/signing_key.priv
%{_usrsrc}/kernels/%{kversion}/certs/x509.genkey

%changelog
* Fri Jul 27 2023 Venkatakrishnaiah Pari <quic_vpari@quicinc.com> 1.0
- First commit!
