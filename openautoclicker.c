#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

// Control IDs
#define ID_EDIT_HOURS       101
#define ID_EDIT_MINS        102
#define ID_EDIT_SECS        103
#define ID_EDIT_MS          104
#define ID_CHK_RANDOM       105
#define ID_EDIT_RANDOM      106
#define ID_BTN_HELP         107

#define ID_COMBO_BTN        201
#define ID_COMBO_TYPE       202

#define ID_RADIO_REPEAT_N   301
#define ID_EDIT_REPEAT_N    302
#define ID_SPIN_REPEAT_N    303
#define ID_RADIO_UNTIL_STOP 304

#define ID_RADIO_CURR_LOC   401
#define ID_RADIO_PICK_LOC   402
#define ID_BTN_PICK_LOC     403
#define ID_EDIT_X           404
#define ID_EDIT_Y           405

#define ID_BTN_START        501
#define ID_BTN_STOP         502
#define ID_BTN_HOTKEY       503
#define ID_BTN_RECORD       504

#define HOTKEY_ID           1
#define WM_UPDATE_STATE     (WM_USER + 1)

// Global State
HWND hMainWnd;
HWND hEditHours, hEditMins, hEditSecs, hEditMs;
HWND hChkRandom, hEditRandom;
HWND hComboBtn, hComboType;
HWND hRadioRepeatN, hEditRepeatN, hSpinRepeatN, hRadioUntilStop;
HWND hRadioCurrLoc, hRadioPickLoc, hBtnPickLoc, hEditX, hEditY;
HWND hBtnStart, hBtnStop, hBtnHotkey, hBtnRecord;

volatile bool isRunning = false;
volatile bool isPicking = false;
volatile bool isWaitingHotkey = false;
UINT currentHotkey = VK_F6;
HANDLE hClickThread = NULL;
HHOOK hMouseHook = NULL;
HFONT hDefaultFont;

int GetEditInt(HWND hEdit) {
    char buf[32];
    GetWindowTextA(hEdit, buf, sizeof(buf));
    return atoi(buf);
}

void GetKeyNameStr(UINT vk, char* out, size_t size) {
    UINT scanCode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    switch (vk) {
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            scanCode |= 0x100;
            break;
    }
    if (GetKeyNameTextA(scanCode << 16, out, (int)size) == 0) {
        snprintf(out, size, "VK_%u", vk);
    }
}

void UpdateHotkeyLabels() {
    char keyName[32];
    char buf[64];
    GetKeyNameStr(currentHotkey, keyName, sizeof(keyName));
    
    snprintf(buf, sizeof(buf), "Start (%s)", keyName);
    SetWindowTextA(hBtnStart, buf);
    
    snprintf(buf, sizeof(buf), "Stop (%s)", keyName);
    SetWindowTextA(hBtnStop, buf);
}

void SetRunningState(bool running) {
    isRunning = running;
    EnableWindow(hBtnStart, !running);
    EnableWindow(hBtnStop, running);
    EnableWindow(hEditHours, !running);
    EnableWindow(hEditMins, !running);
    EnableWindow(hEditSecs, !running);
    EnableWindow(hEditMs, !running);
    EnableWindow(hChkRandom, !running);
    EnableWindow(hEditRandom, !running);
    EnableWindow(hComboBtn, !running);
    EnableWindow(hComboType, !running);
    EnableWindow(hRadioRepeatN, !running);
    EnableWindow(hEditRepeatN, !running);
    EnableWindow(hSpinRepeatN, !running);
    EnableWindow(hRadioUntilStop, !running);
    EnableWindow(hRadioCurrLoc, !running);
    EnableWindow(hRadioPickLoc, !running);
    EnableWindow(hBtnPickLoc, !running);
    EnableWindow(hEditX, !running);
    EnableWindow(hEditY, !running);
}

void PerformClick(int buttonIdx, int typeIdx, bool fixedLoc, int targetX, int targetY) {
    if (fixedLoc) {
        SetCursorPos(targetX, targetY);
    }

    DWORD downFlag = MOUSEEVENTF_LEFTDOWN;
    DWORD upFlag   = MOUSEEVENTF_LEFTUP;

    if (buttonIdx == 1) { // Right
        downFlag = MOUSEEVENTF_RIGHTDOWN;
        upFlag   = MOUSEEVENTF_RIGHTUP;
    } else if (buttonIdx == 2) { // Middle
        downFlag = MOUSEEVENTF_MIDDLEDOWN;
        upFlag   = MOUSEEVENTF_MIDDLEUP;
    }

    int clicks = (typeIdx == 1) ? 2 : 1;
    INPUT inputs[4] = {0};
    int count = 0;

    for (int i = 0; i < clicks; i++) {
        inputs[count].type = INPUT_MOUSE;
        inputs[count].mi.dwFlags = downFlag;
        count++;

        inputs[count].type = INPUT_MOUSE;
        inputs[count].mi.dwFlags = upFlag;
        count++;
    }

    SendInput(count, inputs, sizeof(INPUT));
}

