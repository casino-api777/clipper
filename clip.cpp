#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <wtsapi32.h>
#define INITGUID
#include <initguid.h>
#include <UIAutomation.h>
#include <uiautomationcoreapi.h>

#ifndef UIA_IsReadOnlyPropertyId
#define UIA_IsReadOnlyPropertyId 30046
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr ULONG_PTR kInjectTag = 0x434c4950;
constexpr int kPollTimerId = 1;
constexpr int kConsoleReleaseTimerId = 2;
constexpr int kConsoleInjectDeferTimerId = 3;
constexpr DWORD kConsoleReleaseMs = 300;
constexpr DWORD kConsoleInjectDeferMs = 20;
constexpr UINT WM_CLIP_KEYDOWN = WM_APP + 1;
constexpr UINT WM_CLIP_KEYUP = WM_APP + 2;

constexpr wchar_t kInstallCompanyDir[] = L"Hickory Phantom";
constexpr wchar_t kInstallProductDir[] = L"Clipper";
constexpr wchar_t kInstallExeName[] = L"clip.exe";
constexpr wchar_t kStartupValueName[] = L"Clipper";
constexpr wchar_t kStartupTaskName[] = L"HickoryPhantomClipper";
constexpr wchar_t kServiceName[] = L"Hickory Phantom Clipper";
constexpr wchar_t kConsoleTitle[] = L"Clipper";
constexpr int kPublisherCertResId = 101;
constexpr wchar_t kArgAll[] = L"--all";
constexpr wchar_t kArgService[] = L"--service";
constexpr wchar_t kArgAgent[] = L"--agent";
constexpr DWORD kServiceRestartDelayMs = 7000;

constexpr WORD kVkShift = 0x10;
constexpr WORD kVkLShift = 0xa0;
constexpr WORD kVkRShift = 0xa1;
constexpr WORD kVkCtrl = 0x11;
constexpr WORD kVkAlt = 0x12;
constexpr WORD kVkLCtrl = 0xa2;
constexpr WORD kVkRCtrl = 0xa3;
constexpr WORD kVkLWin = 0x5b;
constexpr WORD kVkRWin = 0x5c;
constexpr WORD kVkLeft = 0x25;
constexpr WORD kVkBack = 0x08;
constexpr WORD kVkDelete = 0x2e;

constexpr WORD kHotkeyStart = 0x53;
constexpr WORD kHotkeyEnd = 0x46;
constexpr WORD kHotkeyToggleAll = 0x45;
constexpr WORD kHotkeyToggleConsole = 0x48;
constexpr WORD kHotkeyClose = 0x43;

constexpr LONG kEsPassword = 0x0020;
constexpr LONG kEsNumber = 0x2000;
constexpr LONG kEsReadonly = 0x0800;
constexpr int kGwlStyle = -16;
constexpr int kMinKeyCount = 12;
constexpr int kMaxKeyCount = 22;

int gMinRandom = kMinKeyCount;
int gMaxRandom = kMaxKeyCount;
DWORD gIgnoreMs = 80;
DWORD gPollMs = 250;
DWORD gWindowKeyIntervalMs = 0;
DWORD gConsoleKeyIntervalMs = 0;
DWORD gConsoleInjectDeferMs = kConsoleInjectDeferMs;
bool gDebug = false;
bool gAllInputs = false;
bool gConsoleVisible = false;
bool gBannerPrinted = false;

bool gActive = false;
bool gShiftHeld = false;
bool gProcessing = false;
bool gWindowCleaning = false;
bool gFieldFocused = false;
bool gComReady = false;

ULONGLONG gIgnoreUntil = 0;
HWND gMsgHwnd = nullptr;
HHOOK gBlockHook = nullptr;
HANDLE gInstanceMutex = nullptr;
std::unordered_set<WORD> gKeysHandledUntilUp;
std::deque<WORD> gPendingInjectKeys;
std::deque<WORD> gPendingConsoleInjects;
std::vector<wchar_t> gCharPool;
DWORD gCachedConsolePid = 0;
DWORD gForeignConsolePid = 0;
HANDLE gForeignConsoleIn = INVALID_HANDLE_VALUE;
bool gForeignConsoleInOwned = false;
bool gClipConsoleWasAttached = false;
bool gClipConsoleVisibleBeforeDetach = false;

void releaseForeignConsoleAttach();

IUIAutomation* gAutomation = nullptr;
SERVICE_STATUS_HANDLE gServiceStatusHandle = nullptr;
SERVICE_STATUS gServiceStatus = {};
HANDLE gServiceStopEvent = nullptr;
PROCESS_INFORMATION gServiceAgent = {};
bool gServiceAgentRunning = false;
bool gServiceAllInputs = false;

bool envTruthy(const char* name) {
  const char* v = std::getenv(name);
  return v && v[0] == '1' && v[1] == '\0';
}

int envInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  return std::atoi(v);
}

bool isExtendedVk(WORD vk) {
  switch (vk) {
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x2d:
    case 0x2e:
    case 0x6e:
    case 0x6f:
      return true;
    default:
      return false;
  }
}

bool isKeyDown(WORD vk) {
  return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool isShiftKey(WORD vk) {
  return vk == kVkShift || vk == kVkLShift || vk == kVkRShift;
}

bool isAnyShiftDown() {
  return isKeyDown(kVkShift) || isKeyDown(kVkLShift) || isKeyDown(kVkRShift);
}

bool isHotkeyModifierDown() {
  return isKeyDown(kVkCtrl) || isKeyDown(kVkLCtrl) || isKeyDown(kVkRCtrl);
}

bool isHotkeyComboDown() {
  return isHotkeyModifierDown() && isAnyShiftDown() && isKeyDown(kVkAlt);
}

bool isBlockingModifierDown() {
  return isHotkeyModifierDown() || isKeyDown(kVkAlt) || isKeyDown(kVkLWin) || isKeyDown(kVkRWin);
}

bool isModifierKey(WORD vk) {
  return vk == kVkCtrl || vk == kVkAlt || vk == kVkLCtrl || vk == kVkRCtrl || vk == kVkLWin ||
         vk == kVkRWin;
}

bool isDisplayableVk(WORD vk) {
  if (vk >= 0x30 && vk <= 0x39) return true;
  if (vk >= 0x41 && vk <= 0x5a) return true;
  if (vk >= 0x60 && vk <= 0x69) return true;
  return false;
}

std::wstring toLower(std::wstring s) {
  for (wchar_t& ch : s) {
    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
  }
  return s;
}

bool icontains(const std::wstring& haystack, const wchar_t* needle) {
  return toLower(haystack).find(needle) != std::wstring::npos;
}

bool ilikePasswordAid(const std::wstring& aid) {
  if (aid.empty()) return false;
  std::wstring lower = toLower(aid);
  if (lower.rfind(L"passwordfield", 0) == 0) return true;
  return lower.find(L"assword") != std::wstring::npos;
}

bool ilikeNumericPasswordHint(const std::wstring& text) {
  if (text.empty()) return false;
  const std::wstring lower = toLower(text);
  if (lower.find(L"pin") != std::wstring::npos) return true;
  if (lower.find(L"otp") != std::wstring::npos) return true;
  if (lower.find(L"passcode") != std::wstring::npos) return true;
  if (lower.find(L"numeric") != std::wstring::npos) return true;
  if (lower.find(L"verification code") != std::wstring::npos) return true;
  if (lower.find(L"one-time") != std::wstring::npos) return true;
  return false;
}

HWND getFocusedHwnd() {
  HWND foreground = GetForegroundWindow();
  if (!foreground) return nullptr;

  DWORD threadId = GetWindowThreadProcessId(foreground, nullptr);
  if (!threadId) return nullptr;

  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  if (!GetGUIThreadInfo(threadId, &info)) return nullptr;
  return info.hwndFocus ? info.hwndFocus : foreground;
}

std::wstring getHwndClassName(HWND hwnd) {
  if (!hwnd) return L"";
  wchar_t buf[256] = {};
  const int len = GetClassNameW(hwnd, buf, 256);
  if (len <= 0) return L"";
  return std::wstring(buf, static_cast<size_t>(len));
}

std::wstring getWindowTitle(HWND hwnd) {
  if (!hwnd) return L"";
  wchar_t buf[512] = {};
  const int len = GetWindowTextW(hwnd, buf, 512);
  if (len <= 0) return L"";
  return std::wstring(buf, static_cast<size_t>(len));
}

bool isWin32TextInput() {
  HWND hwnd = getFocusedHwnd();
  if (!hwnd) return false;

  const std::wstring className = getHwndClassName(hwnd);
  if (className.rfind(L"Edit", 0) != 0 && className.rfind(L"RichEdit", 0) != 0 &&
      className.rfind(L"RICHEDIT", 0) != 0) {
    return false;
  }

  const LONG style = GetWindowLongW(hwnd, kGwlStyle);
  return (style & kEsReadonly) == 0;
}

bool isWin32PasswordField() {
  HWND hwnd = getFocusedHwnd();
  if (!hwnd) return false;
  const LONG style = GetWindowLongW(hwnd, kGwlStyle);
  return (style & kEsPassword) != 0;
}

bool isWin32NumericPasswordField() {
  HWND hwnd = getFocusedHwnd();
  if (!hwnd) return false;
  const LONG style = GetWindowLongW(hwnd, kGwlStyle);
  return (style & kEsPassword) != 0 && (style & kEsNumber) != 0;
}

bool isCredentialUiForeground() {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd) return false;
  if (icontains(getHwndClassName(hwnd), L"credential")) return true;
  return icontains(getWindowTitle(hwnd), L"windows security");
}

bool isSameProcessWindow(HWND hwnd) {
  if (!hwnd) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  return pid == GetCurrentProcessId();
}

bool isConsoleClassName(const std::wstring& className) {
  if (className.empty()) return false;
  if (className == L"ConsoleWindowClass") return true;
  if (className == L"PseudoConsoleWindow") return true;
  if (icontains(className, L"CASCADIA")) return true;
  return false;
}

