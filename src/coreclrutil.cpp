/*
**==============================================================================
**
** Copyright (c) Microsoft Corporation. All rights reserved. See file LICENSE
** for license information.
**
**==============================================================================
*/

#include "coreclrutil.h"
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <iostream>
#include <set>
#include <cstdlib>
#include <vector>
#include <sstream>
#include <algorithm>

// The name of the CoreCLR native runtime DLL
#if defined(__APPLE__)
const std::string coreClrDll = "/libcoreclr.dylib";
#else
const std::string coreClrDll = "/libcoreclr.so";
#endif

void* coreclrLib;

// Prototype of the coreclr_initialize function from the libcoreclr.so
typedef int (*InitializeCoreCLRFunction)(
    const char* exePath,
    const char* appDomainFriendlyName,
    int propertyCount,
    const char** propertyKeys,
    const char** propertyValues,
    void** hostHandle,
    unsigned int* domainId);

// Prototype of the coreclr_shutdown function from the libcoreclr.so
typedef int (*ShutdownCoreCLRFunction)(
    void* hostHandle,
    unsigned int domainId);

InitializeCoreCLRFunction initializeCoreCLR;
ShutdownCoreCLRFunction shutdownCoreCLR;
ExecuteAssemblyFunction executeAssembly;
CreateDelegateFunction createDelegate;

//
// Get the absolute path given the environment variable.
//
std::string GetEnvAbsolutePath(const char* env)
{
    char fullpath[PATH_MAX + 1];

    const char* local = std::getenv(env);
    if (!local)
    {
        std::cerr << "Could not read environment variable " << env << ", using default value." << std::endl;
        return std::string("");
    }

    char *ptr = realpath(local, fullpath);
    if (!ptr)
    {
        std::cerr << "Invalid environment variable " << env << " content, switching to default value. " << std::endl;
        return std::string("");
    }
    return std::string(ptr);
}

// Add all *.dll, *.ni.dll, *.exe, and *.ni.exe files from the specified directory to the tpaList string.
// Note: copied from unixcorerun
void AddFilesFromDirectoryToTpaList(const char* directory, std::string& tpaList)
{
    const char * const tpaExtensions[] = {
        ".ni.dll",      // Probe for .ni.dll first so that it's preferred if ni and il coexist in the same dir
        ".dll",
        ".ni.exe",
        ".exe",
    };

    DIR* dir = opendir(directory);
    if (dir == NULL)
    {
        return;
    }

    std::set<std::string> addedAssemblies;

    // Walk the directory for each extension separately so that we first get files with .ni.dll extension,
    // then files with .dll extension, etc.
    for (unsigned int extIndex = 0; extIndex < sizeof(tpaExtensions) / sizeof(tpaExtensions[0]); extIndex++)
    {
        const char* ext = tpaExtensions[extIndex];
        int extLength = strlen(ext);

        struct dirent* entry;

        // For all entries in the directory
        while ((entry = readdir(dir)) != NULL)
        {
            // We are interested in files only
            switch (entry->d_type)
            {
            case DT_REG:
                break;

                // Handle symlinks and file systems that do not support d_type
            case DT_LNK:
            case DT_UNKNOWN:
            {
                std::string fullFilename;

                fullFilename.append(directory);
                fullFilename.append("/");
                fullFilename.append(entry->d_name);

                struct stat sb;
                if (stat(fullFilename.c_str(), &sb) == -1)
                {
                    continue;
                }

                if (!S_ISREG(sb.st_mode))
                {
                    continue;
                }
            }
            break;

            default:
                continue;
            }

            std::string filename(entry->d_name);

            // Check if the extension matches the one we are looking for
            int extPos = filename.length() - extLength;
            if ((extPos <= 0) || (filename.compare(extPos, extLength, ext) != 0))
            {
                continue;
            }

            std::string filenameWithoutExt(filename.substr(0, extPos));

            // Make sure if we have an assembly with multiple extensions present,
            // we insert only one version of it.
            if (addedAssemblies.find(filenameWithoutExt) == addedAssemblies.end())
            {
                addedAssemblies.insert(filenameWithoutExt);

                tpaList.append(directory);
                tpaList.append("/");
                tpaList.append(filename);
                tpaList.append(":");
            }
        }

        // Rewind the directory stream to be able to iterate over it for the next extension
        rewinddir(dir);
    }

    closedir(dir);
}

struct ReleaseInfo
{
    std::string name;
    int major;
    int minor;
    int patch;
    std::string preReleaseIdentifier;
    int preReleaseVersion;
};

