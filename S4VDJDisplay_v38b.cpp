#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef NODLLEXPORT
#define NODLLEXPORT
#endif
#include "vdjPlugin8.h"
#include "vdjDsp8.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <limits>
#include <filesystem>

// ============================================================
// S4 MK3 -> VirtualDJ native display
// v26
//
// Based on the proven Python s4_vdj_network_live_v3 layout/data.
// USB transport uses the same working libusb-1.0.dll path.
// No HTTP / Network Control / Python required.
// v24 diagnostic: deck-bound Buffer DSP instance test
//
// Display 0 follows get_leftdeck: A=1 / C=3
// Display 1 follows get_rightdeck: B=2 / D=4
// ============================================================

static constexpr USHORT S4_VID = 0x17CC;
static constexpr USHORT S4_PID = 0x1720;
static constexpr int S4_INTERFACE = 4;
static constexpr unsigned char S4_ENDPOINT = 0x03;

static constexpr int W = 320;
static constexpr int H = 240;


static std::wstring getEnvW(const wchar_t* name)
{
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0)
        return L"";
    std::wstring value(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, value.data(), n);
    if (got == 0 || got >= n)
        return L"";
    value.resize(got);
    return value;
}

static std::wstring searchPathW(const wchar_t* file)
{
    DWORD n = SearchPathW(nullptr, file, nullptr, 0, nullptr, nullptr);
    if (n == 0)
        return L"";
    std::wstring path(n + 1, L'\0');
    DWORD got = SearchPathW(nullptr, file, nullptr,
                            static_cast<DWORD>(path.size()),
                            path.data(), nullptr);
    if (got == 0 || got >= path.size())
        return L"";
    path.resize(got);
    return path;
}

static bool fileExistsW(const std::wstring& path)
{
    if (path.empty())
        return false;
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring thisModuleDirectory()
{
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&thisModuleDirectory),
            &hm))
        return L"";

    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(hm, path.data(),
                                static_cast<DWORD>(path.size()));
    if (n == 0 || n >= path.size())
        return L"";
    path.resize(n);
    return std::filesystem::path(path).parent_path().wstring();
}

static std::wstring findLibusbDll()
{
    // First choice: libusb-1.0.dll next to this plugin DLL.
    const std::wstring dir = thisModuleDirectory();
    if (!dir.empty())
    {
        const std::filesystem::path local =
            std::filesystem::path(dir) / L"libusb-1.0.dll";
        if (fileExistsW(local.wstring()))
            return local.wstring();
    }

    // Development fallback: normal Windows/PATH lookup.
    return searchPathW(L"libusb-1.0.dll");
}

static std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                        static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

static void logLine(const std::string& s)
{
    std::ofstream f(R"(C:\temp\S4AutoStartTest.log)", std::ios::app);
    if (f)
        f << s << "\r\n";
}

static std::string errHex(int v)
{
    std::ostringstream ss;
    ss << v
       << " (0x"
       << std::uppercase << std::hex
       << std::setw(8) << std::setfill('0')
       << static_cast<unsigned int>(v)
       << ")";
    return ss.str();
}

// ============================================================
// Minimal dynamic libusb declarations
// ============================================================

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

struct libusb_device_descriptor
{
    uint8_t  bLength;
    uint8_t  bDeviceDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};

using pfn_libusb_init =
    int (__cdecl *)(libusb_context**);

using pfn_libusb_exit =
    void (__cdecl *)(libusb_context*);

using pfn_libusb_get_device_list =
    intptr_t (__cdecl *)(libusb_context*, libusb_device***);

using pfn_libusb_free_device_list =
    void (__cdecl *)(libusb_device**, int);

using pfn_libusb_get_device_descriptor =
    int (__cdecl *)(libusb_device*, libusb_device_descriptor*);

using pfn_libusb_open =
    int (__cdecl *)(libusb_device*, libusb_device_handle**);

using pfn_libusb_close =
    void (__cdecl *)(libusb_device_handle*);

using pfn_libusb_claim_interface =
    int (__cdecl *)(libusb_device_handle*, int);

using pfn_libusb_release_interface =
    int (__cdecl *)(libusb_device_handle*, int);

using pfn_libusb_bulk_transfer =
    int (__cdecl *)(libusb_device_handle*,
                    unsigned char,
                    unsigned char*,
                    int,
                    int*,
                    unsigned int);

// ============================================================
// RGB framebuffer - matches Python v3
// ============================================================

struct RGB
{
    uint8_t r, g, b;
};

static constexpr RGB BLACK   { 3,   5,   9   };
static constexpr RGB DARK    { 10,  15,  24  };
static constexpr RGB GRID    { 24,  34,  48  };
static constexpr RGB WHITE   { 235, 245, 255 };
static constexpr RGB GREY    { 120, 140, 160 };
static constexpr RGB CYAN    { 0,   205, 255 };
static constexpr RGB BLUE    { 50,  110, 255 };
static constexpr RGB GREEN   { 50,  230, 130 };
static constexpr RGB YELLOW  { 255, 205, 40 };
static constexpr RGB ORANGE  { 255, 150, 45 };
static constexpr RGB RED     { 255, 65,  70  };
static constexpr RGB MAGENTA { 240, 70,  210 };