bool isWindowInForeignConsole(HWND hwnd) {
  if (!hwnd) return false;

  const HWND ourConsole = GetConsoleWindow();
  if (ourConsole && hwnd == ourConsole) return false;
  if (isSameProcessWindow(hwnd)) return false;

  for (HWND walk = hwnd; walk; walk = GetParent(walk)) {
    if (isConsoleClassName(getHwndClassName(walk))) return true;
  }
  return false;
}

bool isConsoleProcessId(DWORD pid) {
  if (!pid) return false;
  const HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) return false;

  wchar_t path[MAX_PATH] = {};
  DWORD size = MAX_PATH;
  const bool ok = QueryFullProcessImageNameW(proc, 0, path, &size) != FALSE;
  CloseHandle(proc);
  if (!ok) return false;

  const std::wstring lower = toLower(std::wstring(path, size));
  return lower.find(L"\\cmd.exe") != std::wstring::npos ||
         lower.find(L"\\powershell.exe") != std::wstring::npos ||
         lower.find(L"\\pwsh.exe") != std::wstring::npos ||
         lower.find(L"\\windowsterminal.exe") != std::wstring::npos ||
         lower.find(L"\\openconsole.exe") != std::wstring::npos ||
         lower.find(L"\\wt.exe") != std::wstring::npos ||
         lower.find(L"\\conhost.exe") != std::wstring::npos;
}

bool isConsoleWindowTitle(const std::wstring& title) {
  if (title.empty()) return false;
  const std::wstring lower = toLower(title);
  return lower.find(L"powershell") != std::wstring::npos ||
         lower.find(L"command prompt") != std::wstring::npos ||
         lower.find(L"windows terminal") != std::wstring::npos ||
         lower.find(L"cmd.exe") != std::wstring::npos;
}

bool isWin32ConsoleInput() {
  const HWND focus = getFocusedHwnd();
  if (isWindowInForeignConsole(focus)) return true;

  const HWND foreground = GetForegroundWindow();
  if (!foreground) return false;

  if (isWindowInForeignConsole(foreground)) return true;

  DWORD foregroundPid = 0;
  GetWindowThreadProcessId(foreground, &foregroundPid);
  if (isConsoleProcessId(foregroundPid)) return true;
  if (isConsoleWindowTitle(getWindowTitle(foreground))) return true;

  const HWND focusHwnd = focus ? focus : foreground;
  if (!focusHwnd || focusHwnd == foreground) return false;

  DWORD focusPid = 0;
  GetWindowThreadProcessId(focusHwnd, &focusPid);
  return focusPid == foregroundPid;
}

bool getElementBoolProp(IUIAutomationElement* element, PROPERTYID propId) {
  VARIANT var = {};
  VariantInit(&var);
  if (FAILED(element->GetCurrentPropertyValue(propId, &var))) {
    VariantClear(&var);
    return false;
  }
  const bool value = var.vt == VT_BOOL && var.boolVal == VARIANT_TRUE;
  VariantClear(&var);
  return value;
}

std::wstring getElementBstr(IUIAutomationElement* element, PROPERTYID propId) {
  VARIANT var = {};
  VariantInit(&var);
  if (FAILED(element->GetCurrentPropertyValue(propId, &var)) || var.vt != VT_BSTR || !var.bstrVal) {
    VariantClear(&var);
    return L"";
  }
  std::wstring out(var.bstrVal);
  VariantClear(&var);
  return out;
}

CONTROLTYPEID getElementControlType(IUIAutomationElement* element) {
  CONTROLTYPEID typeId = 0;
  if (FAILED(element->get_CurrentControlType(&typeId))) return 0;
  return typeId;
}

bool isEditableControlType(CONTROLTYPEID typeId) {
  return typeId == UIA_EditControlTypeId || typeId == UIA_DocumentControlTypeId;
}

bool queryUiaPasswordField() {
  if (!gComReady || !gAutomation) return false;

  IUIAutomationElement* focused = nullptr;
  if (FAILED(gAutomation->GetFocusedElement(&focused)) || !focused) return false;

  IUIAutomationTreeWalker* walker = nullptr;
  if (FAILED(gAutomation->get_RawViewWalker(&walker))) {
    focused->Release();
    return false;
  }

  bool found = false;
  IUIAutomationElement* cur = focused;
  cur->AddRef();

  for (int i = 0; i < 12 && cur; ++i) {
    if (getElementBoolProp(cur, UIA_IsPasswordPropertyId)) {
      found = true;
      break;
    }

    const std::wstring aid = getElementBstr(cur, UIA_AutomationIdPropertyId);
    if (ilikePasswordAid(aid)) {
      found = true;
      break;
    }

    const std::wstring name = getElementBstr(cur, UIA_NamePropertyId);
    const CONTROLTYPEID typeId = getElementControlType(cur);
    if (icontains(name, L"password") && isEditableControlType(typeId)) {
      found = true;
      break;
    }

    const std::wstring className = getElementBstr(cur, UIA_ClassNamePropertyId);
    if (icontains(className, L"credential") && typeId == UIA_EditControlTypeId &&
        (ilikePasswordAid(aid) || _wcsicmp(name.c_str(), L"Password") == 0)) {
      found = true;
      break;
    }

    IUIAutomationElement* parent = nullptr;
    if (FAILED(walker->GetParentElement(cur, &parent)) || !parent) break;
    cur->Release();
    cur = parent;
  }

  cur->Release();
  walker->Release();
  focused->Release();
  return found;
}

bool queryUiaNumericPasswordField() {
  if (!gComReady || !gAutomation) return false;

  IUIAutomationElement* focused = nullptr;
  if (FAILED(gAutomation->GetFocusedElement(&focused)) || !focused) return false;

  IUIAutomationTreeWalker* walker = nullptr;
  if (FAILED(gAutomation->get_RawViewWalker(&walker))) {
    focused->Release();
    return false;
  }

  bool found = false;
  IUIAutomationElement* cur = focused;
  cur->AddRef();

  for (int i = 0; i < 12 && cur; ++i) {
    const std::wstring aid = getElementBstr(cur, UIA_AutomationIdPropertyId);
    const std::wstring name = getElementBstr(cur, UIA_NamePropertyId);
    const std::wstring className = getElementBstr(cur, UIA_ClassNamePropertyId);
    const CONTROLTYPEID typeId = getElementControlType(cur);

    const bool passwordLike = getElementBoolProp(cur, UIA_IsPasswordPropertyId) || ilikePasswordAid(aid) ||
                              (icontains(name, L"password") && isEditableControlType(typeId));
    if (passwordLike &&
        (ilikeNumericPasswordHint(aid) || ilikeNumericPasswordHint(name) ||
         ilikeNumericPasswordHint(className))) {
      found = true;
      break;
    }

    IUIAutomationElement* parent = nullptr;
    if (FAILED(walker->GetParentElement(cur, &parent)) || !parent) break;
    cur->Release();
    cur = parent;
  }

  cur->Release();
  walker->Release();
  focused->Release();
  return found;
}

bool isNumericPasswordField() {
  if (isWin32NumericPasswordField()) return true;
  return queryUiaNumericPasswordField();
}

bool queryUiaTextInput() {
  if (!gComReady || !gAutomation) return false;

  IUIAutomationElement* focused = nullptr;
  if (FAILED(gAutomation->GetFocusedElement(&focused)) || !focused) return false;

  const CONTROLTYPEID typeId = getElementControlType(focused);
  if (!isEditableControlType(typeId)) {
    focused->Release();
    return false;
  }

  if (getElementBoolProp(focused, UIA_IsReadOnlyPropertyId)) {
    focused->Release();
    return false;
  }

  focused->Release();
  return true;
}

bool queryUiaConsoleInput() {
  if (!gComReady || !gAutomation) return false;

  IUIAutomationElement* focused = nullptr;
  if (FAILED(gAutomation->GetFocusedElement(&focused)) || !focused) return false;

  IUIAutomationTreeWalker* walker = nullptr;
  if (FAILED(gAutomation->get_RawViewWalker(&walker))) {
    focused->Release();
    return false;
  }

  bool found = false;
  IUIAutomationElement* cur = focused;
  cur->AddRef();

  for (int i = 0; i < 12 && cur; ++i) {
    const std::wstring className = getElementBstr(cur, UIA_ClassNamePropertyId);
    if (icontains(className, L"TermControl") || icontains(className, L"Console") ||
        icontains(className, L"Cascade")) {
      found = true;
      break;
    }

    const std::wstring name = getElementBstr(cur, UIA_NamePropertyId);
    if (icontains(name, L"Terminal") || icontains(name, L"Command Prompt") ||
        icontains(name, L"PowerShell") || icontains(name, L"Windows PowerShell")) {
      found = true;
      break;
    }

    const CONTROLTYPEID typeId = getElementControlType(cur);
    if (isEditableControlType(typeId) && icontains(className, L"Term")) {
      found = true;
      break;
    }

    IUIAutomationElement* parent = nullptr;
    if (FAILED(walker->GetParentElement(cur, &parent)) || !parent) break;
    cur->Release();
    cur = parent;
  }

  cur->Release();
  walker->Release();
  focused->Release();
  return found;
}

bool isConsoleInputActive() {
  if (isWin32ConsoleInput()) return true;
  return queryUiaConsoleInput();
}

bool computePasswordField() {
  if (isWin32PasswordField()) return true;
  if (isCredentialUiForeground()) return queryUiaPasswordField();
  return queryUiaPasswordField();
}

bool computeTextInputField() {
  if (isWin32TextInput()) return true;
  return queryUiaTextInput();
}

bool computeTargetField() {
  if (gAllInputs) {
    return computePasswordField() || computeTextInputField() || isConsoleInputActive();
  }
  return computePasswordField() || isConsoleInputActive();
}

void refreshFieldFocus() {
  gFieldFocused = computeTargetField();
}