//
// Tokenize the directory names
//
bool ParseDirNames(const std::vector<std::string> &subdirs, std::vector<ReleaseInfo> &infos)
{
    for (std::vector<std::string>::const_iterator it=subdirs.begin(); it != subdirs.end(); ++it)
    {
        const std::string &name = *it;
        ReleaseInfo info;
        info.name = name;

        size_t s1 = name.find('.');
        if (std::string::npos == s1)
        {
            continue;
        }
        std::string majorStr = name.substr(0, s1);
        std::istringstream iss(majorStr);
        if ((iss >> info.major).fail())
        {
            continue;
        }

        size_t s2 = name.find('.', s1 + 1);
        if (std::string::npos == s2)
        {
            continue;
        }
        std::string minorStr = name.substr(s1 + 1, s2 - s1 - 1);
        std::istringstream iss1(minorStr);
        if ((iss1 >> info.minor).fail())
        {
            continue;
        }

        size_t s3 = name.find('-', s2 + 1);
        std::string patchStr;
        if (std::string::npos == s3)
        {
            patchStr = name.substr(s2 + 1);
        }
        else
        {
            patchStr = name.substr(s2 + 1, s3 - s2 - 1);
        }
        std::istringstream iss2(patchStr);
        if ((iss2 >> info.patch).fail())
        {
            continue;
        }

        if (std::string::npos == s3)
        {
            info.preReleaseIdentifier = "";
            info.preReleaseVersion = 0;
            infos.push_back(info);
            continue;
        }

        size_t s4 = name.find('.', s3 + 1);
        if (std::string::npos == s4)
        {
            info.preReleaseIdentifier = name.substr(s3 + 1);
            info.preReleaseVersion = 0;
            infos.push_back(info);
            continue;
        }
        info.preReleaseIdentifier = name.substr(s3 + 1, s4 - s3 - 1);
        std::string identifierValStr = name.substr(s4 + 1);
        std::istringstream iss4(identifierValStr);
        if ((iss4 >> info.preReleaseVersion).fail())
        {
            continue;
        }

        infos.push_back(info);
    }
    return !infos.empty();
}

