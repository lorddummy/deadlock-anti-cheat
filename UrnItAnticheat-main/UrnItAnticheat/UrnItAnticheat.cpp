
#define BUILD_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Lmcons.h>
#include <wtsapi32.h>
#include <winhttp.h>

#include <iostream>
#include <vector>
#include <chrono>
#include <string>
#include <thread>
#include <codecvt>
#include <fstream>
#include <sstream>
#include <algorithm>

#include <cpu_info/cpuinfo_x86.h>

#pragma comment(lib, "Wtsapi32.lib")
#pragma comment(lib, "winhttp.lib")

// Tunable constants (single place to adjust intervals and limits)
namespace Config {
  const float TASK_SCAN_INTERVAL_SEC    = 3.f;
  const float SCREENSHOT_INTERVAL_SEC  = 5.f;
  const float CPU_LOG_INTERVAL_SEC     = 0.5f;
  const float KEYLOG_INTERVAL_SEC      = 0.05f;
  const DWORD LOOP_SLEEP_MS            = 20;
  const size_t WEBHOOK_BATCH_SIZE      = 10;
  const DWORD WEBHOOK_RATE_LIMIT_MS    = 500;
  const DWORD UPLOAD_TIMEOUT_MS        = 30000;
  const size_t MAX_UPLOAD_FILE_BYTES   = 25u * 1024 * 1024;
  const size_t MAX_SCREENSHOT_UPLOAD_BYTES = 8u * 1024 * 1024;  // skip huge BMPs in upload (still on disk)
  const bool   AUTO_UPLOAD_ON_GAME_EXIT = true;  // when true, session ends and uploads automatically when Deadlock process exits; F12 still works as manual end
}

// Format: category name ("cheats" or "performance") then one or more process names (e.g. "cheat.exe").
// Matching is case-insensitive. Add known cheat/overlay exe names after "cheats", tools after "performance".
std::vector<std::string> all_programs_list{
  "cheats",
  // "example_cheat.exe",
  "performance",
  // "msi afterburner.exe",
};

std::vector<std::string> found_cheating_programs;
std::vector<std::string> found_performance_programs;

// Returns elapsed time in seconds (matches Config::*_SEC intervals).
inline float GetTimeDifference(std::chrono::high_resolution_clock::time_point time) {
  return std::chrono::duration<float, std::ratio<1>>(
    std::chrono::high_resolution_clock::now() - time).count();
}

inline std::string ConvertWideToString(std::wstring w_str) {
  std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;
  return converter.to_bytes(w_str);
}; //ConvertWideToString

// Keylogger: human-readable name for a virtual key (only keys we log).
inline std::string VkToKeyName(int vk) {
  if (vk >= VK_A && vk <= VK_Z) return std::string(1, (char)('A' + (vk - VK_A)));
  if (vk >= VK_0 && vk <= VK_9) return std::string(1, (char)('0' + (vk - VK_0)));
  switch (vk) {
    case VK_SPACE:   return "Space";
    case VK_RETURN:  return "Enter";
    case VK_BACK:    return "Backspace";
    case VK_TAB:     return "Tab";
    case VK_ESCAPE:  return "Escape";
    case VK_SHIFT:   return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU:    return "Alt";
    case VK_LBUTTON: return "LMB";
    case VK_RBUTTON: return "RMB";
    case VK_MBUTTON: return "MMB";
    default:         return "VK" + std::to_string(vk);
  }
}

// Anticheat: only log input when game window is focused (evidence is in-game only, less privacy surface).
// Set to empty string to log regardless of focus. Partial match so "Deadlock - Main Menu" or "Deadlock | 60 FPS" works.
static const wchar_t* GAME_WINDOW_TITLE = L"Deadlock";

static BOOL CALLBACK EnumFindWindowByTitleProc(HWND hwnd, LPARAM lParam) {
  HWND* out = (HWND*)lParam;
  wchar_t title[256] = {};
  if (GetWindowTextW(hwnd, title, 256) > 0 && wcsstr(title, GAME_WINDOW_TITLE)) {
    *out = hwnd;
    return FALSE;  // stop enumeration
  }
  return TRUE;
}

// Finds game window: exact title match first, then any window whose title contains GAME_WINDOW_TITLE.
static HWND FindGameWindow() {
  if (!GAME_WINDOW_TITLE || GAME_WINDOW_TITLE[0] == L'\0') return NULL;
  HWND h = FindWindowW(NULL, GAME_WINDOW_TITLE);
  if (h) return h;
  HWND found = NULL;
  EnumWindows(EnumFindWindowByTitleProc, (LPARAM)&found);
  return found;
}

inline bool IsGameWindowFocused() {
  HWND game = FindGameWindow();
  if (!game) return true;  // game not running: still log (e.g. launcher) or set title to match launcher
  return GetForegroundWindow() == game;
}