bool isTargetFieldFocusedForInput() {
  if (gAllInputs) {
    if (isConsoleInputActive()) return true;
    if (isWin32PasswordField() || isWin32TextInput()) return true;
    if (isCredentialUiForeground()) return true;
    if (gFieldFocused) return true;
    return queryUiaPasswordField() || queryUiaTextInput();
  }

  if (isConsoleInputActive()) return true;
  if (isWin32PasswordField()) return true;
  if (isCredentialUiForeground()) return true;
  if (gFieldFocused) return true;
  return queryUiaPasswordField();
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType);
void printBanner();

void attachConsoleStreams() {
  freopen("CONOUT$", "w", stdout);
  freopen("CONOUT$", "w", stderr);
  freopen("CONIN$", "r", stdin);
}

void initConsoleCore() {
  if (!GetConsoleWindow()) {
    AllocConsole();
    attachConsoleStreams();
  }
  SetConsoleTitleW(kConsoleTitle);
  HWND hwnd = GetConsoleWindow();
  if (hwnd) {
    HMENU systemMenu = GetSystemMenu(hwnd, FALSE);
    if (systemMenu) {
      DeleteMenu(systemMenu, SC_CLOSE, MF_BYCOMMAND);
      DrawMenuBar(hwnd);
    }
  }
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
}

void ensureConsole() {
  initConsoleCore();
  if (!gBannerPrinted) {
    printBanner();
    gBannerPrinted = true;
  }
}

void initHiddenConsole() {
  ensureConsole();
  HWND hwnd = GetConsoleWindow();
  if (hwnd) ShowWindow(hwnd, SW_HIDE);
  gConsoleVisible = false;
}

void setConsoleVisible(bool visible) {
  if (visible) {
    releaseForeignConsoleAttach();
    ensureConsole();
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    gConsoleVisible = true;
  } else {
    HWND hwnd = GetConsoleWindow();
    if (hwnd) ShowWindow(hwnd, SW_HIDE);
    gConsoleVisible = false;
  }
}

void toggleConsole() {
  if (gConsoleVisible) {
    std::puts("[CL] console hidden");
    setConsoleVisible(false);
  } else {
    setConsoleVisible(true);
    std::puts("[CL] console shown");
  }
}

void fatalError(const char* msg) {
  ensureConsole();
  std::fprintf(stderr, "%s\n", msg);
  MessageBoxA(nullptr, msg, "clip.exe", MB_OK | MB_ICONERROR);
}

bool isProcessElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

  TOKEN_ELEVATION elevation = {};
  DWORD size = sizeof(elevation);
  const BOOL ok =
      GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
  CloseHandle(token);
  return ok && elevation.TokenIsElevated != 0;
}

std::wstring quoteCmdArg(const std::wstring& arg) {
  if (arg.empty()) return L"\"\"";
  if (arg.find_first_of(L" \t") == std::wstring::npos) return arg;
  return L"\"" + arg + L"\"";
}

std::wstring buildParameters(int argc, wchar_t** argv) {
  std::wstring params;
  for (int i = 1; i < argc; ++i) {
    if (i > 1) params += L' ';
    params += quoteCmdArg(argv[i]);
  }
  return params;
}

bool hasArg(int argc, wchar_t** argv, const wchar_t* target) {
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], target) == 0) return true;
  }
  return false;
}

bool relaunchElevated(int argc, wchar_t** argv) {
  wchar_t exePath[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;

  const std::wstring params = buildParameters(argc, argv);

  SHELLEXECUTEINFOW sei = {};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOASYNC;
  sei.lpVerb = L"runas";
  sei.lpFile = exePath;
  sei.lpParameters = params.empty() ? nullptr : params.c_str();
  sei.nShow = SW_HIDE;

  if (ShellExecuteExW(&sei)) return true;
  return false;
}

bool acquireSingleInstance() {
  gInstanceMutex = CreateMutexW(nullptr, TRUE, L"Global\\TWNK_Clip_SingleInstance");
  if (!gInstanceMutex) return false;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(gInstanceMutex);
    gInstanceMutex = nullptr;
    return false;
  }
  return true;
}

void releaseSingleInstance() {
  if (!gInstanceMutex) return;
  CloseHandle(gInstanceMutex);
  gInstanceMutex = nullptr;
}

bool ensureElevated(int argc, wchar_t** argv, bool* handoff) {
  if (handoff) *handoff = false;
  if (isProcessElevated()) return true;
  if (relaunchElevated(argc, argv)) {
    if (handoff) *handoff = true;
    return false;
  }

  if (GetLastError() == ERROR_CANCELLED) {
    fatalError("[CL] Administrator privileges are required.");
  } else {
    fatalError("[CL] Failed to restart with Administrator privileges.");
  }
  return false;
}

bool pathsEqualIgnoreCase(const wchar_t* a, const wchar_t* b) {
  if (!a || !b) return false;
  return _wcsicmp(a, b) == 0;
}

bool createDirectoryTree(const wchar_t* path) {
  const int result = SHCreateDirectoryExW(nullptr, path, nullptr);
  return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS;
}

bool addCertToSystemStore(const wchar_t* storeName, PCCERT_CONTEXT cert) {
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                   CERT_SYSTEM_STORE_LOCAL_MACHINE |
                                       CERT_STORE_OPEN_EXISTING_FLAG,
                                   storeName);
  if (!store) return false;

  const BOOL ok =
      CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
  CertCloseStore(store, 0);
  return ok != FALSE;
}

bool writeCertToFile(const wchar_t* path, PCCERT_CONTEXT cert) {
  if (!cert || !cert->pbCertEncoded || cert->cbCertEncoded == 0) return false;

  const HANDLE file =
      CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;

  DWORD written = 0;
  const BOOL wrote =
      WriteFile(file, cert->pbCertEncoded, cert->cbCertEncoded, &written, nullptr);
  CloseHandle(file);
  return wrote != FALSE && written == cert->cbCertEncoded;
}

bool runHiddenCommand(const std::wstring& commandLine) {
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION pi = {};
  std::vector<wchar_t> cmdBuffer(commandLine.begin(), commandLine.end());
  cmdBuffer.push_back(L'\0');

  if (!CreateProcessW(nullptr, cmdBuffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    return false;
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return exitCode == 0;
}

bool runCertutilAddStore(const wchar_t* storeName, const wchar_t* cerPath) {
  std::wstring cmd = L"certutil.exe -f -addstore \"";
  cmd += storeName;
  cmd += L"\" \"";
  cmd += cerPath;
  cmd += L"\"";
  return runHiddenCommand(cmd);
}

bool getSignerCertFromExe(const wchar_t* exePath, PCCERT_CONTEXT* outCert) {
  if (!outCert) return false;
  *outCert = nullptr;

  HCERTSTORE certStore = nullptr;
  HCRYPTMSG msgStore = nullptr;
  if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, exePath, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                        CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr, &certStore,
                        &msgStore, nullptr)) {
    return false;
  }

  PCCERT_CONTEXT signer = nullptr;
  if (certStore) {
    signer = CertFindCertificateInStore(certStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                        CERT_FIND_ANY, nullptr, nullptr);
  }

  if (signer) *outCert = CertDuplicateCertificateContext(signer);
  if (signer) CertFreeCertificateContext(signer);
  if (certStore) CertCloseStore(certStore, 0);
  if (msgStore) CryptMsgClose(msgStore);
  return *outCert != nullptr;
}

bool getSignerCertFromResource(PCCERT_CONTEXT* outCert) {
  if (!outCert) return false;
  *outCert = nullptr;

  const HRSRC resource =
      FindResourceW(nullptr, MAKEINTRESOURCEW(kPublisherCertResId), RT_RCDATA);
  if (!resource) return false;

  const HGLOBAL loaded = LoadResource(nullptr, resource);
  if (!loaded) return false;

  const DWORD size = SizeofResource(nullptr, resource);
  const BYTE* data = static_cast<const BYTE*>(LockResource(loaded));
  if (!data || size == 0) return false;

  PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, data,
                                                     size);
  if (!cert) return false;

  *outCert = cert;
  return true;
}

bool installPublisherTrustFromCert(PCCERT_CONTEXT cert) {
  if (!cert) return false;

  const bool rootOk = addCertToSystemStore(L"Root", cert);
  const bool publisherOk = addCertToSystemStore(L"TrustedPublisher", cert);
  if (rootOk && publisherOk) return true;

  wchar_t cerPath[MAX_PATH] = {};
  if (GetTempPathW(MAX_PATH, cerPath) == 0) return false;
  wcscat_s(cerPath, L"hp-clipper.cer");
  if (!writeCertToFile(cerPath, cert)) return false;

  const bool utilRootOk = runCertutilAddStore(L"Root", cerPath);
  const bool utilPublisherOk = runCertutilAddStore(L"TrustedPublisher", cerPath);
  DeleteFileW(cerPath);
  return utilRootOk && utilPublisherOk;
}

bool installPublisherTrust() {
  static bool attempted = false;
  static bool installed = false;
  if (attempted) return installed;
  attempted = true;

  wchar_t exePath[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;

  PCCERT_CONTEXT cert = nullptr;
  if (!getSignerCertFromExe(exePath, &cert)) {
    getSignerCertFromResource(&cert);
  }
  if (!cert) return false;

  installed = installPublisherTrustFromCert(cert);
  CertFreeCertificateContext(cert);
  return installed;
}

bool setRegistryRunAsAdmin(const wchar_t* exePath) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", 0,
                    KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }

  const wchar_t* runAsAdmin = L"RUNASADMIN";
  const DWORD bytes = static_cast<DWORD>((wcslen(runAsAdmin) + 1) * sizeof(wchar_t));
  const LONG result =
      RegSetValueExW(key, exePath, 0, REG_SZ, reinterpret_cast<const BYTE*>(runAsAdmin), bytes);
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

bool getInstallPaths(std::wstring& installDir, std::wstring& installExe) {
  wchar_t localAppData[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0) return false;

  installDir = std::wstring(localAppData) + L"\\" + kInstallCompanyDir + L"\\" + kInstallProductDir;
  installExe = installDir + L"\\" + kInstallExeName;
  return true;
}