void InterruptibleSleep(int totalMs) {
    if (totalMs <= 0) return;
    DWORD start = GetTickCount();
    while (isRunning) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= (DWORD)totalMs) break;
        DWORD rem = totalMs - elapsed;
        Sleep(rem > 10 ? 10 : 1);
    }
}

DWORD WINAPI ClickWorker(LPVOID lpParam) {
    int hours = GetEditInt(hEditHours);
    int mins  = GetEditInt(hEditMins);
    int secs  = GetEditInt(hEditSecs);
    int ms    = GetEditInt(hEditMs);

    long long baseInterval = ((long long)hours * 3600000LL) +
                             ((long long)mins  * 60000LL) +
                             ((long long)secs  * 1000LL) +
                             (long long)ms;
    if (baseInterval < 1) baseInterval = 1;

    bool useRandom = (SendMessage(hChkRandom, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int randOffset = GetEditInt(hEditRandom);
    if (randOffset < 0) randOffset = 0;

    int btnIdx  = (int)SendMessage(hComboBtn, CB_GETCURSEL, 0, 0);
    int typeIdx = (int)SendMessage(hComboType, CB_GETCURSEL, 0, 0);

    bool infinite = (SendMessage(hRadioUntilStop, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int maxRepeats = GetEditInt(hEditRepeatN);
    if (maxRepeats < 1) maxRepeats = 1;

    bool fixedLoc = (SendMessage(hRadioPickLoc, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int targetX = GetEditInt(hEditX);
    int targetY = GetEditInt(hEditY);

    int count = 0;
    while (isRunning) {
        PerformClick(btnIdx, typeIdx, fixedLoc, targetX, targetY);
        count++;

        if (!infinite && count >= maxRepeats) {
            break;
        }

        int sleepMs = (int)baseInterval;
        if (useRandom && randOffset > 0) {
            int delta = (rand() % (2 * randOffset + 1)) - randOffset;
            sleepMs += delta;
            if (sleepMs < 1) sleepMs = 1;
        }

        InterruptibleSleep(sleepMs);
    }

    PostMessage(hMainWnd, WM_UPDATE_STATE, 0, 0);
    return 0;
}

void StartClicking() {
    if (isRunning) return;
    SetRunningState(true);
    hClickThread = CreateThread(NULL, 0, ClickWorker, NULL, 0, NULL);
    if (hClickThread) CloseHandle(hClickThread);
}

void StopClicking() {
    if (!isRunning) return;
    isRunning = false;
    SetRunningState(false);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && isPicking) {
        if (wParam == WM_LBUTTONDOWN) {
            MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;
            char buf[16];
            snprintf(buf, sizeof(buf), "%ld", pMouse->pt.x);
            SetWindowTextA(hEditX, buf);
            snprintf(buf, sizeof(buf), "%ld", pMouse->pt.y);
            SetWindowTextA(hEditY, buf);

            SendMessage(hRadioCurrLoc, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessage(hRadioPickLoc, BM_SETCHECK, BST_CHECKED, 0);

            isPicking = false;
            SetWindowTextA(hBtnPickLoc, "Pick location");
            if (hMouseHook) {
                UnhookWindowsHookEx(hMouseHook);
                hMouseHook = NULL;
            }
            ShowWindow(hMainWnd, SW_RESTORE);
            SetForegroundWindow(hMainWnd);
            return 1;
        }
    }
    return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
}

HWND MakeCtrl(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND hwnd = CreateWindowA(cls, text, WS_VISIBLE | WS_CHILD | style, x, y, w, h, hMainWnd, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
    SendMessage(hwnd, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);
    return hwnd;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            hMainWnd = hwnd;
            hDefaultFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            MakeCtrl("BUTTON", "Click interval", BS_GROUPBOX, 10, 8, 435, 82, 0);
            
            hEditHours = MakeCtrl("EDIT", "0", WS_BORDER | ES_NUMBER | ES_RIGHT, 22, 30, 42, 20, ID_EDIT_HOURS);
            MakeCtrl("STATIC", "hours", 0, 68, 33, 35, 16, 0);

            hEditMins = MakeCtrl("EDIT", "0", WS_BORDER | ES_NUMBER | ES_RIGHT, 112, 30, 42, 20, ID_EDIT_MINS);
            MakeCtrl("STATIC", "mins", 0, 158, 33, 30, 16, 0);

            hEditSecs = MakeCtrl("EDIT", "0", WS_BORDER | ES_NUMBER | ES_RIGHT, 198, 30, 42, 20, ID_EDIT_SECS);
            MakeCtrl("STATIC", "secs", 0, 244, 33, 30, 16, 0);

            hEditMs = MakeCtrl("EDIT", "100", WS_BORDER | ES_NUMBER | ES_RIGHT, 284, 30, 48, 20, ID_EDIT_MS);
            MakeCtrl("STATIC", "milliseconds", 0, 336, 33, 70, 16, 0);

            MakeCtrl("BUTTON", "?", BS_PUSHBUTTON, 22, 58, 20, 20, ID_BTN_HELP);
            hChkRandom = MakeCtrl("BUTTON", "Random offset + -", BS_AUTOCHECKBOX, 50, 60, 115, 18, ID_CHK_RANDOM);
            hEditRandom = MakeCtrl("EDIT", "40", WS_BORDER | ES_NUMBER | ES_RIGHT, 172, 58, 55, 20, ID_EDIT_RANDOM);
            MakeCtrl("STATIC", "milliseconds", 0, 232, 61, 70, 16, 0);

            MakeCtrl("BUTTON", "Click options", BS_GROUPBOX, 10, 96, 212, 82, 0);
            
            MakeCtrl("STATIC", "Mouse button:", 0, 22, 120, 75, 16, 0);
            hComboBtn = MakeCtrl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 105, 116, 102, 100, ID_COMBO_BTN);
            SendMessageA(hComboBtn, CB_ADDSTRING, 0, (LPARAM)"Left");
            SendMessageA(hComboBtn, CB_ADDSTRING, 0, (LPARAM)"Right");
            SendMessageA(hComboBtn, CB_ADDSTRING, 0, (LPARAM)"Middle");
            SendMessageA(hComboBtn, CB_SETCURSEL, 0, 0);

            MakeCtrl("STATIC", "Click type:", 0, 22, 148, 75, 16, 0);
            hComboType = MakeCtrl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 105, 144, 102, 100, ID_COMBO_TYPE);
            SendMessageA(hComboType, CB_ADDSTRING, 0, (LPARAM)"Single");
            SendMessageA(hComboType, CB_ADDSTRING, 0, (LPARAM)"Double");
            SendMessageA(hComboType, CB_SETCURSEL, 0, 0);

            MakeCtrl("BUTTON", "Click repeat", BS_GROUPBOX, 230, 96, 215, 82, 0);

            hRadioRepeatN = MakeCtrl("BUTTON", "Repeat", BS_AUTORADIOBUTTON | WS_GROUP, 242, 118, 60, 18, ID_RADIO_REPEAT_N);
            hEditRepeatN = MakeCtrl("EDIT", "1", WS_BORDER | ES_NUMBER | ES_RIGHT, 305, 117, 45, 20, ID_EDIT_REPEAT_N);
            hSpinRepeatN = CreateWindowA(UPDOWN_CLASSA, NULL, WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                                         0, 0, 0, 0, hwnd, (HMENU)ID_SPIN_REPEAT_N, GetModuleHandle(NULL), NULL);
            SendMessage(hSpinRepeatN, UDM_SETBUDDY, (WPARAM)hEditRepeatN, 0);
            SendMessage(hSpinRepeatN, UDM_SETRANGE32, 1, 999999);
            MakeCtrl("STATIC", "times", 0, 368, 120, 40, 16, 0);

            hRadioUntilStop = MakeCtrl("BUTTON", "Repeat until stopped", BS_AUTORADIOBUTTON, 242, 146, 140, 18, ID_RADIO_UNTIL_STOP);
            SendMessage(hRadioUntilStop, BM_SETCHECK, BST_CHECKED, 0);

            MakeCtrl("BUTTON", "Cursor position", BS_GROUPBOX, 10, 184, 435, 52, 0);

            hRadioCurrLoc = MakeCtrl("BUTTON", "Current location", BS_AUTORADIOBUTTON | WS_GROUP, 22, 206, 105, 18, ID_RADIO_CURR_LOC);
            SendMessage(hRadioCurrLoc, BM_SETCHECK, BST_CHECKED, 0);

            hRadioPickLoc = MakeCtrl("BUTTON", "", BS_AUTORADIOBUTTON, 142, 206, 18, 18, ID_RADIO_PICK_LOC);
            hBtnPickLoc = MakeCtrl("BUTTON", "Pick location", BS_PUSHBUTTON, 162, 203, 95, 24, ID_BTN_PICK_LOC);

            MakeCtrl("STATIC", "X", 0, 270, 207, 12, 16, 0);
            hEditX = MakeCtrl("EDIT", "0", WS_BORDER | ES_RIGHT, 285, 205, 48, 20, ID_EDIT_X);

            MakeCtrl("STATIC", "Y", 0, 345, 207, 12, 16, 0);
            hEditY = MakeCtrl("EDIT", "0", WS_BORDER | ES_RIGHT, 360, 205, 48, 20, ID_EDIT_Y);

            hBtnStart  = MakeCtrl("BUTTON", "Start (F6)", BS_PUSHBUTTON, 10, 246, 212, 34, ID_BTN_START);
            hBtnStop   = MakeCtrl("BUTTON", "Stop (F6)", BS_PUSHBUTTON | WS_DISABLED, 230, 246, 215, 34, ID_BTN_STOP);
            hBtnHotkey = MakeCtrl("BUTTON", "Hotkey setting", BS_PUSHBUTTON, 10, 286, 212, 34, ID_BTN_HOTKEY);
            hBtnRecord = MakeCtrl("BUTTON", "Record & Playback", BS_PUSHBUTTON, 230, 286, 215, 34, ID_BTN_RECORD);

            RegisterHotKey(hwnd, HOTKEY_ID, 0, currentHotkey);
            break;
        }

        case WM_HOTKEY: {
            if (wParam == HOTKEY_ID && !isWaitingHotkey) {
                if (isRunning) StopClicking();
                else StartClicking();
            }
            break;
        }

        case WM_KEYDOWN: {
            if (isWaitingHotkey) {
                UINT newVk = (UINT)wParam;
                if (newVk != VK_ESCAPE) {
                    UnregisterHotKey(hwnd, HOTKEY_ID);
                    currentHotkey = newVk;
                    RegisterHotKey(hwnd, HOTKEY_ID, 0, currentHotkey);
                    UpdateHotkeyLabels();
                }
                isWaitingHotkey = false;
                SetWindowTextA(hBtnHotkey, "Hotkey setting");
                return 0;
            }
            break;
        }

        case WM_UPDATE_STATE: {
            SetRunningState(false);
            break;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            switch (id) {
                case ID_BTN_HELP:
                    MessageBoxA(hwnd, "Adds random delay variance between -X and +X milliseconds.", "Random Offset", MB_OK | MB_ICONINFORMATION);
                    break;
                case ID_BTN_START:
                    StartClicking();
                    break;
                case ID_BTN_STOP:
                    StopClicking();
                    break;
                case ID_BTN_HOTKEY:
                    if (!isWaitingHotkey) {
                        isWaitingHotkey = true;
                        SetWindowTextA(hBtnHotkey, "Press any key...");
                        SetFocus(hwnd);
                    }
                    break;
                case ID_BTN_PICK_LOC:
                    if (!isPicking) {
                        isPicking = true;
                        SetWindowTextA(hBtnPickLoc, "Click anywhere...");
                        hMouseHook = SetWindowsHookExA(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);
                    }
                    break;
                case ID_BTN_RECORD:
                    MessageBoxA(hwnd, "Macro Record & Playback placeholder.", "Record & Playback", MB_OK | MB_ICONINFORMATION);
                    break;
            }
            break;
        }

        case WM_DESTROY:
            isRunning = false;
            UnregisterHotKey(hwnd, HOTKEY_ID);
            if (hMouseHook) UnhookWindowsHookEx(hMouseHook);
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    srand((unsigned int)time(NULL));

    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icex);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = "OpenAutoClickerClass";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);

    RegisterClassA(&wc);

    RECT rc = { 0, 0, 455, 330 };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    HWND hwnd = CreateWindowA(
        "OpenAutoClickerClass",
        "OpenAutoClicker",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInstance, NULL
    );

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && isWaitingHotkey) {
            SendMessage(hwnd, WM_KEYDOWN, msg.wParam, msg.lParam);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}