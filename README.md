PowerShell Remoting Protocol [![Build Status](https://travis-ci.com/PowerShell/psl-omi-provider.svg?token=31YifM4jfyVpBmEGitCm&branch=master)](https://travis-ci.com/PowerShell/psl-omi-provider)
============================

PSRP communication is tunneled through the [Open Management
Infrastructure (OMI)][omi] using this OMI provider.

[omi]: https://github.com/PowerShell/omi

Get PSRP
========

You can download and install PSRP from following links:

| Platform     | Releases           | Link                             |
|--------------|--------------------|----------------------------------|
| Linux        | Debian             | [psrp-0.1.0-0.universal.x64.deb] |
| Linux        | RPM                | [psrp-0.1.0-0.universal.x64.rpm] |

[psrp-0.1.0-0.universal.x64.deb]: https://github.com/PowerShell/psl-omi-provider/releases/download/v0.1.0.alpha1/psrp-0.1.0-0.universal.x64.deb
[psrp-0.1.0-0.universal.x64.rpm]: https://github.com/PowerShell/psl-omi-provider/releases/download/v0.1.0.alpha1/psrp-0.1.0-0.universal.x64.rpm

Package Requirement
-------------------

Prior to installing PSRP, make sure that OMI package is installed.
Prior to running PSRP, make sure that PowerShell is installed.

Environment
===========

Toolchain Setup
---------------

PSRP requires the following packages:

Ubuntu 14.04:

```sh
sudo apt-get install libpam0g-dev libssl-dev libcurl4-openssl-dev
```

Mac OS X 10.11:

```sh
/usr/bin/ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
brew install pkg-config
```

Also install [PowerShell][] from the latest release per their instructions.

[powershell]: https://github.com/PowerShell/PowerShell

Git Setup
---------

PSRP has a submodule, so clone recursively.

```sh
git clone --recursive https://github.com/PowerShell/psl-omi-provider.git
```

Building
========

Run `./build.sh` to build OMI and the provider.

This script first builds OMI in developer mode:

```sh
pushd omi/Unix
./configure --dev
make -j
popd
```

Then it builds and registers the provider:

```sh
pushd src
cmake -DCMAKE_BUILD_TYPE=Debug .
make -j
popd
```

The provider maintains its own native host library to initialize the
CLR, but there are plans to refactor .NET's packaged host as a shared
library.

Running
-------

Some initial setup on Windows is required. Open an administrative command
prompt and execute the following:

```cmd
winrm set winrm/config/Client @{AllowUnencrypted="true"}
winrm set winrm/config/Client @{TrustedHosts="*"}
```

> You can also set the `TrustedHosts` to include the target's IP address.

Then on Linux, launch `omiserver` (after building with the
instructions above):

```sh
./run.sh
```

Now in a PowerShell prompt on Windows (opened after setting the WinRM client
configurations):

```powershell
Enter-PSSession -ComputerName <IP address of Linux machine> -Credential $cred -Authentication basic
```

The IP address of the Linux machine can be obtained with:

```sh
ip -f inet addr show dev eth0
```