std::wstring buildStartupCommand(const wchar_t* exePath, int argc, wchar_t** argv) {
  std::wstring cmd = L"\"";
  cmd += exePath;
  cmd += L"\"";
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], kArgAll) == 0) {
      cmd += L" --all";
      break;
    }
  }
  return cmd;
}

std::wstring buildStartupArguments(int argc, wchar_t** argv) {
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], kArgAll) == 0) return kArgAll;
  }
  return L"";
}

std::wstring psQuoteSingle(const std::wstring& value) {
  std::wstring out;
  out.reserve(value.size() + 8);
  for (wchar_t ch : value) {
    if (ch == L'\'') out += L"''";
    else out.push_back(ch);
  }
  return out;
}

std::wstring cmdQuote(const std::wstring& value) {
  return L"\"" + value + L"\"";
}

bool setRegistryStartup(const wchar_t* command) {
  HKEY key = nullptr;
  const LONG openResult = RegOpenKeyExW(
      HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE,
      &key);
  if (openResult != ERROR_SUCCESS) return false;

  const DWORD bytes = static_cast<DWORD>((wcslen(command) + 1) * sizeof(wchar_t));
  const LONG setResult = RegSetValueExW(key, kStartupValueName, 0, REG_SZ,
                                        reinterpret_cast<const BYTE*>(command), bytes);
  RegCloseKey(key);
  return setResult == ERROR_SUCCESS;
}

bool setSchedulerStartup(const wchar_t* exePath, const wchar_t* arguments) {
  const std::wstring quotedExe = psQuoteSingle(exePath);
  const std::wstring taskName = kStartupTaskName;

  std::wstring ps = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
  ps += L"Unregister-ScheduledTask -TaskName '";
  ps += taskName;
  ps += L"' -Confirm:$false -ErrorAction SilentlyContinue;";
  ps += L"$a=New-ScheduledTaskAction -Execute '";
  ps += quotedExe;
  ps += L"'";
  if (arguments && arguments[0]) {
    ps += L" -Argument '";
    ps += psQuoteSingle(arguments);
    ps += L"'";
  }
  ps += L";$t=New-ScheduledTaskTrigger -AtLogOn;$t.Delay='PT15S';";
  ps += L"$p=New-ScheduledTaskPrincipal -UserId ($env:USERDOMAIN+'\\'+$env:USERNAME) ";
  ps += L"-LogonType Interactive -RunLevel Limited;";
  ps += L"$s=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries ";
  ps += L"-StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Seconds 0) ";
  ps += L"-MultipleInstances IgnoreNew;";
  ps += L"Register-ScheduledTask -TaskName '";
  ps += taskName;
  ps += L"' -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null\"";

  return runHiddenCommand(ps);
}

bool stopClipService() {
  std::wstring cmd = L"sc.exe stop ";
  cmd += cmdQuote(kServiceName);
  runHiddenCommand(cmd);

  std::wstring ps = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
  ps += L"try{Stop-Service -Name '";
  ps += psQuoteSingle(kServiceName);
  ps += L"' -Force -ErrorAction Stop | Out-Null}catch{}\"";
  runHiddenCommand(ps);
  return true;
}

bool isClipServiceStopped() {
  std::wstring ps = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
  ps += L"$s=Get-Service -Name '";
  ps += psQuoteSingle(kServiceName);
  ps += L"' -ErrorAction SilentlyContinue;";
  ps += L"if($null -eq $s -or $s.Status -eq 'Stopped'){exit 0}else{exit 1}\"";
  return runHiddenCommand(ps);
}

void waitForClipServiceStopped(DWORD maxWaitMs) {
  const DWORD stepMs = 300;
  DWORD waited = 0;
  while (waited < maxWaitMs) {
    if (isClipServiceStopped()) return;
    Sleep(stepMs);
    waited += stepMs;
  }
}

bool getProcessImagePath(DWORD pid, wchar_t* path, DWORD* inOutSize) {
  if (!path || !inOutSize || !*inOutSize) return false;
  const HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) return false;
  const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, inOutSize) != FALSE;
  CloseHandle(proc);
  return ok != FALSE;
}

bool stopClipProcesses(const wchar_t* installExe) {
  const DWORD self = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;

  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  bool stopped = false;

  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, kInstallExeName) != 0) continue;
      if (pe.th32ProcessID == self) continue;

      if (installExe && installExe[0]) {
        wchar_t imagePath[MAX_PATH] = {};
        DWORD imageSize = MAX_PATH;
        if (getProcessImagePath(pe.th32ProcessID, imagePath, &imageSize) &&
            !pathsEqualIgnoreCase(imagePath, installExe)) {
          continue;
        }
      }

      HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
      if (!proc) continue;

      TerminateProcess(proc, 0);
      WaitForSingleObject(proc, 3000);
      CloseHandle(proc);
      stopped = true;
    } while (Process32NextW(snap, &pe));
  }

  CloseHandle(snap);
  if (stopped) Sleep(500);
  return true;
}

void stopClipDeployment(const wchar_t* installExe) {
  stopClipService();
  waitForClipServiceStopped(8000);
  stopClipProcesses(installExe);
  Sleep(300);
}

bool setServiceRecovery(const wchar_t* serviceName) {
  if (!serviceName || !serviceName[0]) return false;

  std::wstring cmd1 = L"sc.exe failure ";
  cmd1 += cmdQuote(serviceName);
  cmd1 += L" reset= 86400 actions= restart/7000/restart/7000/restart/7000";

  std::wstring cmd2 = L"sc.exe failureflag ";
  cmd2 += cmdQuote(serviceName);
  cmd2 += L" 1";

  return runHiddenCommand(cmd1) && runHiddenCommand(cmd2);
}

bool setServiceStartup(const wchar_t* exePath, int argc, wchar_t** argv) {
  if (!exePath || !exePath[0]) return false;

  std::wstring binPath = L"\\\"";
  binPath += exePath;
  binPath += L"\\\" ";
  binPath += kArgService;
  if (hasArg(argc, argv, kArgAll)) binPath += L" --all";

  std::wstring ps = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
  ps += L"$n='";
  ps += psQuoteSingle(kServiceName);
  ps += L"';";
  ps += L"$b='";
  ps += psQuoteSingle(binPath);
  ps += L"';";
  ps += L"$s=Get-Service -Name $n -ErrorAction SilentlyContinue;";
  ps += L"if($null -eq $s){";
  ps += L"New-Service -Name $n -DisplayName $n -BinaryPathName $b -StartupType Automatic | Out-Null";
  ps += L"}else{";
  ps += L"sc.exe config $n binPath= $b start= auto | Out-Null";
  ps += L"};";
  ps += L"try{Start-Service -Name $n -ErrorAction Stop | Out-Null}catch{}\"";

  const bool configured = runHiddenCommand(ps);
  const bool recoverySet = setServiceRecovery(kServiceName);
  return configured && recoverySet;
}

void closeServiceAgentHandles() {
  if (gServiceAgent.hThread) {
    CloseHandle(gServiceAgent.hThread);
    gServiceAgent.hThread = nullptr;
  }
  if (gServiceAgent.hProcess) {
    CloseHandle(gServiceAgent.hProcess);
    gServiceAgent.hProcess = nullptr;
  }
  gServiceAgentRunning = false;
}

void reportServiceStatus(DWORD currentState, DWORD win32ExitCode, DWORD waitHint) {
  if (!gServiceStatusHandle) return;
  gServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  gServiceStatus.dwCurrentState = currentState;
  gServiceStatus.dwWin32ExitCode = win32ExitCode;
  gServiceStatus.dwWaitHint = waitHint;
  gServiceStatus.dwControlsAccepted =
      (currentState == SERVICE_START_PENDING) ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
  SetServiceStatus(gServiceStatusHandle, &gServiceStatus);
}

bool launchServiceAgentForActiveSession(const wchar_t* exePath) {
  const DWORD sessionId = WTSGetActiveConsoleSessionId();
  if (sessionId == 0xFFFFFFFF) return false;

  HANDLE userToken = nullptr;
  if (!WTSQueryUserToken(sessionId, &userToken)) return false;

  HANDLE primaryToken = nullptr;
  const BOOL tokenOk = DuplicateTokenEx(userToken, TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY, nullptr,
                                        SecurityImpersonation, TokenPrimary, &primaryToken);
  CloseHandle(userToken);
  if (!tokenOk) return false;

  std::wstring cmd = cmdQuote(exePath);
  cmd += L" ";
  cmd += kArgAgent;
  if (gServiceAllInputs) cmd += L" --all";
  std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
  cmdline.push_back(L'\0');

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
  PROCESS_INFORMATION pi = {};

  const BOOL created = CreateProcessAsUserW(primaryToken, nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                                            CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi);
  CloseHandle(primaryToken);
  if (!created) return false;

  closeServiceAgentHandles();
  gServiceAgent = pi;
  gServiceAgentRunning = true;
  return true;
}

void WINAPI serviceCtrlHandler(DWORD ctrlCode) {
  if (ctrlCode == SERVICE_CONTROL_STOP || ctrlCode == SERVICE_CONTROL_SHUTDOWN) {
    reportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
    if (gServiceStopEvent) SetEvent(gServiceStopEvent);
  }
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
  gServiceStatusHandle = RegisterServiceCtrlHandlerW(kServiceName, serviceCtrlHandler);
  if (!gServiceStatusHandle) return;

  reportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
  gServiceStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!gServiceStopEvent) {
    reportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
    return;
  }

  wchar_t exePath[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
    CloseHandle(gServiceStopEvent);
    gServiceStopEvent = nullptr;
    reportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
    return;
  }

  reportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);
  while (WaitForSingleObject(gServiceStopEvent, 1000) == WAIT_TIMEOUT) {
    if (!gServiceAgentRunning) {
      if (!launchServiceAgentForActiveSession(exePath)) Sleep(1000);
      continue;
    }
    const DWORD waitResult = WaitForSingleObject(gServiceAgent.hProcess, 0);
    if (waitResult == WAIT_OBJECT_0) {
      closeServiceAgentHandles();
      Sleep(kServiceRestartDelayMs);
    }
  }

  if (gServiceAgentRunning && gServiceAgent.hProcess) TerminateProcess(gServiceAgent.hProcess, 0);
  closeServiceAgentHandles();
  CloseHandle(gServiceStopEvent);
  gServiceStopEvent = nullptr;
  reportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