//
// Convert preReleaseIdentifier to an integer value
// 
int convertIdentifierToInt(const std::string &id)
{
    // "" > "rc" > "beta" > "alpha"
        
    if (id.empty())
        return 4;

    std::string upperString;

    std::transform(id.begin(), id.end(), std::back_inserter(upperString), (int (*)(int))std::toupper);

    if (upperString == "RC")
    {
        return 3;
    }
    else if (upperString == "BETA")
    {
        return 2;
    }
    else if (upperString == "ALPHA")
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

//
// Find newest subdir
//
std::string getNewestSubDir(const std::vector<ReleaseInfo> &infos)
{
    size_t sz = infos.size();
    if (sz < 2)
    {
        return infos[0].name;
    }

    int newest = 0;
    // assume that no version numbers will be > 1024
    unsigned long long newestHash = ((((((((unsigned long long)infos[0].major << 10) + infos[0].minor) << 10) 
                                        + infos[0].patch) << 10) + convertIdentifierToInt(infos[0].preReleaseIdentifier)) << 10) 
                                        + infos[0].preReleaseVersion;

    for (size_t i = 1; i < sz; ++i)
    {
        unsigned long long nextHash = ((((((((unsigned long long)infos[i].major << 10) + infos[i].minor) << 10) 
                                          + infos[i].patch) << 10) + convertIdentifierToInt(infos[i].preReleaseIdentifier)) << 10) 
                                          + infos[i].preReleaseVersion;
        if (nextHash < newestHash)
        {
            continue;
        }
        else 
        {
            newest = i;
            newestHash = nextHash;
        }
    }

    return infos[newest].name;
}

//
// Search PowerShell subdirectories for newest
//
std::string getNewestPowerShellDir(const std::string &parentDir)
{
    // assume all subdirectories have the format:  x.x.x-x.x or x.x.x
    // e.g. 6.0.0-alpha.8, or 10.0.1

    if (parentDir.empty())
    {
        return parentDir;
    }

    DIR *dir = opendir(parentDir.c_str());
    if (NULL == dir)
    {
        return std::string("");
    }

    std::vector<std::string> subdirs;
    struct dirent *entry = readdir(dir);
    while (NULL != entry)
    {
        if (DT_DIR == entry->d_type && entry->d_name[0] != '.')
        {
            subdirs.push_back(entry->d_name);
        }
        entry = readdir(dir);
    }
    closedir(dir);

    if (subdirs.empty())
    {
        return parentDir;
    }
    
    std::vector<ReleaseInfo> subdirsInfo;
    bool ret = ParseDirNames(subdirs, subdirsInfo);

    if (!ret)
    {
        return parentDir;
    }

    return parentDir + '/' + getNewestSubDir(subdirsInfo);
}

//
// Below is our custom start/stop interface
//
int startCoreCLR(
    const char* appDomainFriendlyName,
    void** hostHandle,
    unsigned int* domainId)
{
    char exePath[PATH_MAX];

    // get path to current executable
    ssize_t len = readlink("/proc/self/exe", exePath, PATH_MAX);
    if (len == -1 || len == sizeof(exePath))
        len = 0;
    exePath[len] = '\0';

    // get the CoreCLR root path
    std::string clrAbsolutePath = GetEnvAbsolutePath("CORE_ROOT");
    if (clrAbsolutePath.empty())
    {
#if defined(__APPLE__)
        clrAbsolutePath = std::string("/usr/local/microsoft/powershell");
#else
        clrAbsolutePath = std::string("/opt/microsoft/powershell");
#endif
    }

    clrAbsolutePath = getNewestPowerShellDir(clrAbsolutePath);

    // get the CoreCLR shared library path
    std::string coreClrDllPath(clrAbsolutePath);
    coreClrDllPath += coreClrDll;

    // open the shared library
    coreclrLib = dlopen(coreClrDllPath.c_str(), RTLD_NOW|RTLD_LOCAL);
    if (coreclrLib == NULL)
    {
        std::cerr << "dlopen failed to open the CoreCLR library: " << dlerror() << std::endl;
        return -1;
    }

    // query and verify the function pointers
    initializeCoreCLR = (InitializeCoreCLRFunction)dlsym(coreclrLib,"coreclr_initialize");
    shutdownCoreCLR = (ShutdownCoreCLRFunction)dlsym(coreclrLib,"coreclr_shutdown");
    executeAssembly = (ExecuteAssemblyFunction)dlsym(coreclrLib,"coreclr_execute_assembly");
    createDelegate = (CreateDelegateFunction)dlsym(coreclrLib,"coreclr_create_delegate");

    if (initializeCoreCLR == NULL)
    {
        std::cerr << "function coreclr_initialize not found in CoreCLR library" << std::endl;
        return -1;
    }
    if (executeAssembly == NULL)
    {
        std::cerr << "function coreclr_execute_assembly not found in CoreCLR library" << std::endl;
        return -1;
    }
    if (shutdownCoreCLR == NULL)
    {
        std::cerr << "function coreclr_shutdown not found in CoreCLR library" << std::endl;
        return -1;
    }
    if (createDelegate == NULL)
    {
        std::cerr << "function coreclr_create_delegate not found in CoreCLR library" << std::endl;
        return -1;
    }

    // generate the Trusted Platform Assemblies list
    std::string tpaList;

    // add assemblies in the CoreCLR root path
    AddFilesFromDirectoryToTpaList(clrAbsolutePath.c_str(), tpaList);

    // create list of properties to initialize CoreCLR
    const char* propertyKeys[] = {
        "TRUSTED_PLATFORM_ASSEMBLIES",
        "APP_PATHS",
        "APP_NI_PATHS",
        "NATIVE_DLL_SEARCH_DIRECTORIES",
        "PLATFORM_RESOURCE_ROOTS",
        "AppDomainCompatSwitch",
        "SERVER_GC",
        "APP_CONTEXT_BASE_DIRECTORY"
    };

    // We use the CORE_ROOT for just about everything: trusted
    // platform assemblies, DLLs, native DLLs, resources, and the
    // AppContext.BaseDirectory. Server garbage collection is disabled
    // by default because of dotnet/cli#652
    const char* propertyValues[] = {
        // TRUSTED_PLATFORM_ASSEMBLIES
        tpaList.c_str(),
        // APP_PATHS
        clrAbsolutePath.c_str(),
        // APP_NI_PATHS
        clrAbsolutePath.c_str(),
        // NATIVE_DLL_SEARCH_DIRECTORIES
        clrAbsolutePath.c_str(),
        // PLATFORM_RESOURCE_ROOTS
        clrAbsolutePath.c_str(),
        // AppDomainCompatSwitch
        "UseLatestBehaviorWhenTFMNotSpecified",
        // SERVER_GC
        "0",
        // APP_CONTEXT_BASE_DIRECTORY
        clrAbsolutePath.c_str()
    };

    // initialize CoreCLR
    int status = initializeCoreCLR(
        exePath,
        appDomainFriendlyName,
        sizeof(propertyKeys)/sizeof(propertyKeys[0]),
        propertyKeys,
        propertyValues,
        hostHandle,
        domainId);

    return status;
}

int stopCoreCLR(void* hostHandle, unsigned int domainId)
{
    // shutdown CoreCLR
    int status = shutdownCoreCLR(hostHandle, domainId);
    if (!SUCCEEDED(status))
    {
        std::cerr << "coreclr_shutdown failed - status: " << std::hex << status << std::endl;
    }

    // close the dynamic library
    if (0 != dlclose(coreclrLib))
    {
        std::cerr << "failed to close CoreCLR library" << std::endl;
    }

    return status;
}