static inline uint16_t rgb565(RGB c)
{
    return static_cast<uint16_t>(
        ((c.b & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.r >> 3)
    );
}

static const char* FONT[43][7] =
{
    // 0-9
    {"01110","10001","10011","10101","11001","10001","01110"},
    {"00100","01100","00100","00100","00100","00100","01110"},
    {"01110","10001","00001","00010","00100","01000","11111"},
    {"11110","00001","00001","01110","00001","00001","11110"},
    {"00010","00110","01010","10010","11111","00010","00010"},
    {"11111","10000","10000","11110","00001","00001","11110"},
    {"01110","10000","10000","11110","10001","10001","01110"},
    {"11111","00001","00010","00100","01000","01000","01000"},
    {"01110","10001","10001","01110","10001","10001","01110"},
    {"01110","10001","10001","01111","00001","00001","01110"},

    // A-Z
    {"01110","10001","10001","11111","10001","10001","10001"},
    {"11110","10001","10001","11110","10001","10001","11110"},
    {"01111","10000","10000","10000","10000","10000","01111"},
    {"11110","10001","10001","10001","10001","10001","11110"},
    {"11111","10000","10000","11110","10000","10000","11111"},
    {"11111","10000","10000","11110","10000","10000","10000"},
    {"01111","10000","10000","10111","10001","10001","01111"},
    {"10001","10001","10001","11111","10001","10001","10001"},
    {"11111","00100","00100","00100","00100","00100","11111"},
    {"00111","00010","00010","00010","00010","10010","01100"},
    {"10001","10010","10100","11000","10100","10010","10001"},
    {"10000","10000","10000","10000","10000","10000","11111"},
    {"10001","11011","10101","10101","10001","10001","10001"},
    {"10001","11001","10101","10011","10001","10001","10001"},
    {"01110","10001","10001","10001","10001","10001","01110"},
    {"11110","10001","10001","11110","10000","10000","10000"},
    {"01110","10001","10001","10001","10101","10010","01101"},
    {"11110","10001","10001","11110","10100","10010","10001"},
    {"01111","10000","10000","01110","00001","00001","11110"},
    {"11111","00100","00100","00100","00100","00100","00100"},
    {"10001","10001","10001","10001","10001","10001","01110"},
    {"10001","10001","10001","10001","10001","01010","00100"},
    {"10001","10001","10001","10101","10101","11011","10001"},
    {"10001","10001","01010","00100","01010","10001","10001"},
    {"10001","10001","01010","00100","00100","00100","00100"},
    {"11111","00001","00010","00100","01000","10000","11111"},

    // space, '.', ':', '-', '%', '/', '+'
    {"00000","00000","00000","00000","00000","00000","00000"},
    {"00000","00000","00000","00000","00000","00000","00100"},
    {"00000","00100","00100","00000","00100","00100","00000"},
    {"00000","00000","00000","11111","00000","00000","00000"},
    {"11001","11010","00010","00100","01000","01011","10011"},
    {"00001","00010","00100","01000","10000","00000","00000"},
    {"00000","00100","00100","11111","00100","00100","00000"},
};

static int glyphIndex(char ch)
{
    ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));

    if (ch >= '0' && ch <= '9')
        return ch - '0';

    if (ch >= 'A' && ch <= 'Z')
        return 10 + (ch - 'A');

    switch (ch)
    {
        case ' ': return 36;
        case '.': return 37;
        case ':': return 38;
        case '-': return 39;
        case '%': return 40;
        case '/': return 41;
        case '+': return 42;
        default:  return 36;
    }
}

class Frame
{
public:
    std::vector<uint8_t> p;

    Frame()
        : p(static_cast<size_t>(W) * H * 2)
    {
        clear(BLACK);
    }

    void clear(RGB c)
    {
        const uint16_t v = rgb565(c);
        const uint8_t hi = static_cast<uint8_t>(v >> 8);
        const uint8_t lo = static_cast<uint8_t>(v & 0xFF);

        for (size_t i = 0; i < p.size(); i += 2)
        {
            p[i] = hi;
            p[i + 1] = lo;
        }
    }

    void rect(int x, int y, int w, int h, RGB c)
    {
        if (w <= 0 || h <= 0)
            return;

        const int x0 = (std::max)(0, x);
        const int y0 = (std::max)(0, y);
        const int x1 = (std::min)(W, x + w);
        const int y1 = (std::min)(H, y + h);

        const uint16_t v = rgb565(c);
        const uint8_t hi = static_cast<uint8_t>(v >> 8);
        const uint8_t lo = static_cast<uint8_t>(v & 0xFF);

        for (int yy = y0; yy < y1; ++yy)
        {
            for (int xx = x0; xx < x1; ++xx)
            {
                const size_t i =
                    static_cast<size_t>((yy * W + xx) * 2);

                p[i] = hi;
                p[i + 1] = lo;
            }
        }
    }

    void text(
        const std::string& s,
        int x,
        int y,
        int scale = 1,
        RGB c = WHITE,
        int spacing = 1
    )
    {
        if (scale <= 0)
            return;

        for (char ch : s)
        {
            const int gi = glyphIndex(ch);

            for (int gy = 0; gy < 7; ++gy)
            {
                for (int gx = 0; gx < 5; ++gx)
                {
                    if (FONT[gi][gy][gx] == '1')
                    {
                        rect(
                            x + gx * scale,
                            y + gy * scale,
                            scale,
                            scale,
                            c
                        );
                    }
                }
            }

            x += (5 + spacing) * scale;
        }
    }
};

// ============================================================
// S4 libusb transport
// ============================================================

class LibUsbS4
{
public:
    ~LibUsbS4()
    {
        close();
    }

    bool open()
    {
        close();

        logLine("USB: using libusb backend");

        if (!loadLibrary())
            return false;

        int r = fn_init_(&ctx_);
        logLine("USB: libusb_init=" + errHex(r));

        if (r != 0)
            return false;

        libusb_device** list = nullptr;

        intptr_t count =
            fn_get_device_list_(ctx_, &list);

        logLine(
            "USB: libusb_get_device_list=" +
            std::to_string(count)
        );

        if (count < 0 || !list)
            return false;

        libusb_device* found = nullptr;

        for (intptr_t i = 0; i < count; ++i)
        {
            libusb_device* dev = list[i];

            if (!dev)
                continue;

            libusb_device_descriptor desc{};

            r = fn_get_device_descriptor_(dev, &desc);

            if (r != 0)
                continue;

            if (desc.idVendor == S4_VID &&
                desc.idProduct == S4_PID)
            {
                found = dev;
                break;
            }
        }

        if (!found)
        {
            fn_free_device_list_(list, 1);
            logLine("USB: S4 NOT FOUND");
            return false;
        }

        r = fn_open_(found, &handle_);

        logLine(
            "USB: libusb_open=" +
            errHex(r)
        );

        if (r != 0 || !handle_)
        {
            fn_free_device_list_(list, 1);
            return false;
        }

        r = fn_claim_interface_(
            handle_,
            S4_INTERFACE
        );

        logLine(
            "USB: libusb_claim_interface(4)=" +
            errHex(r)
        );

        fn_free_device_list_(list, 1);

        if (r != 0)
            return false;

        claimed_ = true;

        logLine(
            "USB: OPEN SUCCESS - interface 4 claimed"
        );

        return true;
    }