int runAsServiceProcess(int argc, wchar_t** argv) {
  gServiceAllInputs = hasArg(argc, argv, kArgAll);
  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<LPWSTR>(kServiceName), serviceMain},
      {nullptr, nullptr},
  };
  if (StartServiceCtrlDispatcherW(table)) return 0;
  const DWORD err = GetLastError();
  return (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) ? 0 : 1;
}

bool relaunchExecutable(const wchar_t* exePath, int argc, wchar_t** argv, bool elevated) {
  const std::wstring params = buildParameters(argc, argv);

  SHELLEXECUTEINFOW sei = {};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOASYNC;
  sei.lpVerb = elevated ? L"runas" : nullptr;
  sei.lpFile = exePath;
  sei.lpParameters = params.empty() ? nullptr : params.c_str();
  sei.nShow = SW_HIDE;
  return ShellExecuteExW(&sei) != FALSE;
}

bool configureInstalledInstance(const wchar_t* installExe, int argc, wchar_t** argv) {
  setRegistryRunAsAdmin(installExe);
  const bool serviceOk = setServiceStartup(installExe, argc, argv);
  const std::wstring startupCmd = buildStartupCommand(installExe, argc, argv);
  const std::wstring startupArgs = buildStartupArguments(argc, argv);
  const bool taskOk = setSchedulerStartup(installExe, startupArgs.c_str());
  const bool runOk = setRegistryStartup(startupCmd.c_str());
  return serviceOk || taskOk || runOk;
}

bool restartClipDeployment(const wchar_t* installExe, int argc, wchar_t** argv, bool relaunchProcess) {
  if (!configureInstalledInstance(installExe, argc, argv)) return false;
  if (!relaunchProcess) return true;
  Sleep(500);
  return relaunchExecutable(installExe, argc, argv, true);
}

bool acquireSingleInstanceAfterStop(const wchar_t* installExe) {
  if (acquireSingleInstance()) return true;
  stopClipDeployment(installExe);
  Sleep(1500);
  return acquireSingleInstance();
}

bool ensureInstalled(int argc, wchar_t** argv, bool* handoff) {
  if (handoff) *handoff = false;

  std::wstring installDir;
  std::wstring installExe;
  if (!getInstallPaths(installDir, installExe)) {
    fatalError("[CL] Failed to resolve Local AppData install path.");
    return false;
  }

  wchar_t current[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, current, MAX_PATH)) return false;

  if (pathsEqualIgnoreCase(current, installExe.c_str())) {
    stopClipDeployment(installExe.c_str());
    configureInstalledInstance(installExe.c_str(), argc, argv);
    return true;
  }

  stopClipDeployment(installExe.c_str());

  if (!createDirectoryTree(installDir.c_str())) {
    fatalError("[CL] Failed to create Local AppData install folder.");
    return false;
  }

  if (!CopyFileW(current, installExe.c_str(), FALSE)) {
    fatalError("[CL] Failed to copy clip.exe to Local AppData install folder.");
    return false;
  }

  if (restartClipDeployment(installExe.c_str(), argc, argv, true)) {
    if (handoff) *handoff = true;
    return false;
  }

  fatalError("[CL] Failed to restart service and installed copy after update.");
  return false;
}

void pumpMessages() {
  MSG msg = {};
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

template <typename Fn>
void withFocusedThreadInput(Fn&& run) {
  HWND hwnd = getFocusedHwnd();
  if (!hwnd) hwnd = GetForegroundWindow();
  if (!hwnd) {
    run();
    return;
  }

  const DWORD targetThread = GetWindowThreadProcessId(hwnd, nullptr);
  const DWORD ourThread = GetCurrentThreadId();
  bool attached = false;

  if (targetThread && targetThread != ourThread) {
    attached = AttachThreadInput(ourThread, targetThread, TRUE) != FALSE;
  }

  run();

  if (attached) {
    AttachThreadInput(ourThread, targetThread, FALSE);
  }
}

std::wstring getProcessBaseName(DWORD pid) {
  if (!pid) return L"";
  const HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) return L"";

  wchar_t path[MAX_PATH] = {};
  DWORD size = MAX_PATH;
  const bool ok = QueryFullProcessImageNameW(proc, 0, path, &size) != FALSE;
  CloseHandle(proc);
  if (!ok) return L"";

  const std::wstring full(path, size);
  const size_t slash = full.find_last_of(L"\\/");
  return slash == std::wstring::npos ? toLower(full) : toLower(full.substr(slash + 1));
}

int consoleAttachRank(const std::wstring& name) {
  if (name == L"pwsh.exe" || name == L"powershell.exe" || name == L"cmd.exe") return 0;
  if (name == L"openconsole.exe") return 1;
  if (name == L"conhost.exe") return 2;
  if (name == L"windowsterminal.exe" || name == L"wt.exe") return 10;
  return 5;
}

int consoleWriteInputRank(const std::wstring& name) {
  if (name == L"openconsole.exe") return 0;
  if (name == L"conhost.exe") return 1;
  if (name == L"cmd.exe") return 2;
  if (name == L"pwsh.exe" || name == L"powershell.exe") return 3;
  if (name == L"windowsterminal.exe" || name == L"wt.exe") return 10;
  return 5;
}

struct ConsoleAttachCandidate {
  DWORD pid = 0;
  bool fromHwnd = false;
  int rank = 5;
};

void addConsoleAttachCandidate(std::vector<ConsoleAttachCandidate>& out, DWORD pid, bool fromHwnd) {
  if (!pid || pid == GetCurrentProcessId()) return;
  for (ConsoleAttachCandidate& candidate : out) {
    if (candidate.pid == pid) {
      if (fromHwnd) candidate.fromHwnd = true;
      return;
    }
  }
  const std::wstring name = getProcessBaseName(pid);
  if (name.empty()) return;
  out.push_back({pid, fromHwnd, consoleAttachRank(name)});
}

std::vector<DWORD> getConsoleAttachCandidates() {
  std::vector<ConsoleAttachCandidate> candidates;

  const HWND focus = getFocusedHwnd();
  const HWND foreground = GetForegroundWindow();
  HWND walkStart = focus ? focus : foreground;

  for (HWND walk = walkStart; walk; walk = GetParent(walk)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(walk, &pid);
    addConsoleAttachCandidate(candidates, pid, true);
  }

  if (focus) {
    DWORD pid = 0;
    GetWindowThreadProcessId(focus, &pid);
    addConsoleAttachCandidate(candidates, pid, true);
  }
  if (foreground) {
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    addConsoleAttachCandidate(candidates, pid, true);
  }

  DWORD consolePids[64] = {};
  const DWORD consoleCount = GetConsoleProcessList(consolePids, 64);
  for (DWORD i = 0; i < consoleCount && i < 64; ++i) {
    addConsoleAttachCandidate(candidates, consolePids[i], false);
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const ConsoleAttachCandidate& a, const ConsoleAttachCandidate& b) {
              if (a.fromHwnd != b.fromHwnd) return a.fromHwnd > b.fromHwnd;
              if (a.rank != b.rank) return a.rank < b.rank;
              return a.pid < b.pid;
            });

  std::vector<DWORD> ordered;
  ordered.reserve(candidates.size());
  for (const ConsoleAttachCandidate& candidate : candidates) {
    ordered.push_back(candidate.pid);
  }
  return ordered;
}

std::vector<DWORD> getConsoleWriteCandidates() {
  std::vector<ConsoleAttachCandidate> candidates;

  const HWND focus = getFocusedHwnd();
  const HWND foreground = GetForegroundWindow();
  HWND walkStart = focus ? focus : foreground;

  for (HWND walk = walkStart; walk; walk = GetParent(walk)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(walk, &pid);
    addConsoleAttachCandidate(candidates, pid, true);
  }

  if (focus) {
    DWORD pid = 0;
    GetWindowThreadProcessId(focus, &pid);
    addConsoleAttachCandidate(candidates, pid, true);
  }
  if (foreground) {
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    addConsoleAttachCandidate(candidates, pid, true);
  }

  DWORD consolePids[64] = {};
  const DWORD consoleCount = GetConsoleProcessList(consolePids, 64);
  for (DWORD i = 0; i < consoleCount && i < 64; ++i) {
    addConsoleAttachCandidate(candidates, consolePids[i], false);
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const ConsoleAttachCandidate& a, const ConsoleAttachCandidate& b) {
              if (a.fromHwnd != b.fromHwnd) return a.fromHwnd > b.fromHwnd;
              const int rankA = consoleWriteInputRank(getProcessBaseName(a.pid));
              const int rankB = consoleWriteInputRank(getProcessBaseName(b.pid));
              if (rankA != rankB) return rankA < rankB;
              return a.pid < b.pid;
            });

  std::vector<DWORD> ordered;
  ordered.reserve(candidates.size());
  for (const ConsoleAttachCandidate& candidate : candidates) {
    ordered.push_back(candidate.pid);
  }
  return ordered;
}

DWORD getPreferredConsolePidFromHwnd() {
  std::vector<ConsoleAttachCandidate> candidates;

  const HWND focus = getFocusedHwnd();
  const HWND foreground = GetForegroundWindow();
  HWND walkStart = focus ? focus : foreground;

  for (HWND walk = walkStart; walk; walk = GetParent(walk)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(walk, &pid);
    addConsoleAttachCandidate(candidates, pid, true);
  }

  if (focus) {
    DWORD pid = 0;
    GetWindowThreadProcessId(focus, &pid);
    addConsoleAttachCandidate(candidates, pid, true);
  }
  if (foreground) {
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    addConsoleAttachCandidate(candidates, pid, true);
  }

  if (candidates.empty()) return 0;

  std::sort(candidates.begin(), candidates.end(),
            [](const ConsoleAttachCandidate& a, const ConsoleAttachCandidate& b) {
              if (a.fromHwnd != b.fromHwnd) return a.fromHwnd > b.fromHwnd;
              if (a.rank != b.rank) return a.rank < b.rank;
              return a.pid < b.pid;
            });
  return candidates[0].pid;
}

