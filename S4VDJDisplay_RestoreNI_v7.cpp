// S4 VDJ Display - Restore NI driver v7
// Non-interactive restore tool for Inno Setup.
// Restores ONLY USB\VID_17CC&PID_1720&MI_04 to the Native Instruments driver.

#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cwctype>
#include <sstream>

#pragma comment(lib,"setupapi.lib")

namespace fs = std::filesystem;

static const std::wstring TARGET = L"USB\\VID_17CC&PID_1720&MI_04";

static std::wstring low(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static bool eqi(const std::wstring& a, const std::wstring& b)
{
    return low(a) == low(b);
}

static std::wstring rv(HKEY k, const wchar_t* n)
{
    wchar_t v[1024] = {};
    DWORD cb = sizeof(v), t = 0;

    if (RegQueryValueExW(k, n, nullptr, &t,
        (BYTE*)v, &cb) == ERROR_SUCCESS)
        return v;

    return L"";
}

static std::vector<std::wstring> hardwareIds(
    HDEVINFO s, SP_DEVINFO_DATA& d)
{
    DWORD t = 0, n = 0;
    std::vector<std::wstring> out;

    SetupDiGetDeviceRegistryPropertyW(
        s, &d, SPDRP_HARDWAREID,
        &t, nullptr, 0, &n);

    if (!n)
        return out;

    std::vector<BYTE> b(n + 4, 0);

    if (!SetupDiGetDeviceRegistryPropertyW(
        s, &d, SPDRP_HARDWAREID,
        &t, b.data(), (DWORD)b.size(), nullptr))
        return out;

    const wchar_t* p = (wchar_t*)b.data();

    while (*p)
    {
        out.emplace_back(p);
        p += wcslen(p) + 1;
    }

    return out;
}

struct DeviceInfo
{
    bool found = false;
    std::wstring inf;
    std::wstring provider;
    std::wstring service;
};

static DeviceInfo inspect()
{
    DeviceInfo r;

    HDEVINFO s = SetupDiGetClassDevsW(
        nullptr, nullptr, nullptr,
        DIGCF_ALLCLASSES | DIGCF_PRESENT);

    if (s == INVALID_HANDLE_VALUE)
        return r;

    for (DWORD i = 0;; i++)
    {
        SP_DEVINFO_DATA d{ sizeof(d) };

        if (!SetupDiEnumDeviceInfo(s, i, &d))
            break;

        bool match = false;

        for (auto& x : hardwareIds(s, d))
        {
            if (eqi(x, TARGET))
            {
                match = true;
                break;
            }
        }

        if (!match)
            continue;

        r.found = true;

        DWORD t = 0, n = 0;

        SetupDiGetDeviceRegistryPropertyW(
            s, &d, SPDRP_SERVICE,
            &t, nullptr, 0, &n);

        if (n)
        {
            std::vector<BYTE> b(n + 2, 0);

            if (SetupDiGetDeviceRegistryPropertyW(
                s, &d, SPDRP_SERVICE,
                &t, b.data(), (DWORD)b.size(), nullptr))
            {
                r.service = (wchar_t*)b.data();
            }
        }

        HKEY k = SetupDiOpenDevRegKey(
            s, &d,
            DICS_FLAG_GLOBAL,
            0,
            DIREG_DRV,
            KEY_READ);

        if (k != INVALID_HANDLE_VALUE)
        {
            r.inf = rv(k, L"InfPath");
            r.provider = rv(k, L"ProviderName");
            RegCloseKey(k);
        }

        break;
    }

    SetupDiDestroyDeviceInfoList(s);
    return r;
}

static bool admin()
{
    BOOL a = FALSE;
    PSID sid = nullptr;

    SID_IDENTIFIER_AUTHORITY nt =
        SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(
        &nt,
        2,
        SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0,
        &sid))
    {
        CheckTokenMembership(nullptr, sid, &a);
        FreeSid(sid);
    }

    return a == TRUE;
}

static std::string trim(std::string s)
{
    while (!s.empty() &&
           (s.back() == '\r' ||
            s.back() == ' ' ||
            s.back() == '\t'))
        s.pop_back();

    size_t p = 0;

    while (p < s.size() &&
           (s[p] == ' ' || s[p] == '\t'))
        p++;

    return s.substr(p);
}

static bool findNI(std::wstring& published)
{
    fs::path temp =
        fs::temp_directory_path() /
        L"S4VDJDisplay_pnputil.txt";

    std::wstring cmd =
        L"cmd.exe /C pnputil /enum-drivers > \"" +
        temp.wstring() + L"\"";

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> b(cmd.begin(), cmd.end());
    b.push_back(0);

    if (!CreateProcessW(
        nullptr,
        b.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi))
        return false;

    DWORD wait =
        WaitForSingleObject(pi.hProcess, 10000);

    DWORD exitCode = 1;

    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exitCode != 0)
        return false;

    std::ifstream in(temp, std::ios::binary);

    std::string line;
    std::string current;
    bool ok = false;

    while (std::getline(in, line))
    {
        std::string l = line;

        std::transform(
            l.begin(),
            l.end(),
            l.begin(),
            [](unsigned char c)
            {
                return (char)tolower(c);
            });

        auto c = line.find(':');

        std::string val =
            c == std::string::npos
            ? ""
            : trim(line.substr(c + 1));

        if (l.find("published name") != std::string::npos)
        {
            current = val;
        }

        if (l.find("original name") != std::string::npos &&
            l.find("niks4m3bd.inf") != std::string::npos)
        {
            published.assign(
                current.begin(),
                current.end());

            ok = !published.empty();
            break;
        }
    }

    in.close();

    std::error_code ec;
    fs::remove(temp, ec);

    return ok;
}

static bool runCommand(
    const std::wstring& command,
    DWORD& rc)
{
    std::vector<wchar_t> b(
        command.begin(),
        command.end());

    b.push_back(0);

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(
        nullptr,
        b.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi))
        return false;

    DWORD wait =
        WaitForSingleObject(
            pi.hProcess,
            30000);

    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    GetExitCodeProcess(
        pi.hProcess,
        &rc);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return true;
}

int wmain()
{
    if (!admin())
        return 10;

    DeviceInfo before = inspect();

    if (!before.found)
        return 20;

    // Already restored.
    if (eqi(before.provider, L"Native Instruments") &&
        !eqi(before.service, L"WinUSB"))
        return 0;

    std::wstring published;

    if (!findNI(published))
        return 30;

    // The published OEM name is normally oem7.inf.
    // It refers to the package already present in
    // C:\Windows\INF.
    std::wstring infPath =
        L"C:\\Windows\\INF\\" +
        published;

    if (GetFileAttributesW(infPath.c_str()) ==
        INVALID_FILE_ATTRIBUTES)
        return 40;

    std::wstring cmd =
        L"pnputil.exe /add-driver \"" +
        infPath +
        L"\" /install";

    DWORD rc = 1;

    if (!runCommand(cmd, rc))
        return 50;

    if (rc != 0)
        return 60;

    Sleep(1000);

    DeviceInfo after = inspect();

    if (!after.found)
        return 70;

    if (!eqi(after.provider, L"Native Instruments"))
        return 80;

    if (eqi(after.service, L"WinUSB"))
        return 90;

    return 0;
}