    bool sendScreen(
        int screen,
        const std::vector<uint8_t>& pixels
    )
    {
        if (!handle_ ||
            !claimed_ ||
            pixels.size() !=
                static_cast<size_t>(W) * H * 2)
        {
            logLine(
                "USB: sendScreen refused "
                "(not open or wrong buffer size)"
            );
            return false;
        }

        std::vector<uint8_t> packet;
        packet.reserve(16 + pixels.size() + 4);

        // Exact packet used by the working Python code.
        packet.push_back(0x84);
        packet.push_back(0x00);
        packet.push_back(
            static_cast<uint8_t>(screen)
        );
        packet.push_back(0x21);

        packet.push_back(0x00);
        packet.push_back(0x00);
        packet.push_back(0x00);
        packet.push_back(0x00);

        packet.push_back(0x00);
        packet.push_back(0x00); // X

        packet.push_back(0x00);
        packet.push_back(0x00); // Y

        packet.push_back(0x01);
        packet.push_back(0x40); // W = 320

        packet.push_back(0x00);
        packet.push_back(0xF0); // H = 240

        packet.insert(
            packet.end(),
            pixels.begin(),
            pixels.end()
        );

        packet.push_back(0x40);
        packet.push_back(0x00);
        packet.push_back(
            static_cast<uint8_t>(screen)
        );
        packet.push_back(0x00);

        int transferred = 0;

        int r = fn_bulk_transfer_(
            handle_,
            S4_ENDPOINT,
            packet.data(),
            static_cast<int>(packet.size()),
            &transferred,
            5000
        );

        if (r != 0 || transferred != static_cast<int>(packet.size()))
        {
            std::ostringstream ss;
            ss << "USB: bulk screen=" << screen
               << " bytes=" << packet.size()
               << " transferred=" << transferred
               << " result=" << errHex(r);
            logLine(ss.str());
        }

        return r == 0 &&
               transferred ==
                   static_cast<int>(packet.size());
    }

    void close()
    {
        if (handle_)
        {
            if (claimed_)
            {
                int r =
                    fn_release_interface_(
                        handle_,
                        S4_INTERFACE
                    );

                logLine(
                    "USB: "
                    "libusb_release_interface(4)=" +
                    errHex(r)
                );

                claimed_ = false;
            }

            fn_close_(handle_);
            handle_ = nullptr;
        }

        if (ctx_)
        {
            fn_exit_(ctx_);
            ctx_ = nullptr;
        }

        if (dll_)
        {
            FreeLibrary(dll_);
            dll_ = nullptr;
        }
    }

private:
    bool loadLibrary()
    {
        const std::wstring libusbPath = findLibusbDll();
        if (!libusbPath.empty())
        {
            dll_ = LoadLibraryW(libusbPath.c_str());
            if (dll_)
                logLine("USB: libusb DLL=" + wideToUtf8(libusbPath));
        }

        if (!dll_)
        {
            // Last chance: let Windows apply its normal DLL search rules.
            dll_ = LoadLibraryW(L"libusb-1.0.dll");
            if (dll_)
                logLine("USB: libusb DLL loaded by normal Windows search");
        }

        if (!dll_)
        {
            logLine(
                "USB: libusb-1.0.dll NOT FOUND, "
                "Win32 error=" +
                std::to_string(GetLastError())
            );

            return false;
        }

        fn_init_ =
            reinterpret_cast<pfn_libusb_init>(
                GetProcAddress(
                    dll_,
                    "libusb_init"
                )
            );

        fn_exit_ =
            reinterpret_cast<pfn_libusb_exit>(
                GetProcAddress(
                    dll_,
                    "libusb_exit"
                )
            );

        fn_get_device_list_ =
            reinterpret_cast<
                pfn_libusb_get_device_list
            >(
                GetProcAddress(
                    dll_,
                    "libusb_get_device_list"
                )
            );

        fn_free_device_list_ =
            reinterpret_cast<
                pfn_libusb_free_device_list
            >(
                GetProcAddress(
                    dll_,
                    "libusb_free_device_list"
                )
            );

        fn_get_device_descriptor_ =
            reinterpret_cast<
                pfn_libusb_get_device_descriptor
            >(
                GetProcAddress(
                    dll_,
                    "libusb_get_device_descriptor"
                )
            );

        fn_open_ =
            reinterpret_cast<pfn_libusb_open>(
                GetProcAddress(
                    dll_,
                    "libusb_open"
                )
            );

        fn_close_ =
            reinterpret_cast<pfn_libusb_close>(
                GetProcAddress(
                    dll_,
                    "libusb_close"
                )
            );

        fn_claim_interface_ =
            reinterpret_cast<
                pfn_libusb_claim_interface
            >(
                GetProcAddress(
                    dll_,
                    "libusb_claim_interface"
                )
            );

        fn_release_interface_ =
            reinterpret_cast<
                pfn_libusb_release_interface
            >(
                GetProcAddress(
                    dll_,
                    "libusb_release_interface"
                )
            );

        fn_bulk_transfer_ =
            reinterpret_cast<
                pfn_libusb_bulk_transfer
            >(
                GetProcAddress(
                    dll_,
                    "libusb_bulk_transfer"
                )
            );

        if (!fn_init_ ||
            !fn_exit_ ||
            !fn_get_device_list_ ||
            !fn_free_device_list_ ||
            !fn_get_device_descriptor_ ||
            !fn_open_ ||
            !fn_close_ ||
            !fn_claim_interface_ ||
            !fn_release_interface_ ||
            !fn_bulk_transfer_)
        {
            logLine(
                "USB: required libusb export missing"
            );

            FreeLibrary(dll_);
            dll_ = nullptr;

            return false;
        }

        logLine(
            "USB: required libusb exports OK"
        );

        return true;
    }