void restoreClipConsoleWindow() {
  initConsoleCore();
  HWND hwnd = GetConsoleWindow();
  if (!hwnd) return;
  const bool show = gClipConsoleVisibleBeforeDetach || gConsoleVisible;
  if (show) {
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
  } else {
    ShowWindow(hwnd, SW_HIDE);
  }
}

void releaseForeignConsoleAttach() {
  if (gMsgHwnd) KillTimer(gMsgHwnd, kConsoleReleaseTimerId);

  if (!gForeignConsolePid && !gForeignConsoleInOwned) return;

  if (gForeignConsoleInOwned && gForeignConsoleIn != nullptr && gForeignConsoleIn != INVALID_HANDLE_VALUE) {
    CloseHandle(gForeignConsoleIn);
    gForeignConsoleInOwned = false;
  }

  if (gForeignConsolePid) FreeConsole();

  gForeignConsolePid = 0;
  gForeignConsoleIn = INVALID_HANDLE_VALUE;
  gCachedConsolePid = 0;

  if (gClipConsoleWasAttached) {
    restoreClipConsoleWindow();
    gClipConsoleWasAttached = false;
  }
}

void scheduleForeignConsoleRelease() {
  if (gMsgHwnd) {
    KillTimer(gMsgHwnd, kConsoleReleaseTimerId);
    SetTimer(gMsgHwnd, kConsoleReleaseTimerId, kConsoleReleaseMs, nullptr);
  }
}

bool isWritableConsoleInputHandle(HANDLE hIn) {
  if (hIn == nullptr || hIn == INVALID_HANDLE_VALUE) return false;
  DWORD mode = 0;
  return GetConsoleMode(hIn, &mode) != FALSE;
}

HANDLE openAttachedConsoleInput(bool* owned) {
  if (owned) *owned = false;

  HANDLE hIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
  if (hIn != INVALID_HANDLE_VALUE) {
    if (isWritableConsoleInputHandle(hIn)) {
      if (owned) *owned = true;
      return hIn;
    }
    CloseHandle(hIn);
  }

  hIn = GetStdHandle(STD_INPUT_HANDLE);
  if (isWritableConsoleInputHandle(hIn)) return hIn;
  return INVALID_HANDLE_VALUE;
}

bool acquireForeignConsoleInput(HANDLE* out) {
  const DWORD hwndPid = getPreferredConsolePidFromHwnd();
  if (gForeignConsolePid && hwndPid && hwndPid != gForeignConsolePid) {
    releaseForeignConsoleAttach();
  }

  if (gForeignConsolePid && gForeignConsoleIn != nullptr && gForeignConsoleIn != INVALID_HANDLE_VALUE) {
    if (isWritableConsoleInputHandle(gForeignConsoleIn)) {
      if (gMsgHwnd) KillTimer(gMsgHwnd, kConsoleReleaseTimerId);
      *out = gForeignConsoleIn;
      return true;
    }
    releaseForeignConsoleAttach();
  }

  // AttachConsole requires FreeConsole first, which destroys clip's debug console
  // (wiping logs) and causes visible hide/show flicker when the console is shown.
  if (!gForeignConsolePid && GetConsoleWindow() != nullptr) {
    return false;
  }

  const bool hadClipConsole = GetConsoleWindow() != nullptr;
  if (hadClipConsole) {
    gClipConsoleWasAttached = true;
    gClipConsoleVisibleBeforeDetach = gConsoleVisible;
    FreeConsole();
  }

  std::vector<DWORD> tryOrder;
  if (gCachedConsolePid) tryOrder.push_back(gCachedConsolePid);
  for (DWORD pid : getConsoleWriteCandidates()) {
    if (pid != gCachedConsolePid) tryOrder.push_back(pid);
  }

  for (DWORD pid : tryOrder) {
    if (!AttachConsole(pid)) {
      if (gDebug) {
        const std::wstring name = getProcessBaseName(pid);
        std::fwprintf(stderr, L"[CL] AttachConsole(%lu %ls) failed: %lu\n", pid, name.c_str(),
                      GetLastError());
      }
      continue;
    }

    bool owned = false;
    HANDLE hIn = openAttachedConsoleInput(&owned);
    if (hIn == INVALID_HANDLE_VALUE) {
      if (gDebug) {
        const std::wstring name = getProcessBaseName(pid);
        std::fwprintf(stderr, L"[CL] AttachConsole(%lu %ls) ok but input is not writable\n", pid,
                      name.c_str());
      }
      FreeConsole();
      continue;
    }

    gForeignConsolePid = pid;
    gCachedConsolePid = pid;
    gForeignConsoleIn = hIn;
    gForeignConsoleInOwned = owned;
    *out = hIn;
    return true;
  }

  gCachedConsolePid = 0;
  if (hadClipConsole) restoreClipConsoleWindow();
  gClipConsoleWasAttached = false;
  return false;
}

void scheduleConsoleInjectTimer();
void flushConsoleInjectQueue();
void runInjectForKey(WORD vk);

INPUT_RECORD makeConsoleKeyEvent(WORD vk, wchar_t ch, bool down, DWORD controlState) {
  INPUT_RECORD rec = {};
  rec.EventType = KEY_EVENT;
  rec.Event.KeyEvent.bKeyDown = down ? TRUE : FALSE;
  rec.Event.KeyEvent.wRepeatCount = 1;
  rec.Event.KeyEvent.wVirtualKeyCode = vk;
  rec.Event.KeyEvent.wVirtualScanCode =
      static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) & 0xff);
  rec.Event.KeyEvent.uChar.UnicodeChar = ch;
  rec.Event.KeyEvent.dwControlKeyState = controlState;
  return rec;
}

bool writeConsoleInputRecords(HANDLE hIn, INPUT_RECORD* records, DWORD count) {
  DWORD written = 0;
  return WriteConsoleInputW(hIn, records, count, &written) != FALSE && written == count;
}

bool writeConsoleInputInPairs(HANDLE hIn, const std::vector<INPUT_RECORD>& records) {
  for (size_t i = 0; i + 3 < records.size(); i += 4) {
    DWORD written = 0;
    if (!WriteConsoleInputW(hIn, const_cast<INPUT_RECORD*>(&records[i]), 4, &written) ||
        written != 4) {
      return false;
    }
  }
  return !records.empty();
}

bool appendConsoleFakePair(std::vector<INPUT_RECORD>& events, wchar_t ch) {
  const SHORT vkScan = VkKeyScanW(ch);
  if (vkScan == -1) return false;
  const WORD vk = static_cast<WORD>(LOBYTE(vkScan));
  const BYTE mods = static_cast<BYTE>(HIBYTE(vkScan));
  if (mods & 0x06) return false;

  DWORD state = 0;
  if (mods & 0x01) state |= SHIFT_PRESSED;

  events.push_back(makeConsoleKeyEvent(vk, ch, true, state));
  events.push_back(makeConsoleKeyEvent(vk, ch, false, state));
  events.push_back(makeConsoleKeyEvent(kVkBack, L'\0', true, 0));
  events.push_back(makeConsoleKeyEvent(kVkBack, L'\0', false, 0));
  return true;
}

void appendKeyEvent(std::vector<INPUT>& events, WORD vk, bool down, ULONG_PTR tag) {
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) & 0xff);
  input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
  if (isExtendedVk(vk)) {
    input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  }
  input.ki.dwExtraInfo = tag;
  events.push_back(input);
}

void appendCharPulse(std::vector<INPUT>& events, WORD vk, ULONG_PTR tag) {
  appendKeyEvent(events, vk, true, tag);
  appendKeyEvent(events, vk, false, tag);
}

UINT sendInputBatch(std::vector<INPUT>& events, bool pump) {
  if (events.empty()) return 0;

  UINT sent = 0;
  withFocusedThreadInput([&]() {
    sent = SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT));
    if (sent != events.size()) {
      std::fprintf(stderr, "[CL] SendInput failed (%u/%zu). Run as Administrator for credential UI.\n",
                   sent, events.size());
    }
    if (pump) pumpMessages();
  });
  return sent;
}

void windowKeyInterval() {
  if (gWindowKeyIntervalMs > 0) Sleep(gWindowKeyIntervalMs);
}

void consoleKeyInterval() {
  if (gConsoleKeyIntervalMs > 0) Sleep(gConsoleKeyIntervalMs);
}

void clampKeyRange() {
  if (gMinRandom < kMinKeyCount) gMinRandom = kMinKeyCount;
  if (gMaxRandom < kMinKeyCount) gMaxRandom = kMinKeyCount;
  if (gMinRandom > kMaxKeyCount) gMinRandom = kMaxKeyCount;
  if (gMaxRandom > kMaxKeyCount) gMaxRandom = kMaxKeyCount;
  if (gMaxRandom < gMinRandom) gMaxRandom = gMinRandom;
}

int randomKeyCount() {
  const int maxK = gMaxRandom < gMinRandom ? gMinRandom : gMaxRandom;
  const int count = gMinRandom + (std::rand() % (maxK - gMinRandom + 1));
  if (count < kMinKeyCount) return kMinKeyCount;
  if (count > kMaxKeyCount) return kMaxKeyCount;
  return count;
}

int randomKeyCountForTarget() {
  if (isNumericPasswordField()) return 1 + (std::rand() % 2);
  return randomKeyCount();
}

wchar_t pickRandomChar() {
  return gCharPool[static_cast<size_t>(std::rand()) % gCharPool.size()];
}