// Accurate Windows version (GetVersionEx is deprecated and wrong on Win10/11). Returns e.g. "10 22H2 (build 19045)".
static std::string GetWindowsVersionFromRegistry() {
  HKEY key = NULL;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &key) != ERROR_SUCCESS)
    return "unknown";
  char buf[256] = {};
  DWORD sz = sizeof(buf) - 1;
  std::string build = "unknown", display = "";
  if (RegQueryValueExA(key, "CurrentBuild", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS) build = buf;
  sz = sizeof(buf) - 1; buf[0] = '\0';
  if (RegQueryValueExA(key, "DisplayVersion", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS) display = buf;
  RegCloseKey(key);
  if (display.empty()) return "build " + build;
  return "build " + build + " (" + display + ")";
}

// --- Discord webhook upload (no server; players don't submit files manually) ---
// Webhook URL is read from <exe_dir>/webhook.txt (first line). Create in Discord: Channel → Integrations → Webhooks.

struct FilePart {
  std::string filename;
  std::vector<char> data;
};

static bool ReadWebhookUrl(const std::string& exe_dir, std::string& out_url) {
  std::string path = exe_dir + "\\webhook.txt";
  std::ifstream f(path);
  if (!f) return false;
  out_url.clear();
  if (!std::getline(f, out_url)) return false;
  while (!out_url.empty() && (out_url.back() == '\r' || out_url.back() == '\n')) out_url.pop_back();
  return out_url.size() > 20 && out_url.find("discord.com") != std::string::npos;
}

static bool ReadFileToVector(const std::string& path, std::vector<char>& out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  auto sz = f.tellg();
  if (sz <= 0 || sz > (std::streamsize)Config::MAX_UPLOAD_FILE_BYTES) return false;
  out.resize((size_t)sz);
  f.seekg(0);
  return (bool)f.read(out.data(), sz);
}

static void AppendMultipartField(std::vector<char>& body, const char* boundary,
  const char* name, const void* data, size_t len) {
  std::ostringstream head;
  head << "\r\n--" << boundary << "\r\n"
       << "Content-Disposition: form-data; name=\"" << name << "\"\r\n\r\n";
  std::string h = head.str();
  body.insert(body.end(), h.begin(), h.end());
  const char* p = (const char*)data;
  body.insert(body.end(), p, p + len);
}

static void AppendMultipartFile(std::vector<char>& body, const char* boundary,
  const char* name, const std::string& filename, const void* data, size_t len, const char* content_type) {
  std::ostringstream head;
  head << "\r\n--" << boundary << "\r\n"
       << "Content-Disposition: form-data; name=\"" << name << "\"; filename=\"" << filename << "\"\r\n"
       << "Content-Type: " << content_type << "\r\n\r\n";
  std::string h = head.str();
  body.insert(body.end(), h.begin(), h.end());
  const char* p = (const char*)data;
  body.insert(body.end(), p, p + len);
}

static bool SendWebhookBatch(const std::string& webhook_url, const std::string& message_content,
  const std::vector<FilePart>& files) {
  std::string host, path;
  {
    size_t start = 0;
    if (webhook_url.find("https://") == 0) start = 8;
    else if (webhook_url.find("http://") == 0) start = 7;
    size_t slash = webhook_url.find('/', start);
    if (slash == std::string::npos) return false;
    host = webhook_url.substr(start, slash - start);
    path = webhook_url.substr(slash);
  }
  const char* boundary = "----UrnItWebhookBoundary";
  std::vector<char> body;
  std::string escaped;
  for (char c : message_content) {
    if (c == '"') escaped += "\\\"";
    else if (c == '\\') escaped += "\\\\";
    else escaped += c;
  }
  std::string payload_json = "{\"content\":\"" + escaped + "\"}";
  AppendMultipartField(body, boundary, "payload_json", payload_json.data(), payload_json.size());
  for (size_t i = 0; i < files.size(); i++) {
    std::string name = "file" + std::to_string(i + 1);
    const char* ct = (files[i].filename.size() >= 4 && files[i].filename.compare(files[i].filename.size() - 4, 4, ".bmp") == 0)
      ? "image/bmp" : "application/octet-stream";
    AppendMultipartFile(body, boundary, name.c_str(), files[i].filename, files[i].data.data(), files[i].data.size(), ct);
  }
  {
    std::string tail = "\r\n--";
    tail += boundary;
    tail += "--\r\n";
    body.insert(body.end(), tail.begin(), tail.end());
  }
  std::wstring whost(host.begin(), host.end());
  std::wstring wpath(path.begin(), path.end());
  HINTERNET session = WinHttpOpen(L"UrnItAnticheat/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, Config::UPLOAD_TIMEOUT_MS, Config::UPLOAD_TIMEOUT_MS, Config::UPLOAD_TIMEOUT_MS, Config::UPLOAD_TIMEOUT_MS);
  HINTERNET connect = WinHttpConnect(session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!connect) { WinHttpCloseHandle(session); return false; }
  HINTERNET request = WinHttpOpenRequest(connect, L"POST", wpath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }
  std::wstring ctheader = L"Content-Type: multipart/form-data; boundary=";
  std::wstring wboundary(boundary, boundary + strlen(boundary));
  ctheader += wboundary;
  WinHttpAddRequestHeaders(request, ctheader.c_str(), (DWORD)ctheader.size(), WINHTTP_ADDREQ_FLAG_ADD);
  BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
  if (!sent) { WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }
  BOOL recv = WinHttpReceiveResponse(request, NULL);
  if (!recv) { WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }
  DWORD status = 0, len = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &len, NULL);
  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return (status >= 200 && status < 300);
}

static std::string ReadPlayerId(const std::string& exe_dir) {
  std::string path = exe_dir + "\\player_id.txt";
  std::ifstream f(path);
  if (!f) return "";
  std::string line;
  if (!std::getline(f, line)) return "";
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  return line;
}

// Returns true if all batches uploaded successfully. Sends a final status message to Discord.
static bool UploadSessionToDiscord(const std::string& exe_dir, const std::string& session_dir) {
  std::string webhook_url;
  if (!ReadWebhookUrl(exe_dir, webhook_url)) return false;
  std::string player_label = ReadPlayerId(exe_dir);
  std::vector<FilePart> all;
  const std::string names[] = { "REPORT.TXT", "KEY_LOG.TXT", "TASK.TXT" };
  for (const auto& n : names) {
    FilePart fp;
    fp.filename = n;
    if (ReadFileToVector(session_dir + n, fp.data)) all.push_back(std::move(fp));
  }
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((session_dir + "*.bmp").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        FilePart fp;
        fp.filename = fd.cFileName;
        if (ReadFileToVector(session_dir + fd.cFileName, fp.data)) {
          if (fp.data.size() <= Config::MAX_SCREENSHOT_UPLOAD_BYTES)
            all.push_back(std::move(fp));
        }
      }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  bool all_ok = true;
  for (size_t i = 0; i < all.size(); i += Config::WEBHOOK_BATCH_SIZE) {
    if (i > 0) Sleep(Config::WEBHOOK_RATE_LIMIT_MS);
    size_t end = (std::min)(i + Config::WEBHOOK_BATCH_SIZE, all.size());
    std::vector<FilePart> batch(all.begin() + i, all.begin() + end);
    std::string msg = "UrnIt Anticheat session (part " + std::to_string((i / Config::WEBHOOK_BATCH_SIZE) + 1) + ")";
    if (!player_label.empty()) msg = "Player: " + player_label + " – " + msg;
    bool ok = SendWebhookBatch(webhook_url, msg, batch);
    if (!ok) { Sleep(1000); ok = SendWebhookBatch(webhook_url, msg, batch); }
    if (!ok) all_ok = false;
  }
  std::string status_msg = all_ok ? "UrnIt upload: completed" : "UrnIt upload: one or more batches failed";
  if (!player_label.empty()) status_msg = "Player: " + player_label + " – " + status_msg;
  SendWebhookBatch(webhook_url, status_msg, {});  // no files; just the status line
  return all_ok;
}

struct Program_Flags {
  enum CHEATING_POTENTIAL : uint8_t {
    VERY_UNLIKELY = 0,
    UNLIKELY,
    MODERATE,
    LIKELY,
    VERY_LIKELY
  }; //CHEATING_POTENTIAL ENUM

  uint8_t cheating_potential    : 3;
  uint8_t deadlock_repeat_flag  : 1;
}; //program_flags

time_t tt;
std::string local_time;
std::string file_time;

void UpdateTime() {
  time(&tt);
  local_time = asctime(localtime(&tt));
  local_time.erase(local_time.end()-1);

  file_time = local_time;
  for (auto& c : file_time) {
    if (c == ':') c = ';';
  }; //For every char
}; //UpdateTime

inline int64_t FileTimeToInt64(FILETIME& ft) {
  ULARGE_INTEGER foo;
  foo.LowPart = ft.dwLowDateTime;
  foo.HighPart = ft.dwHighDateTime;
  return (foo.QuadPart);
}; //FileTimeToInt64

int main()
{
  Program_Flags prgrm_flags{};

  std::string task_str;
  std::string keylog_str;
  std::string report_str;
  std::string urn_acii;

  HANDLE task_log_file   = INVALID_HANDLE_VALUE;
  HANDLE report_log_file = INVALID_HANDLE_VALUE;
  HANDLE key_log_file    = INVALID_HANDLE_VALUE;

  std::string file_directory;
  std::string exe_directory;  // folder containing .exe (for webhook.txt)

  uint8_t cpu_cores;

  InitializationAndLogFileCreation: {
    char dir_path[MAX_PATH];
    GetModuleFileNameA(NULL, dir_path, MAX_PATH);
    auto dir_str = std::string(dir_path);
    file_directory = dir_str.substr(0, dir_str.find_last_of('\\'));
    exe_directory = file_directory;

    UpdateTime();

    file_directory += '\\';
    file_directory += file_time;
    file_directory += '\\';
    if (!CreateDirectoryA(file_directory.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) goto ReportConclusion;
    task_log_file   = CreateFileA((file_directory + "TASK.TXT").c_str(), GENERIC_WRITE | GENERIC_READ, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    report_log_file = CreateFileA((file_directory + "REPORT.TXT").c_str(), GENERIC_WRITE | GENERIC_READ, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    key_log_file    = CreateFileA((file_directory + "KEY_LOG.TXT").c_str(), GENERIC_WRITE | GENERIC_READ, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (task_log_file == INVALID_HANDLE_VALUE || report_log_file == INVALID_HANDLE_VALUE || key_log_file == INVALID_HANDLE_VALUE)
      goto ReportConclusion;

    urn_acii = R"(
                                 _   _                               
                     _ x_ ._____.._t_                          
                    + ,,AJNNNXbqbLG._ u                        
                  :_,EHNNNNNNNNNXro7OA___                      
                 . GHHNNNNNNNNNNELeA+..,_q                     
                z_eHHHMMMLCGMSBHHKQ1QKHD_n                     
                 _HHIHCEMNMIBACCCFMVX5y__q                     
          ______t.  ,ACCCCHHNMSVXYXXXQHHX y.                   
        _t         BBABBAABFHGDAGMMEBVadZ-_.                   
        _p       __CCCCCDHHINMMMMGAPenaBFA_                    
          _+ _    .AAAAAAABOCCCCVCCCCREPKC,     :              
           t_      CICCCCCCCGOCDGCCCPCCLLE.  _  .              
            :_.__  CCCCCCCCSCCCCCHCFCCCKNK_. ____ k            
            p     _BJCCCCCCCGCCCCMCNCCCMNE. +__ _,x            
           _,_.._+.,NCCCCCDGCCCCCCFCCCGRMC,    _r              
             qfjihxxCDDDDDDIDDDDCCDPBCH2KE_     ._             
          _ :rvvxvq_.OCCCCDCCDCCCDCCDCHcH+ ttttty_             
         _:___  __ ._,CDCCECCOFCKCCCCHJSJ_ _ ___._j            
           :          -CKCJICHCCCCGKJJ0A_        l.            
          ___     ._____BOCCCCCDCCCBH5K,__ . __ ..             
         _          _   .ACCCCCCCCHECHt  D. ,_ _+              
         ;__        _._.__,KHAAAAJ97JK2._ABMI__;               
         __ . vwvww __+____.8ALJC__ _.HWE89j___                
           t        __.BHAWWB,_    _  ,98A. l.                 
          _+.      _f .DW__            _h__+_                  
              _     ,._JF_we9SRRRRRRSWdu..___,                 
                    _+-SBCASSSSSSSSSSSSSSSSx, ,                
                    _v. __             __  .i_                 
                              _______                          

                                                               
    )"; //urn_acii


    report_str += urn_acii;
    task_str += urn_acii;
    keylog_str += urn_acii;

    report_str += "\n";
    task_str += "\n";
    keylog_str += "\n";

    report_str += "URN YOUR WINS - EAT THE APPLE\n";
    task_str += "URN YOUR WINS - EAT THE APPLE\n";
    keylog_str += "URN YOUR WINS - EAT THE APPLE\n";

    report_str += "PERSONNEL LOGGING BEGIN\n";
    keylog_str += "Format: +<ms_since_session_start> KeyDown|KeyUp <key> (for anticheat timing analysis)\n";

    WriteFile(report_log_file, report_str.c_str(), report_str.size(), NULL, NULL);
    WriteFile(key_log_file, keylog_str.c_str(), keylog_str.size(), NULL, NULL);
    WriteFile(task_log_file, task_str.c_str(), task_str.size(), NULL, NULL);
  }; //Library Initialization

  GetDiscordInformation: {
    report_str = "\n";
    report_str += "\n";
  }; //GetDiscordInformation

  GetSteamInformation: {
    report_str = "\n";
    report_str += "\n";
  }; //GetSteamInformation

  GetOSInformation: {
    report_str = "\n";

    CHAR username[256] = {};
    DWORD username_len = 256;
    GetUserNameA(username, &username_len);

    report_str += "Client Version: UrnItAnticheat 1.0\n";
    std::string player_id = ReadPlayerId(exe_directory);
    if (!player_id.empty()) report_str += "Player ID: " + player_id + "\n";
    report_str += "Operating System Information: \n";
    report_str += "User Name: " + std::string(username) + "\n";
    report_str += "Windows: " + GetWindowsVersionFromRegistry() + "\n";
    report_str += "\n";

    WriteFile(report_log_file, report_str.c_str(), report_str.size(), NULL, NULL);
  }; //GetOSInformation

  GetCPUInformation: {
    report_str = "\n";

    #ifdef CPU_FEATURES_ARCH_X86_64
      auto cpu_info              = cpu_features::GetX86Info();
      auto cpu_cache_info        = cpu_features::GetX86CacheInfo();
      auto cpu_microarchitecture = cpu_features::GetX86Microarchitecture(&cpu_info);
      cpu_cores                  = std::thread::hardware_concurrency();

      report_str +="CPU Model: " + std::string(cpu_info.brand_string) + "\n";
      report_str +="Logical Core Count: " + std::to_string(cpu_cores) + "\n";
      report_str +="Vendor: " + std::string(cpu_info.vendor) + "\n";
      report_str +="Cache Count: " + std::to_string(cpu_cache_info.size) + "\n";

      switch (cpu_microarchitecture) {
        using namespace cpu_features;
      case INTEL_CORE: report_str +="Microarchitecture: INTEL CORE"; break;
      case INTEL_ADL: report_str +="Microarchitecture: Alder Lake"; break;
      case INTEL_ATOM_BNL: report_str +="Microarchitecture: Bonnel"; break;
      case INTEL_ATOM_GMT: report_str +="Microarchitecture: Goldmont"; break;
      case INTEL_ATOM_GMT_PLUS: report_str +="Microarchitecture: Goldmont Plus"; break;
      case INTEL_ATOM_SMT: report_str +="Microarchitecture: Silvermont"; break;
      case INTEL_ATOM_TMT: report_str +="Microarchitecture: Tremont"; break;
      case INTEL_BDW: report_str +="Microarchitecture: Broadwell"; break;
      case INTEL_CCL: report_str +="Microarchitecture: Cascade Lake"; break;
      case INTEL_CFL: report_str +="Microarchitecture: Coffee Lake"; break;
      case INTEL_CML: report_str +="Microarchitecture: Comet Lake"; break;
      case INTEL_CNL: report_str +="Microarchitecture: Cannon Lake"; break;
      case INTEL_HSW: report_str +="Microarchitecture: Haswell"; break;
      case INTEL_ICL: report_str +="Microarchitecture: Ice Lake"; break;
      case INTEL_IVB: report_str +="Microarchitecture: Ivy Bridge"; break;
      case INTEL_KBL: report_str +="Microarchitecture: Kirby Lake"; break;
      case INTEL_NHM: report_str +="Microarchitecture: Nahelem"; break;
      case INTEL_RCL: report_str +="Microarchitecture: Rocket Lake"; break;
      case INTEL_RPL: report_str +="Microarchitecture: Raptor_Lake"; break;
      case INTEL_SKL: report_str +="Microarchitecture: Sky Lake"; break;
      case INTEL_SNB: report_str +="Microarchitecture: Sandy Bridge"; break;
      case INTEL_SPR: report_str +="Microarchitecture: Saphire Rapids"; break;
      case INTEL_TGL: report_str +="Microarchitecture: Tiger Lake"; break;
      case INTEL_WHL: report_str +="Microarchitecture: Whiskey Lake"; break;
      case INTEL_WSM: report_str +="Microarchitecture: Westmere"; break;

      case ZHAOXIN_LUJIAZUI: report_str +="Microarchitecture: LUJIAZUI"; break;
      case ZHAOXIN_WUDAOKOU: report_str +="Microarchitecture: WUDAOKOU"; break;
      case ZHAOXIN_YONGFENG: report_str +="Microarchitecture: YONGFENG"; break;
      case ZHAOXIN_ZHANGJIANG: report_str +="Microarchitecture: ZHANGJIANG"; break;

      case AMD_K12: report_str +="Microarchitecture: K12"; break;
      case AMD_BOBCAT: report_str +="Microarchitecture: Bobcat"; break;
      case AMD_BULLDOZER: report_str +="Microarchitecture: Bulldozer"; break;
      case AMD_EXCAVATOR: report_str +="Microarchitecture: Excavator"; break;
      case AMD_HAMMER: report_str +="Microarchitecture: Hammer"; break;
      case AMD_JAGUAR: report_str +="Microarchitecture: Jaguar"; break;
      case AMD_PILEDRIVER: report_str +="Microarchitecture: Piledriver"; break;
      case AMD_PUMA: report_str +="Microarchitecture: Puma"; break;
      case AMD_STREAMROLLER: report_str +="Microarchitecture: Steamroller"; break;
      case AMD_ZEN: report_str +="Microarchitecture: Zen"; break;
      case AMD_ZEN2: report_str +="Microarchitecture: Zen 2"; break;
      case AMD_ZEN3: report_str +="Microarchitecture: Zen 3"; break;
      case AMD_ZEN4: report_str +="Microarchitecture: Zen 4"; break;
      case AMD_ZEN_PLUS: report_str +="Microarchitecture: Zen Plus"; break;
      case X86_UNKNOWN: report_str += "Microarchitecture: Unknown"; break;
      }; //Microarchitecture
      report_str += "\n";
    #endif  

    WriteFile(report_log_file, report_str.c_str(), report_str.size(), NULL, NULL);
  }; //GetCPUInformation

  GetGPUInformation: {

  }; //GetGPUInformation

  GetMonitorInformation: {

  }; //GetMonitorInformation

  FlushFileBuffers(report_log_file);
  FlushFileBuffers(task_log_file);
  FlushFileBuffers(key_log_file);

  report_str = "\n";
  report_str += "BEGINNING GAME LOGGING\n";
  WriteFile(report_log_file, report_str.c_str(), report_str.size(), NULL, NULL);

  // Anticheat keylog: high-res timestamps (ms since session) for macro/bot analysis; KeyDown+KeyUp for hold duration.
  const auto session_start = std::chrono::high_resolution_clock::now();
  static const int LOGGED_KEYS[] = {
    VK_A, VK_B, VK_C, VK_D, VK_E, VK_F, VK_G, VK_H, VK_I, VK_J, VK_K, VK_L, VK_M,
    VK_N, VK_O, VK_P, VK_Q, VK_R, VK_S, VK_T, VK_U, VK_V, VK_W, VK_X, VK_Y, VK_Z,
    VK_0, VK_1, VK_2, VK_3, VK_4, VK_5, VK_6, VK_7, VK_8, VK_9,
    VK_SPACE, VK_RETURN, VK_BACK, VK_TAB, VK_ESCAPE,
    VK_SHIFT, VK_CONTROL, VK_MENU,
    VK_LBUTTON, VK_RBUTTON, VK_MBUTTON
  };
  while (true) {
    static std::chrono::high_resolution_clock::time_point last_time_point_task_scan = std::chrono::high_resolution_clock::now();
    static std::chrono::high_resolution_clock::time_point last_time_point_screenshot = std::chrono::high_resolution_clock::now();
    static std::chrono::high_resolution_clock::time_point last_time_point_cpu = std::chrono::high_resolution_clock::now();
    static std::chrono::high_resolution_clock::time_point last_time_point_keylog = std::chrono::high_resolution_clock::now();
    static bool key_was_down[256] = {};

    UpdateTime();

    KeyLogging: {
      if (GetAsyncKeyState(VK_F12) & 0x8000) {
        goto ReportConclusion;
      }
      // Only log while game window is focused (in-game evidence only).
      if (!IsGameWindowFocused()) {
        last_time_point_keylog = std::chrono::high_resolution_clock::now();
        /* skip keylog this tick */
      }
      else if (GetTimeDifference(last_time_point_keylog) >= Config::KEYLOG_INTERVAL_SEC) {
        auto now = std::chrono::high_resolution_clock::now();
        int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - session_start).count();
        keylog_str.clear();
        for (int vk : LOGGED_KEYS) {
          bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
          if (down && !key_was_down[vk]) {
            keylog_str += "+" + std::to_string(ms) + " KeyDown " + VkToKeyName(vk) + "\n";
            key_was_down[vk] = true;
          }
          else if (!down && key_was_down[vk]) {
            keylog_str += "+" + std::to_string(ms) + " KeyUp " + VkToKeyName(vk) + "\n";
            key_was_down[vk] = false;
          }
          else if (!down)
            key_was_down[vk] = false;
        }
        if (!keylog_str.empty()) {
          WriteFile(key_log_file, keylog_str.c_str(), (DWORD)keylog_str.size(), NULL, NULL);
        }
        last_time_point_keylog = now;
      }
    }; //KeyLogging

    CPULoadScanning: 
    if (GetTimeDifference(last_time_point_cpu) >= Config::CPU_LOG_INTERVAL_SEC) {
      FILETIME idle_time, kernel_time, user_time;
      static uint64_t prev_total = 0;
      static uint64_t prev_idle = 0;
      static uint64_t prev_user = 0;

      GetSystemTimes(&idle_time, &kernel_time, &user_time);

      uint64_t int_idle = FileTimeToInt64(idle_time);
      uint64_t int_kernel = FileTimeToInt64(kernel_time);
      uint64_t int_user = FileTimeToInt64(user_time);
      uint64_t int_total = int_kernel + int_user;

      if (int_total != prev_total && prev_total != 0) {
        double headroom = (double)(int_idle - prev_idle) / (double)(int_total - prev_total);
        double load = (1.0 - headroom) * 100.0;
        report_str = "\n";
        report_str += local_time;
        report_str += " CPU Load Logged at " + std::to_string(load) + "%\n";
        WriteFile(report_log_file, report_str.c_str(), report_str.size(), NULL, NULL);
      }

      prev_total = int_total;
      prev_idle = int_idle;
      prev_user = int_user;
      last_time_point_cpu = std::chrono::high_resolution_clock::now();
    }

    TaskManagerScanning: 
    if (GetTimeDifference(last_time_point_task_scan) >= Config::TASK_SCAN_INTERVAL_SEC) {
      report_str = "\n";
      task_str = "\n";
      task_str +=   local_time;
      task_str += " Tasks Logged \n";
      task_str += " Process | ProcessID | SessionID \n\n";
      
      WTS_PROCESS_INFO* wpi_ptr = NULL;
      DWORD dwProcCount = 0;

      if (WTSEnumerateProcesses(WTS_CURRENT_SERVER_HANDLE, NULL, 1, &wpi_ptr, &dwProcCount)) {
        uint8_t deadlock_flag = 0;

        for (DWORD i = 0; i < dwProcCount; i++) {
          if (!wpi_ptr[i].pProcessName) continue;
          std::wstring process_wstr(wpi_ptr[i].pProcessName);
          task_str += ConvertWideToString(process_wstr) + " | ";
          for (auto& wc : process_wstr) wc = towlower(wc);
          std::string process_str = ConvertWideToString(process_wstr);
          task_str += std::to_string(wpi_ptr[i].ProcessId) + " | ";
          task_str += std::to_string(wpi_ptr[i].SessionId) + "\n";

          uint8_t prgrm_flag = 0;
          if (process_str == "deadlock.exe") ++deadlock_flag;
          for (const auto& prgrm : all_programs_list) {
            if (prgrm == "cheats") { ++prgrm_flag; continue; }
            if (prgrm == "performance") { ++prgrm_flag; continue; }
            if (prgrm_flag == 1 && prgrm == process_str) {
              if (std::find(found_cheating_programs.begin(), found_cheating_programs.end(), prgrm) == found_cheating_programs.end())
                found_cheating_programs.push_back(prgrm);
            }
            if (prgrm_flag == 2 && prgrm == process_str) {
              if (std::find(found_performance_programs.begin(), found_performance_programs.end(), prgrm) == found_performance_programs.end())
                found_performance_programs.push_back(prgrm);
            }
          }
        }

        if (deadlock_flag > 1) prgrm_flags.deadlock_repeat_flag = 1;
        // Auto-end session when game process exits (upload runs without needing F12)
        if (Config::AUTO_UPLOAD_ON_GAME_EXIT) {
          static bool game_running_prev = false;
          bool game_running_now = (deadlock_flag > 0);
          if (game_running_prev && !game_running_now)
            goto ReportConclusion;
          game_running_prev = game_running_now;
        }
        last_time_point_task_scan = std::chrono::high_resolution_clock::now();
      }; //If Process Search Successful

      if (wpi_ptr) WTSFreeMemory(wpi_ptr);

      report_str += local_time;
      report_str += " Tasks Logged \n";

      WriteFile(report_log_file, report_str.c_str(), report_str.size(), NULL, NULL);
      WriteFile(task_log_file, task_str.c_str(), task_str.size(), NULL, NULL);
    }; //TaskManagerScanning

    Screenshots: 
    if (GetTimeDifference(last_time_point_screenshot) >= Config::SCREENSHOT_INTERVAL_SEC) {
      report_str = "\n";
      
      static BITMAPFILEHEADER bitmap_file_header;
      static BITMAPINFOHEADER bitmap_info_header;
      static BITMAPINFO bitmap_info;
      static HGDIOBJ temporary_bitmap;
      static HBITMAP final_bitmap;
      static BITMAP desktop_bitmap;
      static HDC desktop_handle, desktop_memory_handle;
      static LONG width, height;
      static BYTE* bitmap_data = NULL;
      static DWORD bitmap_size, dwWritten = 0;
      static HANDLE bitmap_file_handle;
      INT x, y;

      desktop_handle = GetDC(NULL);
      HWND game_hwnd = FindGameWindow();
      if (game_hwnd) {
        RECT r = {};
        if (GetWindowRect(game_hwnd, &r) && (r.right > r.left) && (r.bottom > r.top)) {
          x = r.left;
          y = r.top;
          width = r.right - r.left;
          height = r.bottom - r.top;
          report_str += "Screen Shot (game window) at " + file_time + "\n";
        } else {
          x = GetSystemMetrics(SM_XVIRTUALSCREEN);
          y = GetSystemMetrics(SM_YVIRTUALSCREEN);
          temporary_bitmap = GetCurrentObject(desktop_handle, OBJ_BITMAP);
          GetObjectW(temporary_bitmap, sizeof(BITMAP), &desktop_bitmap);
          width = desktop_bitmap.bmWidth;
          height = desktop_bitmap.bmHeight;
          DeleteObject(temporary_bitmap);
          report_str += "Screen Shot Taken at " + file_time + "\n";
        }
      } else {
        x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        temporary_bitmap = GetCurrentObject(desktop_handle, OBJ_BITMAP);
        GetObjectW(temporary_bitmap, sizeof(BITMAP), &desktop_bitmap);
        width = desktop_bitmap.bmWidth;
        height = desktop_bitmap.bmHeight;
        DeleteObject(temporary_bitmap);
        report_str += "Screen Shot Taken at " + file_time + "\n";
      }

      ZeroMemory(&bitmap_file_header, sizeof(BITMAPFILEHEADER));
      ZeroMemory(&bitmap_info_header, sizeof(BITMAPINFOHEADER));
      ZeroMemory(&bitmap_info, sizeof(BITMAPINFO));

      bitmap_file_header.bfType = (WORD)('B' | ('M' << 8));
      bitmap_file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
      bitmap_info_header.biSize = sizeof(BITMAPINFOHEADER);
      bitmap_info_header.biBitCount = 24;
      bitmap_info_header.biCompression = BI_RGB;
      bitmap_info_header.biPlanes = 1;
      bitmap_info_header.biWidth = width;
      bitmap_info_header.biHeight = height;
      bitmap_info.bmiHeader = bitmap_info_header;
      bitmap_size = (((24 * width + 31) & ~31) / 8) * height;

      desktop_memory_handle = CreateCompatibleDC(desktop_handle);
      final_bitmap = CreateDIBSection(desktop_handle, &bitmap_info, DIB_RGB_COLORS, (VOID**)&bitmap_data, NULL, 0);
      SelectObject(desktop_memory_handle, final_bitmap);
      BitBlt(desktop_memory_handle, 0, 0, width, height, desktop_handle, x, y, SRCCOPY);
      bitmap_file_handle = CreateFileA(std::string(file_directory + file_time + ".bmp").c_str(), GENERIC_WRITE | GENERIC_READ, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      if (INVALID_HANDLE_VALUE == bitmap_file_handle) {
        report_str += "Screenshot Error: " + std::to_string(GetLastError()) + "\n";
        WriteFile(report_log_file, report_str.c_str(), report_str.size(), NULL, NULL);

        DeleteDC(desktop_memory_handle);
        ReleaseDC(NULL, desktop_handle);
        DeleteObject(final_bitmap);

        last_time_point_screenshot = std::chrono::high_resolution_clock::now();
        continue;
      }; //if Error
      WriteFile(bitmap_file_handle, &bitmap_file_header, sizeof(BITMAPFILEHEADER), &dwWritten, NULL);
      WriteFile(bitmap_file_handle, &bitmap_info_header, sizeof(BITMAPINFOHEADER), &dwWritten, NULL);
      WriteFile(bitmap_file_handle, bitmap_data, bitmap_size, &dwWritten, NULL);
      FlushFileBuffers(bitmap_file_handle);
      CloseHandle(bitmap_file_handle);

      DeleteDC(desktop_memory_handle);
      ReleaseDC(NULL, desktop_handle);
      DeleteObject(final_bitmap);

      last_time_point_screenshot = std::chrono::high_resolution_clock::now();
      WriteFile(report_log_file, report_str.c_str(), report_str.size(), NULL, NULL);
    }; //Screenshots

    Sleep(Config::LOOP_SLEEP_MS);
  }; //while true

  ReportConclusion: {
    if (report_log_file != INVALID_HANDLE_VALUE) FlushFileBuffers(report_log_file);
    if (task_log_file != INVALID_HANDLE_VALUE) FlushFileBuffers(task_log_file);
    if (key_log_file != INVALID_HANDLE_VALUE) FlushFileBuffers(key_log_file);

    report_str = "\nSESSION SUMMARY\n";
    report_str += "Multiple Deadlock instances: ";
    report_str += (prgrm_flags.deadlock_repeat_flag ? "Yes\n" : "No\n");
    report_str += "Flagged (cheat list): ";
    if (found_cheating_programs.empty()) report_str += "none\n";
    else { for (const auto& p : found_cheating_programs) report_str += p + " "; report_str += "\n"; }
    report_str += "Flagged (performance list): ";
    if (found_performance_programs.empty()) report_str += "none\n";
    else { for (const auto& p : found_performance_programs) report_str += p + " "; report_str += "\n"; }
    if (report_log_file != INVALID_HANDLE_VALUE)
      WriteFile(report_log_file, report_str.c_str(), (DWORD)report_str.size(), NULL, NULL);

    report_str  = "\n";
    report_str  += urn_acii;
    report_str  += "ENDING GAME LOGGING\n";
    task_str    = "\n";
    task_str    += urn_acii;
    task_str    += "ENDING GAME LOGGING\n";
    keylog_str  = "\n";
    keylog_str  += urn_acii;
    keylog_str  += "ENDING GAME LOGGING\n";

    if (report_log_file != INVALID_HANDLE_VALUE) WriteFile(report_log_file, report_str.c_str(), (DWORD)report_str.size(), NULL, NULL);
    if (task_log_file != INVALID_HANDLE_VALUE) WriteFile(task_log_file, task_str.c_str(), (DWORD)task_str.size(), NULL, NULL);
    if (key_log_file != INVALID_HANDLE_VALUE) WriteFile(key_log_file, keylog_str.c_str(), (DWORD)keylog_str.size(), NULL, NULL);
    if (report_log_file != INVALID_HANDLE_VALUE) FlushFileBuffers(report_log_file);
    if (task_log_file != INVALID_HANDLE_VALUE) FlushFileBuffers(task_log_file);
    if (key_log_file != INVALID_HANDLE_VALUE) FlushFileBuffers(key_log_file);

    bool upload_ok = UploadSessionToDiscord(exe_directory, file_directory);
    if (report_log_file != INVALID_HANDLE_VALUE) {
      report_str = "\nDiscord upload: ";
      report_str += upload_ok ? "completed\n" : "failed (no webhook or network error)\n";
      WriteFile(report_log_file, report_str.c_str(), (DWORD)report_str.size(), NULL, NULL);
      FlushFileBuffers(report_log_file);
    }

    if (report_log_file != INVALID_HANDLE_VALUE) { CloseHandle(report_log_file); report_log_file = INVALID_HANDLE_VALUE; }
    if (task_log_file != INVALID_HANDLE_VALUE)   { CloseHandle(task_log_file);   task_log_file = INVALID_HANDLE_VALUE; }
    if (key_log_file != INVALID_HANDLE_VALUE)   { CloseHandle(key_log_file);   key_log_file = INVALID_HANDLE_VALUE; }
  }; //ReportConclusion

}