    static std::string narrow(
        const wchar_t* ws
    )
    {
        if (!ws)
            return {};

        const int len =
            static_cast<int>(wcslen(ws));

        if (len <= 0)
            return {};

        const int n =
            WideCharToMultiByte(
                CP_UTF8,
                0,
                ws,
                len,
                nullptr,
                0,
                nullptr,
                nullptr
            );

        if (n <= 0)
            return {};

        std::string out(
            static_cast<size_t>(n),
            '\0'
        );

        WideCharToMultiByte(
            CP_UTF8,
            0,
            ws,
            len,
            out.data(),
            n,
            nullptr,
            nullptr
        );

        return out;
    }

private:
    HMODULE dll_ = nullptr;
    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    bool claimed_ = false;

    pfn_libusb_init fn_init_ = nullptr;
    pfn_libusb_exit fn_exit_ = nullptr;
    pfn_libusb_get_device_list fn_get_device_list_ = nullptr;
    pfn_libusb_free_device_list fn_free_device_list_ = nullptr;
    pfn_libusb_get_device_descriptor fn_get_device_descriptor_ = nullptr;
    pfn_libusb_open fn_open_ = nullptr;
    pfn_libusb_close fn_close_ = nullptr;
    pfn_libusb_claim_interface fn_claim_interface_ = nullptr;
    pfn_libusb_release_interface fn_release_interface_ = nullptr;
    pfn_libusb_bulk_transfer fn_bulk_transfer_ = nullptr;
};

// ============================================================
// VDJ data - same commands as Python v3
// ============================================================

struct DeckData
{
    std::string title;
    std::string key;
    std::string filepath;

    double bpm = 0.0;
    double elapsed = 0.0;
    double remain = 0.0;
    double loop = 0.0;
    double playing = 0.0;
};

static std::string getString(
    IVdjPlugin8* p,
    const std::string& command
)
{
    char buffer[512]{};

    HRESULT hr =
        p->GetStringInfo(
            command.c_str(),
            buffer,
            sizeof(buffer)
        );

    if (hr != S_OK)
    {
        return {};
    }

    return std::string(buffer);
}

static double getValue(
    IVdjPlugin8* p,
    const std::string& command
)
{
    double value = 0.0;

    HRESULT hr =
        p->GetInfo(
            command.c_str(),
            &value
        );

    if (hr != S_OK)
    {
        return 0.0;
    }

    return value;
}

static DeckData queryDeck(
    IVdjPlugin8* p,
    int deck
)
{
    DeckData d;

    const std::string prefix =
        "deck " +
        std::to_string(deck) +
        " ";

    d.title =
        getString(
            p,
            prefix + "get_title"
        );

    d.filepath =
        getString(
            p,
            prefix + "get_filepath"
        );

    d.bpm =
        getValue(
            p,
            prefix + "get_bpm"
        );

    d.elapsed =
        getValue(
            p,
            prefix + "get_time \"elapsed\""
        );

    d.remain =
        getValue(
            p,
            prefix + "get_time \"remain\""
        );

    d.key =
        getString(
            p,
            prefix + "get_key \"harmonic\""
        );

    d.loop =
        getValue(
            p,
            prefix + "get_loop"
        );

    d.playing =
        getValue(
            p,
            prefix + "play"
        );

    return d;
}

static int getActiveDeck(
    IVdjPlugin8* p,
    const char* command,
    int fallback
)
{
    const double raw =
        getValue(p, command);

    const int deck =
        static_cast<int>(
            std::llround(raw)
        );

    if (deck >= 1 && deck <= 4)
        return deck;

    return fallback;
}

static std::string fmtSeconds(
    double milliseconds
)
{
    const int total =
        (std::max)(
            0,
            static_cast<int>(
                milliseconds / 1000.0
            )
        );

    const int minutes = total / 60;
    const int seconds = total % 60;

    char out[32]{};

    sprintf_s(
        out,
        "%02d:%02d",
        minutes,
        seconds
    );

    return out;
}

static std::string fmtBpm(double bpm)
{
    char out[32]{};

    sprintf_s(
        out,
        "%05.1f",
        bpm
    );

    return out;
}


// ============================================================
// V36: VirtualDJ cache waveform extraction + render
//
// VirtualDJ's live cache.db is locked while VDJ is running.
// V31 therefore copies cache.db (and WAL if present) to a temporary folder
// and inspects only that disposable copy. The live VDJ database is never
// opened by SQLite and is never modified.
// ============================================================

static std::string quoteWindowsArg(const std::string& s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}


static std::string deckName(int deck)
{
    switch (deck)
    {
        case 1: return "A";
        case 2: return "B";
        case 3: return "C";
        case 4: return "D";
        default: return "?";
    }
}

// ============================================================
// V29 file-based RMS waveform cache
//
// The waveform is built from the actual audio file, never from the live
// audio callback. This deliberately keeps all potentially expensive work
// out of OnGetSongBuffer().
//
// The cache contains only the compact display representation: one peak value
// per waveform bin. The source file can therefore be many millions of frames
// without consuming significant RAM.
// ============================================================


struct sqlite3;
struct sqlite3_stmt;