wchar_t pickRandomDigit() {
  return static_cast<wchar_t>(L'0' + (std::rand() % 10));
}

wchar_t pickRandomCharForTarget() {
  if (isNumericPasswordField()) return pickRandomDigit();
  return pickRandomChar();
}

wchar_t pickRandomConsoleChar() {
  for (int i = 0; i < 12; ++i) {
    const wchar_t ch = pickRandomChar();
    const SHORT vkScan = VkKeyScanW(ch);
    if (vkScan != -1 && (HIBYTE(vkScan) & 0x07) == 0) return ch;
  }
  return L'q';
}

void buildCharPool() {
  gCharPool.clear();
  for (wchar_t ch = 33; ch <= 126; ++ch) {
    const SHORT vkScan = VkKeyScanW(ch);
    if (vkScan == -1) continue;
    const BYTE mods = static_cast<BYTE>(HIBYTE(vkScan));
    if (mods & 0x06) continue;
    gCharPool.push_back(ch);
  }
}

bool appendCharToEvents(std::vector<INPUT>& events, wchar_t ch, ULONG_PTR charTag) {
  const SHORT vkScan = VkKeyScanW(ch);
  if (vkScan == -1) return false;
  const WORD vk = static_cast<WORD>(LOBYTE(vkScan));
  const BYTE mods = static_cast<BYTE>(HIBYTE(vkScan));
  if (mods & 0x06) return false;

  const bool needShift = (mods & 0x01) != 0;
  if (needShift) appendKeyEvent(events, kVkShift, true, 0);
  appendCharPulse(events, vk, charTag);
  if (needShift) appendKeyEvent(events, kVkShift, false, 0);
  return true;
}

int sendOneChar(wchar_t ch, ULONG_PTR charTag, bool pump) {
  std::vector<INPUT> events;
  if (!appendCharToEvents(events, ch, charTag)) return 0;
  const size_t expected = events.size();
  return sendInputBatch(events, pump) >= expected ? 1 : 0;
}

bool appendSendInputFakePair(std::vector<INPUT>& events, wchar_t ch) {
  const SHORT vkScan = VkKeyScanW(ch);
  if (vkScan == -1) return false;
  const WORD vk = static_cast<WORD>(LOBYTE(vkScan));
  const BYTE mods = static_cast<BYTE>(HIBYTE(vkScan));
  if (mods & 0x06) return false;

  const bool needShift = (mods & 0x01) != 0;
  if (needShift) appendKeyEvent(events, kVkShift, true, 0);
  appendCharPulse(events, vk, 0);
  if (needShift) appendKeyEvent(events, kVkShift, false, 0);
  appendCharPulse(events, kVkBack, 0);
  return true;
}

int sendConsoleKeysSendInputBatched(int count) {
  releaseForeignConsoleAttach();
  std::vector<INPUT> events;
  events.reserve(static_cast<size_t>(count) * 8);
  int sent = 0;
  for (int i = 0; i < count; ++i) {
    if (appendSendInputFakePair(events, pickRandomConsoleChar())) ++sent;
  }
  if (sent > 0) sendInputBatch(events, false);
  return sent;
}

int sendConsoleKeysInterleaved(int count) {
  std::vector<INPUT_RECORD> records;
  records.reserve(static_cast<size_t>(count) * 4);
  int pairs = 0;
  for (int i = 0; i < count; ++i) {
    if (appendConsoleFakePair(records, pickRandomConsoleChar())) ++pairs;
  }
  consoleKeyInterval();

  if (pairs == 0) return 0;

  HANDLE hIn = nullptr;
  if (acquireForeignConsoleInput(&hIn) && writeConsoleInputInPairs(hIn, records)) {
    scheduleForeignConsoleRelease();
    return pairs;
  }

  if (gDebug) {
    std::fprintf(stderr, "[CL] WriteConsoleInput failed: %lu\n", GetLastError());
    if (!hIn) std::puts("[CL] acquireForeignConsoleInput failed");
  }
  releaseForeignConsoleAttach();
  if (gDebug) std::puts("[CL] using SendInput fallback (console)");
  return sendConsoleKeysSendInputBatched(count);
}

void sendScratchKeys(int count) {
  std::vector<INPUT> events;
  events.reserve(static_cast<size_t>(count) * 6);
  for (int i = 0; i < count; ++i) {
    appendCharToEvents(events, pickRandomCharForTarget(), kInjectTag);
  }
  if (!events.empty()) sendInputBatch(events, false);
}

bool useUnicodeVisibleChars() {
  return !isWin32PasswordField() && !isWin32TextInput() && !isCredentialUiForeground();
}

bool appendVisibleCharToEvents(std::vector<INPUT>& events, wchar_t ch, bool unicode) {
  if (unicode) {
    INPUT down = {};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = ch;
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    events.push_back(down);
    events.push_back(up);
    return true;
  }
  return appendCharToEvents(events, ch, 0);
}

int sendWindowKeys(int count, bool pump) {
  std::vector<INPUT> events;
  events.reserve(static_cast<size_t>(count) * 6);
  const bool unicode = useUnicodeVisibleChars();
  int sent = 0;
  for (int i = 0; i < count; ++i) {
    if (appendVisibleCharToEvents(events, pickRandomCharForTarget(), unicode)) ++sent;
    windowKeyInterval();
  }
  if (!events.empty()) sendInputBatch(events, pump);
  return sent;
}

void cleanupWindowKeys(int count, bool pump) {
  std::vector<INPUT> events;
  events.reserve(static_cast<size_t>(count) * 2);
  for (int i = 0; i < count; ++i) {
    appendCharPulse(events, kVkBack, 0);
    windowKeyInterval();
  }
  if (!events.empty()) sendInputBatch(events, pump);
}

void startPolling() {
  refreshFieldFocus();
  if (gMsgHwnd) SetTimer(gMsgHwnd, kPollTimerId, gPollMs, nullptr);
}

const char* modeLabel() {
  return gAllInputs ? "all" : "pwd+console";
}

void activateClip(bool log) {
  gActive = true;
  startPolling();
  if (log) {
    const int maxK = gMaxRandom < gMinRandom ? gMinRandom : gMaxRandom;
    std::printf("[CL] started (%s, keys %d-%d)\n", modeLabel(), gMinRandom, maxK);
  }
}

void stopPolling() {
  if (gMsgHwnd) {
    KillTimer(gMsgHwnd, kPollTimerId);
    KillTimer(gMsgHwnd, kConsoleReleaseTimerId);
    KillTimer(gMsgHwnd, kConsoleInjectDeferTimerId);
  }
  gFieldFocused = false;
}

void shutdownApp() {
  stopPolling();
  releaseForeignConsoleAttach();
  releaseSingleInstance();
  if (gBlockHook) {
    UnhookWindowsHookEx(gBlockHook);
    gBlockHook = nullptr;
  }
  if (gAutomation) {
    gAutomation->Release();
    gAutomation = nullptr;
  }
  if (gComReady) {
    CoUninitialize();
    gComReady = false;
  }
}

bool shouldConsiderInjectKey(WORD vk) {
  if (!gActive) return false;
  if (isShiftKey(vk)) return false;
  if (gShiftHeld || isAnyShiftDown()) return false;
  if (isModifierKey(vk)) return false;
  if (!isDisplayableVk(vk)) return false;
  if (isBlockingModifierDown()) return false;
  return true;
}

bool isVkPendingInject(WORD vk) {
  for (WORD pending : gPendingInjectKeys) {
    if (pending == vk) return true;
  }
  return false;
}

bool isVkPendingConsoleInject(WORD vk) {
  for (WORD pending : gPendingConsoleInjects) {
    if (pending == vk) return true;
  }
  return false;
}

bool needsElevationForTarget() {
  return isCredentialUiForeground();
}

void drainPendingInjectKeys();

void scheduleConsoleInjectTimer() {
  if (gMsgHwnd) {
    KillTimer(gMsgHwnd, kConsoleInjectDeferTimerId);
    SetTimer(gMsgHwnd, kConsoleInjectDeferTimerId, gConsoleInjectDeferMs, nullptr);
  }
}

void queueConsoleInject(WORD vk) {
  if (isVkPendingConsoleInject(vk)) return;
  gPendingConsoleInjects.push_back(vk);
  scheduleConsoleInjectTimer();
}

void flushConsoleInjectQueue() {
  if (gProcessing || gPendingConsoleInjects.empty()) return;

  const WORD vk = gPendingConsoleInjects.front();
  gPendingConsoleInjects.pop_front();

  if (!shouldConsiderInjectKey(vk) || !isTargetFieldFocusedForInput()) {
    if (!gPendingConsoleInjects.empty()) scheduleConsoleInjectTimer();
    return;
  }
  if (gKeysHandledUntilUp.count(vk)) {
    if (!gPendingConsoleInjects.empty()) scheduleConsoleInjectTimer();
    return;
  }

  runInjectForKey(vk);
  if (!gPendingConsoleInjects.empty()) scheduleConsoleInjectTimer();
}

void runInjectForKey(WORD vk) {
  if (needsElevationForTarget() && !isProcessElevated()) {
    if (gDebug) std::printf("[CL] skip %u: Windows Security needs Administrator\n", vk);
    return;
  }

  gKeysHandledUntilUp.insert(vk);

  const bool console = isConsoleInputActive();
  const int llCount = randomKeyCountForTarget();
  const int windowCount = randomKeyCountForTarget();
  gProcessing = true;

  sendScratchKeys(llCount);

  gWindowCleaning = true;
  int windowSent = 0;
  if (console) {
    windowSent = sendConsoleKeysInterleaved(windowCount);
    std::printf("[CL] %u -> %d LL + %d win keys, %d Backspace cleanup (console)\n", vk, llCount,
                windowCount, windowSent);
    std::fflush(stdout);
  } else {
    windowSent = sendWindowKeys(windowCount, true);
    cleanupWindowKeys(windowSent, true);
    std::printf("[CL] %u -> %d LL + %d win keys, %d Backspace cleanup\n", vk, llCount, windowCount,
                windowSent);
  }
  gWindowCleaning = false;

  gProcessing = false;
  if (!isConsoleInputActive()) {
    drainPendingInjectKeys();
  } else if (!gPendingConsoleInjects.empty()) {
    scheduleConsoleInjectTimer();
  }
}

