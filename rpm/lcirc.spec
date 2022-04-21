Name:		lcirc
Version:	999.999
Release:	99999%{?dist}
Summary:	A terminal IRC client

Group:		Applications/Internet
License:	BSD
URL:		https://github.com/injinj/%{name}
Source0:	%{name}-%{version}-99999.tar.gz
BuildRoot:	${_tmppath}
Prefix:	        /usr
BuildRequires:  gcc-c++
BuildRequires:  linecook
BuildRequires:  libdecnumber
BuildRequires:  pcre2-devel
Requires:       pcre2
Requires:       linecook
Requires:       libdecnumber
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description
A terminal IRC client

%prep
%setup -q


%define _unpackaged_files_terminate_build 0
%define _missing_doc_files_terminate_build 0
%define _missing_build_ids_terminate_build 0
%define _include_gdb_index 1

%build
make build_dir=./usr %{?_smp_mflags} dist_bins
cp -a ./include ./usr/include
mkdir -p ./usr/share/doc/%{name}
cp -a README.md ./usr/share/doc/%{name}/

%install
rm -rf %{buildroot}
mkdir -p  %{buildroot}

# in builddir
cp -a * %{buildroot}

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
/usr/bin/*
/usr/share/doc/*

%post

%postun

%changelog
* __DATE__ <gchrisanderson@gmail.com>
- Hello world