class WinSqlite
{
public:
    using open_v2_t = int (__cdecl*)(const char*, sqlite3**, int, const char*);
    using close_t = int (__cdecl*)(sqlite3*);
    using prepare_v2_t = int (__cdecl*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
    using bind_text_t = int (__cdecl*)(sqlite3_stmt*, int, const char*, int, void(__cdecl*)(void*));
    using step_t = int (__cdecl*)(sqlite3_stmt*);
    using column_int64_t = long long (__cdecl*)(sqlite3_stmt*, int);
    using column_double_t = double (__cdecl*)(sqlite3_stmt*, int);
    using column_text_t = const unsigned char* (__cdecl*)(sqlite3_stmt*, int);
    using column_blob_t = const void* (__cdecl*)(sqlite3_stmt*, int);
    using column_bytes_t = int (__cdecl*)(sqlite3_stmt*, int);
    using finalize_t = int (__cdecl*)(sqlite3_stmt*);
    using errmsg_t = const char* (__cdecl*)(sqlite3*);

    HMODULE dll = nullptr;
    open_v2_t open_v2 = nullptr;
    close_t close = nullptr;
    prepare_v2_t prepare_v2 = nullptr;
    bind_text_t bind_text = nullptr;
    step_t step = nullptr;
    column_int64_t column_int64 = nullptr;
    column_double_t column_double = nullptr;
    column_text_t column_text = nullptr;
    column_blob_t column_blob = nullptr;
    column_bytes_t column_bytes = nullptr;
    finalize_t finalize = nullptr;
    errmsg_t errmsg = nullptr;

    ~WinSqlite() { if (dll) FreeLibrary(dll); }

    bool load()
    {
        // Windows 10/11 ship winsqlite3.dll. Loading dynamically means no SDK
        // header/import library is required by this project.
        dll = LoadLibraryW(L"winsqlite3.dll");
        if (!dll)
            return false;

#define SQLSYM(member, name) \
        member = reinterpret_cast<member##_t>(GetProcAddress(dll, name)); \
        if (!member) return false
        SQLSYM(open_v2, "sqlite3_open_v2");
        SQLSYM(close, "sqlite3_close");
        SQLSYM(prepare_v2, "sqlite3_prepare_v2");
        SQLSYM(bind_text, "sqlite3_bind_text");
        SQLSYM(step, "sqlite3_step");
        SQLSYM(column_int64, "sqlite3_column_int64");
        SQLSYM(column_double, "sqlite3_column_double");
        SQLSYM(column_text, "sqlite3_column_text");
        SQLSYM(column_blob, "sqlite3_column_blob");
        SQLSYM(column_bytes, "sqlite3_column_bytes");
        SQLSYM(finalize, "sqlite3_finalize");
        SQLSYM(errmsg, "sqlite3_errmsg");
#undef SQLSYM
        return true;
    }
};

static constexpr int SQLITE_OK_ = 0;
static constexpr int SQLITE_ROW_ = 100;
static constexpr int SQLITE_OPEN_READONLY_ = 0x00000001;
static auto SQLITE_TRANSIENT_ = reinterpret_cast<void(__cdecl*)(void*)>(-1);

struct WaveformCache
{
    static constexpr int BAR_COUNT = 149;

    std::string filepath;
    std::string title;
    std::array<uint8_t, BAR_COUNT> bars{};
    bool valid = false;

    void clear()
    {
        filepath.clear();
        title.clear();
        bars.fill(0);
        valid = false;
    }