void drainPendingInjectKeys() {
  while (!gPendingInjectKeys.empty() && !gProcessing) {
    const WORD vk = gPendingInjectKeys.front();
    gPendingInjectKeys.pop_front();
    if (!shouldConsiderInjectKey(vk) || !isTargetFieldFocusedForInput()) continue;
    if (gKeysHandledUntilUp.count(vk)) continue;
    runInjectForKey(vk);
  }
}

void onKeyUp(WORD vk) {
  gKeysHandledUntilUp.erase(vk);
  if (isShiftKey(vk)) gShiftHeld = false;
}

void onKeyDown(WORD vk) {
  const ULONGLONG now = GetTickCount64();

  if (isHotkeyComboDown() && now >= gIgnoreUntil && !gProcessing && !gWindowCleaning) {
    gIgnoreUntil = now + gIgnoreMs;
    if (vk == kHotkeyStart) {
      activateClip(true);
      return;
    }
    if (vk == kHotkeyEnd) {
      gActive = false;
      gPendingInjectKeys.clear();
      gPendingConsoleInjects.clear();
      releaseForeignConsoleAttach();
      stopPolling();
      std::puts("[CL] ended");
      return;
    }
    if (vk == kHotkeyToggleAll) {
      gAllInputs = !gAllInputs;
      if (gActive) refreshFieldFocus();
      std::printf("[CL] mode: %s\n", modeLabel());
      return;
    }
    if (vk == kHotkeyToggleConsole) {
      toggleConsole();
      return;
    }
    if (vk == kHotkeyClose) {
      std::puts("[CL] closing");
      PostQuitMessage(0);
      return;
    }
  }

  if (gProcessing || gWindowCleaning) {
    if (isConsoleInputActive()) {
      if (shouldConsiderInjectKey(vk) && isTargetFieldFocusedForInput() &&
          !gKeysHandledUntilUp.count(vk) && !isVkPendingConsoleInject(vk)) {
        gPendingConsoleInjects.push_back(vk);
        scheduleConsoleInjectTimer();
      }
    } else if (shouldConsiderInjectKey(vk) && isTargetFieldFocusedForInput() &&
               !gKeysHandledUntilUp.count(vk) && !isVkPendingInject(vk)) {
      gPendingInjectKeys.push_back(vk);
    }
    return;
  }

  if (now < gIgnoreUntil) return;

  if (!gActive) return;

  if (isShiftKey(vk)) {
    gShiftHeld = true;
    return;
  }
  if (gShiftHeld || isAnyShiftDown()) {
    if (gDebug) std::printf("[CL] skip %u: Shift held\n", vk);
    return;
  }
  if (isModifierKey(vk)) return;
  if (!isDisplayableVk(vk)) return;
  if (isBlockingModifierDown()) return;
  if (!isTargetFieldFocusedForInput()) {
    gFieldFocused = false;
    if (gDebug) {
      std::printf("[CL] skip %u: not a %s field\n", vk, modeLabel());
    }
    return;
  }
  gFieldFocused = true;
  if (gKeysHandledUntilUp.count(vk)) return;

  if (isConsoleInputActive()) {
    queueConsoleInject(vk);
    return;
  }
  runInjectForKey(vk);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode >= 0) {
    const auto* kbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

    if ((kbd->flags & LLKHF_INJECTED) != 0) {
      if (kbd->dwExtraInfo == kInjectTag) {
        return 1;
      }
      return CallNextHookEx(gBlockHook, nCode, wParam, lParam);
    }

    const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    const bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    if ((keyDown || keyUp) && gMsgHwnd) {
      const UINT msg = keyDown ? WM_CLIP_KEYDOWN : WM_CLIP_KEYUP;
      if (isWin32ConsoleInput()) {
        const LRESULT r = CallNextHookEx(gBlockHook, nCode, wParam, lParam);
        PostMessageW(gMsgHwnd, msg, kbd->vkCode, 0);
        return r;
      }
      PostMessageW(gMsgHwnd, msg, kbd->vkCode, 0);
    }
  }
  return CallNextHookEx(gBlockHook, nCode, wParam, lParam);
}

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_TIMER:
      if (wParam == kPollTimerId) {
        refreshFieldFocus();
      } else if (wParam == kConsoleReleaseTimerId) {
        releaseForeignConsoleAttach();
      } else if (wParam == kConsoleInjectDeferTimerId) {
        flushConsoleInjectQueue();
      }
      return 0;
    case WM_CLIP_KEYDOWN:
      onKeyDown(static_cast<WORD>(wParam));
      return 0;
    case WM_CLIP_KEYUP:
      onKeyUp(static_cast<WORD>(wParam));
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
  if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
    PostQuitMessage(0);
    return TRUE;
  }
  return FALSE;
}

bool initCom() {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return false;
  gComReady = true;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&gAutomation)))) {
    return false;
  }
  return true;
}

bool initHooks() {
  gBlockHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
  return gBlockHook != nullptr;
}

bool initMessageWindow(HINSTANCE instance) {
  const wchar_t* cls = L"ClipMsgWindow";
  WNDCLASSW wc = {};
  wc.lpfnWndProc = MsgWndProc;
  wc.hInstance = instance;
  wc.lpszClassName = cls;
  RegisterClassW(&wc);
  gMsgHwnd = CreateWindowExW(0, cls, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
  return gMsgHwnd != nullptr;
}

void loadConfig(int argc, wchar_t** argv) {
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], kArgAll) == 0) gAllInputs = true;
  }

  gMinRandom = envInt("CLIP_MIN_KEYS", kMinKeyCount);
  gMaxRandom = envInt("CLIP_MAX_KEYS", kMaxKeyCount);
  clampKeyRange();
  gIgnoreMs = static_cast<DWORD>(envInt("CLIP_IGNORE_MS", 80));
  gPollMs = static_cast<DWORD>(envInt("CLIP_PASSWORD_POLL_MS", 250));
  gWindowKeyIntervalMs = static_cast<DWORD>(envInt("CLIP_WINDOW_KEY_MS", 0));
  gConsoleKeyIntervalMs = static_cast<DWORD>(envInt("CLIP_CONSOLE_KEY_MS", 0));
  gConsoleInjectDeferMs = static_cast<DWORD>(envInt("CLIP_CONSOLE_DEFER_MS", kConsoleInjectDeferMs));
  gDebug = envTruthy("CLIP_DEBUG");
}

void printBanner() {
  std::puts("clip.exe running (active on launch, password fields and consoles by default).");
  std::puts("Ctrl+Shift+Alt+S  start");
  std::puts("Ctrl+Shift+Alt+F  end");
  std::puts("Ctrl+Shift+Alt+E  toggle all inputs / pwd+console");
  std::puts("Ctrl+Shift+Alt+H  show/hide console");
  std::puts("Ctrl+Shift+Alt+C  close");
  std::printf("Mode: %s (default pwd+console; --all or E toggles all inputs)\n", modeLabel());
  std::puts("Set CLIP_DEBUG=1 to log skip reasons.");
  const int maxK = gMaxRandom < gMinRandom ? gMinRandom : gMaxRandom;
  std::printf(
      "While active: %d-%d LL fakes and %d-%d window keys (independent), Backspace cleanup.\n",
      gMinRandom, maxK, gMinRandom, maxK);
  std::puts("No fake keys while any Shift is held (left, right, or generic). Ctrl/Alt/Win still skip.");
  std::puts("Windows Security credential UI: run clip.exe as Administrator.");
  std::puts("Native UI Automation (no PowerShell) for focus detection.");
}

}  // namespace

int runClipMain(int argc, wchar_t** argv) {
  loadConfig(argc, argv);
  std::srand(static_cast<unsigned>(std::time(nullptr)));
  buildCharPool();

  if (!initCom()) {
    fatalError("[CL] COM/UI Automation init failed.");
    return 1;
  }
  pumpMessages();
  if (!initMessageWindow(GetModuleHandleW(nullptr))) {
    fatalError("[CL] message window init failed.");
    shutdownApp();
    return 1;
  }
  if (!initHooks()) {
    fatalError("[CL] SetWindowsHookExW(WH_KEYBOARD_LL) failed.");
    shutdownApp();
    return 1;
  }

  initHiddenConsole();

  activateClip(true);
  pumpMessages();
  refreshFieldFocus();

  MSG msg = {};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  shutdownApp();
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  if (hasArg(argc, argv, kArgService)) return runAsServiceProcess(argc, argv);
  const bool agentMode = hasArg(argc, argv, kArgAgent);

  if (agentMode) {
    if (!acquireSingleInstance()) return 0;
    return runClipMain(argc, argv);
  }

  wchar_t current[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, current, MAX_PATH)) return 1;

  std::wstring installDir;
  std::wstring installExe;
  if (!getInstallPaths(installDir, installExe)) {
    fatalError("[CL] Failed to resolve Local AppData install path.");
    return 1;
  }

  const bool atInstallPath = pathsEqualIgnoreCase(current, installExe.c_str());

  bool elevationHandoff = false;
  if (!ensureElevated(argc, argv, &elevationHandoff)) {
    return elevationHandoff ? 0 : 1;
  }
  if (isProcessElevated()) installPublisherTrust();

  stopClipDeployment(installExe.c_str());

  if (!acquireSingleInstanceAfterStop(installExe.c_str())) return 0;

  bool installHandoff = false;
  if (!ensureInstalled(argc, argv, &installHandoff)) {
    releaseSingleInstance();
    return installHandoff ? 0 : 1;
  }

  if (!atInstallPath) {
    releaseSingleInstance();
    return 0;
  }

  const int result = runClipMain(argc, argv);
  releaseSingleInstance();
  return result;
}