    uint8_t maxBar(int index) const
    {
        if (index < 0 || index >= BAR_COUNT)
            return 0;
        return bars[index];
    }
};


static bool loadVdjCacheWaveform(const std::string& trackPath, WaveformCache& cache)
{
    const ULONGLONG started = GetTickCount64();

    WinSqlite sql;
    if (!sql.load())
    {
        logLine("V38b CACHE: winsqlite3.dll unavailable");
        return false;
    }

    std::filesystem::path track = std::filesystem::u8path(trackPath);
    std::filesystem::path root = track.root_path();
    std::filesystem::path db = root / "VirtualDJ" / "Cache" / "cache.db";
    std::filesystem::path wal = db;
    wal += "-wal";

    std::error_code ec;
    if (!std::filesystem::exists(db, ec))
    {
        logLine("V38b CACHE: cache.db not found [" + db.u8string() + "]");
        return false;
    }

    // Keep the proven robust strategy: snapshot DB + WAL while VDJ is running.
    // Native copy is much cheaper than starting Python for every track.
    std::filesystem::path td = std::filesystem::temp_directory_path(ec) / "S4VDJ_v38";
    std::filesystem::create_directories(td, ec);
    std::filesystem::path cp = td / "cache.db";
    std::filesystem::path cpwal = td / "cache.db-wal";

    ec.clear();
    std::filesystem::copy_file(db, cp,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        logLine("V38b CACHE: cache.db copy failed");
        return false;
    }

    ec.clear();
    if (std::filesystem::exists(wal, ec))
    {
        ec.clear();
        std::filesystem::copy_file(wal, cpwal,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            logLine("V38b CACHE: WAL copy failed");
            return false;
        }
    }
    else
    {
        std::filesystem::remove(cpwal, ec);
    }

    sqlite3* conn = nullptr;
    const std::string cpUtf8 = cp.u8string();
    if (sql.open_v2(cpUtf8.c_str(), &conn, SQLITE_OPEN_READONLY_, nullptr) != SQLITE_OK_ || !conn)
    {
        logLine("V38b CACHE: sqlite open failed");
        if (conn) sql.close(conn);
        return false;
    }

    const char* q =
        "select id,filepath,filename,valuesPerSecond,length(waveform),waveform "
        "from waveforms "
        "where filename=? collate nocase and type=1 and version=4 "
        "order by length(waveform) desc,id desc";

    sqlite3_stmt* st = nullptr;
    if (sql.prepare_v2(conn, q, -1, &st, nullptr) != SQLITE_OK_ || !st)
    {
        logLine(std::string("V38b CACHE: prepare failed: ") + sql.errmsg(conn));
        sql.close(conn);
        return false;
    }

    const std::string filename = track.filename().u8string();
    sql.bind_text(st, 1, filename.c_str(), -1, SQLITE_TRANSIENT_);

    const std::filesystem::path wantedDir = track.parent_path().lexically_normal();
    bool found = false;
    long long bestId = 0;
    double bestVps = 0.0;
    int bestBytes = 0;
    std::vector<uint8_t> bestBlob;
    bool haveExactDir = false;

    while (sql.step(st) == SQLITE_ROW_)
    {
        const auto* fp = sql.column_text(st, 1);
        std::filesystem::path rowDir =
            fp ? std::filesystem::u8path(reinterpret_cast<const char*>(fp)).lexically_normal()
               : std::filesystem::path();

        bool exactDir = false;
        if (!rowDir.empty())
        {
            std::wstring a = rowDir.wstring();
            std::wstring b = wantedDir.wstring();
            exactDir = (_wcsicmp(a.c_str(), b.c_str()) == 0);
        }

        // Rows are already highest-resolution first. If duplicate filenames
        // exist, an exact directory match takes precedence.
        if (found && haveExactDir)
            continue;
        if (found && !exactDir)
            continue;

        const void* blob = sql.column_blob(st, 5);
        const int n = sql.column_bytes(st, 5);
        if (!blob || n < 4)
            continue;

        bestId = sql.column_int64(st, 0);
        bestVps = sql.column_double(st, 3);
        bestBytes = n;
        bestBlob.assign(static_cast<const uint8_t*>(blob),
                        static_cast<const uint8_t*>(blob) + n);
        found = true;
        haveExactDir = exactDir;
    }

    sql.finalize(st);
    sql.close(conn);

    if (!found)
    {
        logLine("V38b CACHE: waveform row not found [" + filename + "]");
        return false;
    }

    size_t records = bestBlob.size() / 4;
    while (records > 0)
    {
        const size_t i = (records - 1) * 4;
        if (bestBlob[i] == 0 && bestBlob[i+1] == 0 &&
            bestBlob[i+2] == 0 && bestBlob[i+3] == 0)
            --records;
        else
            break;
    }
    if (records == 0)
        return false;

    std::array<double, WaveformCache::BAR_COUNT> raw{};
    for (int j = 0; j < WaveformCache::BAR_COUNT; ++j)
    {
        const double x = j * (records - 1) /
            static_cast<double>(WaveformCache::BAR_COUNT - 1);
        const size_t i = static_cast<size_t>(x);
        const double f = x - static_cast<double>(i);
        const double a = bestBlob[i * 4 + 3];
        const double b = (i + 1 < records) ? bestBlob[(i + 1) * 4 + 3] : a;
        raw[j] = a * (1.0 - f) + b * f;
    }

    std::vector<double> nz;
    nz.reserve(WaveformCache::BAR_COUNT);
    for (double v : raw)
        if (v > 0.0) nz.push_back(v);
    if (nz.empty())
        return false;
    std::sort(nz.begin(), nz.end());
    const size_t p95 = static_cast<size_t>((nz.size() - 1) * 0.95);
    const double ref = (std::max)(1.0, nz[p95]);

    for (int j = 0; j < WaveformCache::BAR_COUNT; ++j)
    {
        int v = static_cast<int>(std::lround(raw[j] / ref * 38.0));
        v = (std::max)(0, (std::min)(38, v));
        cache.bars[j] = static_cast<uint8_t>(v);
    }
    cache.valid = true;

    std::ostringstream ss;
    ss << "V38b CACHE WAVE: deck data ready timeMs="
       << (GetTickCount64() - started)
       << " [id=" << bestId
       << " blobBytes=" << bestBytes
       << " records=" << records
       << " vps=" << bestVps
       << " ref=" << ref << "]";
    logLine(ss.str());
    return true;
}

static RGB waveformColorForDeck(int deck)
{
    switch (deck)
    {
        case 1: return RGB{0, 130, 205};   // A: blue
        case 2: return RGB{220, 55, 50};   // B: red
        case 3: return RGB{30, 190, 60};   // C: green
        case 4: return RGB{225, 150, 35};  // D: orange
        default: return RGB{105, 115, 130};
    }
}

static void drawWaveform(
    Frame& f,
    const WaveformCache& cache,
    RGB color
)
{
    if (!cache.valid)
        return;

    constexpr int X0 = 11;
    constexpr int Y0 = 135;
    constexpr int INNER_W = 298;
    constexpr int INNER_H = 82;
    constexpr int CENTER_Y = Y0 + INNER_H / 2;

    for (int x = 0; x < INNER_W; ++x)
    {
        const int index =
            (x * WaveformCache::BAR_COUNT) / INNER_W;

        const int halfHeight =
            static_cast<int>(cache.maxBar(index));

        if (halfHeight <= 0)
            continue;

        f.rect(
            X0 + x,
            CENTER_Y - halfHeight,
            1,
            halfHeight * 2 + 1,
            color
        );
    }
}

// ============================================================
// Exact Python v3 layout + precomputed full-track waveform
// ============================================================

static Frame makeLiveScreen(
    int deck,
    const DeckData& data,
    int screen,
    const WaveformCache& waveform
)
{
    Frame f;

    const std::string title =
        data.title.empty()
            ? "NO TRACK"
            : data.title;

    const std::string key =
        data.key.empty()
            ? "--"
            : data.key;

    const RGB accent = CYAN;
    const RGB loopColor = GREEN;

    // TITLE: same geometry as Python v3
    f.rect(
        8, 5, 304, 35,
        GRID
    );

    f.rect(
        11, 8, 298, 29,
        DARK
    );

    f.text(
        title.substr(
            0,
            (std::min)(
                size_t(31),
                title.size()
            )
        ),
        18, 14, 2,
        WHITE
    );

    // BPM | KEY
    f.rect(
        8, 44, 147, 40,
        GRID
    );

    f.rect(
        11, 47, 141, 34,
        DARK
    );

    f.rect(
        165, 44, 147, 40,
        GRID
    );

    f.rect(
        168, 47, 141, 34,
        DARK
    );

    f.text(
        fmtBpm(data.bpm),
        28, 53, 3,
        WHITE
    );

    f.text(
        key.substr(
            0,
            (std::min)(
                size_t(4),
                key.size()
            )
        ),
        208, 52, 3,
        WHITE
    );

    // TIME LEFT | LOOP
    f.rect(
        8, 88, 147, 40,
        GRID
    );

    f.rect(
        11, 91, 141, 34,
        DARK
    );

    f.rect(
        165, 88, 147, 40,
        GRID
    );

    f.rect(
        168, 91, 141, 34,
        DARK
    );

    f.text(
        "-" + fmtSeconds(data.remain),
        17, 94, 3,
        WHITE
    );

    if (data.loop > 0.0)
    {
        std::string loopText;

        if (std::abs(
                data.loop -
                std::round(data.loop)
            ) < 0.0001)
        {
            loopText =
                std::to_string(
                    static_cast<int>(
                        std::round(data.loop)
                    )
                );
        }
        else
        {
            char out[32]{};

            sprintf_s(
                out,
                "%g",
                data.loop
            );

            loopText = out;
        }

        f.text(
            loopText,
            220, 94, 3,
            loopColor
        );
    }
    else
    {
        f.text(
            "--",
            224, 94, 3,
            GREY
        );
    }

    // WAVEFORM AREA
    f.rect(
        8, 132, 304, 88,
        GRID
    );

    f.rect(
        11, 135, 298, 82,
        DARK
    );

    // V36: VDJ uses a consistent waveform color per deck.
    // Position marker remains separate and is drawn afterwards.
    drawWaveform(
        f,
        waveform,
        waveformColorForDeck(deck)
    );

    // Current position marker.
    const double total =
        (std::max)(
            0.0,
            data.elapsed + data.remain
        );

    double ratio = 0.0;

    if (total > 0.0)
    {
        ratio =
            data.elapsed / total;
    }

    ratio =
        (std::max)(
            0.0,
            (std::min)(
                1.0,
                ratio
            )
        );

    const int playX =
        15 +
        static_cast<int>(
            ratio * 290.0
        );

    f.rect(
        playX, 138, 2, 76,
        WHITE
    );

    return f;
}

// ============================================================
// Native VDJ plugin
//
// V27 uses the deck file path to build the complete waveform before playback.
// OnGetSongBuffer() is retained as a transparent DSP pass-through only.
// No waveform processing or file I/O occurs in the audio callback.

// ============================================================

class S4VDJDisplay final
    : public IVdjPluginBufferDsp8
{
public:
    HRESULT VDJ_API OnLoad() override
    {
        logLine("");
        logLine(
            "============================================================"
        );
        logLine(
            "S4 MK3 NATIVE VDJ DISPLAY v38b BEGIN"
        );
        logLine(
            "V38b: native VirtualDJ cache B3 waveform"
        );
        logLine(
            "Native C++ layout/data, libusb transport"
        );
        logLine(
            "Display 1 = A/C, Display 2 = B/D"
        );
        logLine(
            "V38b: native SQLite + plugin-local libusb; no Python runtime"
        );
        logLine(
            "============================================================"
        );

        if (cb)
        {
            {
                std::ostringstream ss;
                ss << "INSTANCE: OnLoad this=0x"
                   << std::uppercase << std::hex
                   << reinterpret_cast<uintptr_t>(this)
                   << std::dec
                   << " pluginDeck="
                   << pluginDeck_;
                logLine(ss.str());
            }

            char version[128]{};

            HRESULT hr =
                GetStringInfo(
                    "get_version",
                    version,
                    sizeof(version)
                );

            if (hr == S_OK)
            {
                logLine(
                    std::string(
                        "VDJ: version="
                    ) + version
                );
            }
        }

        // Some VDJ plugin hosts use only Basic for General plugins.
        // Start here as well; start() is protected against duplicate calls.
        start();

        return S_OK;
    }

    short* VDJ_API OnGetSongBuffer(
        int songPos,
        int nb
    ) override
    {
        // V27 deliberately does no waveform processing here. This callback
        // remains a transparent pass-through so audio playback is not held
        // up by file I/O, waveform analysis, logging or deck queries.
        short* buffer = nullptr;

        const HRESULT hr =
            GetSongBuffer(
                songPos,
                nb,
                &buffer
            );

        if (FAILED(hr) || !buffer)
        {
            if (bufferErrorCount_ < 5)
            {
                std::ostringstream ss;
                ss << "BUFFER: GetSongBuffer FAILED songPos="
                   << songPos
                   << " nb="
                   << nb
                   << " hr="
                   << errHex(static_cast<int>(hr));
                logLine(ss.str());
            }

            ++bufferErrorCount_;
            return nullptr;
        }

        return buffer;
    }

    HRESULT VDJ_API OnGetPluginInfo(
        TVdjPluginInfo8* info
    ) override
    {
        if (!info)
            return E_FAIL;

        double pd = -999;
        HRESULT pdhr = GetInfo("get_plugindeck", &pd);
        if (pdhr == S_OK)
            pluginDeck_ = static_cast<int>(pd);
        else
            pluginDeck_ = -999;

        {
            std::ostringstream ss;
            ss << "INSTANCE: OnGetPluginInfo this=0x"
               << std::uppercase << std::hex
               << reinterpret_cast<uintptr_t>(this)
               << std::dec
               << " pluginDeck="
               << pluginDeck_;
            logLine(ss.str());
        }

        info->PluginName =
            "S4 MK3 Display";

        info->Author =
            "S4 VDJ Display / Custom";

        info->Description =
            "Native Traktor Kontrol S4 MK3 display driver for VirtualDJ";

        info->Version =
            "1.0.1-v38b-local-libusb";

        info->Bitmap = nullptr;
        info->Flags = 0;

        return S_OK;
    }

    HRESULT VDJ_API OnStart() override
    {
        logLine(
            "PLUGIN: OnStart"
        );

        return start()
            ? S_OK
            : E_FAIL;
    }

    HRESULT VDJ_API OnStop() override
    {
        logLine(
            "PLUGIN: OnStop"
        );

        stop();

        return S_OK;
    }

    ULONG VDJ_API Release() override
    {
        logLine(
            "PLUGIN: Release"
        );

        stop();

        delete this;

        return S_OK;
    }

private:
    bool start()
    {
        bool expected = false;

        if (!running_.compare_exchange_strong(
                expected,
                true))
        {
            return true;
        }

        if (!usb_.open())
        {
            running_ = false;

            logLine(
                "PLUGIN: USB OPEN FAILED"
            );

            return false;
        }

        // Immediate synchronous first frame.
        updateOnce();

        worker_ =
            std::thread(
                &S4VDJDisplay::workerLoop,
                this
            );

        logLine(
            "PLUGIN: display worker started"
        );

        return true;
    }

    void stop()
    {
        running_ = false;

        if (worker_.joinable())
            worker_.join();

        usb_.close();
    }

    void workerLoop()
    {
        while (running_)
        {
            updateOnce();

            std::this_thread::sleep_for(
                std::chrono::milliseconds(200)
            );
        }
    }

    void updateWaveform(
        int deck,
        const DeckData& data,
        WaveformCache& cache,
        std::string& trackedPath
    )
    {
        if (data.filepath.empty())
        {
            if (!trackedPath.empty())
            {
                trackedPath.clear();
                cache.clear();
            }
            return;
        }

        if (trackedPath == data.filepath)
            return;

        trackedPath = data.filepath;
        cache.clear();
        cache.filepath = data.filepath;
        cache.title = data.title;

        const bool ok = loadVdjCacheWaveform(data.filepath, cache);

        std::ostringstream ss;
        ss << "WAVE CACHE: deck=" << deck
           << " source=VDJ-CACHE-B3"
           << " status=" << (ok ? "OK" : "FAILED")
           << " file=[" << data.filepath << "]";
        logLine(ss.str());
    }

    void updateOnce()
    {
        const int leftDeck =
            getActiveDeck(
                this,
                "get_leftdeck",
                1
            );

        const int rightDeck =
            getActiveDeck(
                this,
                "get_rightdeck",
                2
            );

        const int left =
            (leftDeck == 1 ||
             leftDeck == 3)
                ? leftDeck
                : 1;

        const int right =
            (rightDeck == 2 ||
             rightDeck == 4)
                ? rightDeck
                : 2;

        const DeckData leftData =
            queryDeck(
                this,
                left
            );

        const DeckData rightData =
            queryDeck(
                this,
                right
            );

        // V29: load the complete RMS/peak waveform directly from each deck's file.
        // No waveform work is performed in the audio callback.
        updateWaveform(
            left,
            leftData,
            leftWaveform_,
            leftTrackedPath_
        );

        updateWaveform(
            right,
            rightData,
            rightWaveform_,
            rightTrackedPath_
        );

        // Log a small amount of actual VDJ data every 10 cycles.
        ++logCounter_;

        if (logCounter_ >= 10)
        {
            logCounter_ = 0;

            std::ostringstream ss;

            ss << "VDJ: LEFT deck="
               << left
               << " title=["
               << leftData.title
               << "] bpm="
               << leftData.bpm
               << " key=["
               << leftData.key
               << "] remain="
               << leftData.remain
               << " loop="
               << leftData.loop

               << " | RIGHT deck="
               << right
               << " title=["
               << rightData.title
               << "] bpm="
               << rightData.bpm
               << " key=["
               << rightData.key
               << "] remain="
               << rightData.remain
               << " loop="
               << rightData.loop;

            logLine(ss.str());
        }

        const Frame leftFrame =
            makeLiveScreen(
                left,
                leftData,
                0,
                leftWaveform_
            );

        const Frame rightFrame =
            makeLiveScreen(
                right,
                rightData,
                1,
                rightWaveform_
            );

        usb_.sendScreen(
            0,
            leftFrame.p
        );

        usb_.sendScreen(
            1,
            rightFrame.p
        );
    }

private:
    std::atomic<bool> running_{false};
    std::thread worker_;
    LibUsbS4 usb_;
    WaveformCache leftWaveform_;
    WaveformCache rightWaveform_;

    std::string leftTrackedPath_;
    std::string rightTrackedPath_;
    // Captured in OnGetPluginInfo(), which is the supported phase for
    // get_plugindeck. Do not query get_plugindeck from the audio callback.
    int pluginDeck_ = -999;

    int logCounter_ = 0;

    uint64_t bufferErrorCount_ = 0;
};

// ============================================================
// VDJ class factory
// ============================================================

extern "C" VDJ_EXPORT HRESULT VDJ_API DllGetClassObject(
    const GUID& rclsid,
    const GUID& riid,
    void** ppObject
)
{
    if (!ppObject)
        return E_POINTER;

    *ppObject = nullptr;

    if (std::memcmp(
            &rclsid,
            &CLSID_VdjPlugin8,
            sizeof(GUID)
        ) != 0)
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* plugin =
        new S4VDJDisplay();

    if (std::memcmp(
            &riid,
            &IID_IVdjPluginBuffer8,
            sizeof(GUID)
        ) == 0)
    {
        *ppObject =
            static_cast<
                IVdjPluginBufferDsp8*
            >(plugin);

        return S_OK;
    }

    delete plugin;

    return E_NOINTERFACE;
}
