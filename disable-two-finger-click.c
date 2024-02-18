//x86_64-w64-mingw32-gcc -o disable-two-finger-click -mwindows -O99 disable-two-finger-click.cpp

#include <Windows.h>
#include <hidsdi.h>
#include <time.h>
#include <stdio.h>

NTSTATUS HidP_GetCaps(PHIDP_PREPARSED_DATA preparsed_data, HIDP_CAPS *caps);

HHOOK miHook;

const char ALLOW_DOUBLE_TAP_RIGHT_CLICK = 1;
const unsigned TWOFINGER_DETECT_DELAY_CLOCKS = CLOCKS_PER_SEC * 100 / 1000;
const unsigned CLICK_DETECT_DELAY_CLOCKS = CLOCKS_PER_SEC * 100 / 1000;
#define OUT_BUFFER_SIZE 4096
#define MAX_HID  4096
unsigned num_fingers = 0;
clock_t last_click = 0;
clock_t last_two_finger_time = 0;

unsigned char outBuffer[OUT_BUFFER_SIZE];
unsigned outBufferHead;
unsigned outBufferTail;

HANDLE queueReady;
char running = 1;

int popBuffer() {
    if (outBufferHead == outBufferTail)
        return -1;
    unsigned char c = outBuffer[outBufferHead];
    outBufferHead = (outBufferHead+1) % OUT_BUFFER_SIZE;
    return c;
}

int pushBuffer(unsigned char c) {
    unsigned newTail = (outBufferTail+1) % OUT_BUFFER_SIZE;
    if (newTail == outBufferHead)
        return -1;
    outBuffer[outBufferTail] = c;
    outBufferTail = newTail;
    return 0;
}

DWORD WINAPI handleQueue(void* arg) {
    INPUT ip;
    ip.type = INPUT_MOUSE;
    ip.mi.dx = 0;
    ip.mi.dy = 0;
    ip.mi.mouseData = 0;
    ip.mi.time = 0;
    
    while(running) {
        WaitForSingleObject(queueReady, INFINITE);
        int c;
        while (running && (c = popBuffer()) >= 0) {
            if (c) 
                ip.mi.dwFlags = WM_RBUTTONDOWN;
            else
                ip.mi.dwFlags = WM_RBUTTONUP;
            SendInput(1,&ip,sizeof(INPUT));
        }
    }
    
    ExitThread(0);
    return 0;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    static char remapped_down = 0;
    if(nCode == HC_ACTION) {
        if(wParam == WM_RBUTTONDOWN) {
            clock_t t = clock();
            if ( (num_fingers > 1 || t-last_two_finger_time<TWOFINGER_DETECT_DELAY_CLOCKS) &&
                (ALLOW_DOUBLE_TAP_RIGHT_CLICK || t-last_click<CLICK_DETECT_DELAY_CLOCKS) ) {
                pushBuffer(1);
                SetEvent(queueReady);
                remapped_down = 1;
                return 1;
            }
        }
        else if (wParam == WM_RBUTTONUP && remapped_down) {
            pushBuffer(0);
            SetEvent(queueReady);
            remapped_down = 0;
            return 1;
        }
    }
 
    return CallNextHookEx(miHook, nCode, wParam, lParam); // Important! Otherwise other mouse hooks may misbehave
}

int haveValueCap(HIDP_VALUE_CAPS* cap, unsigned usagePage, unsigned usage) {
	if (cap->UsagePage != usagePage)
		return 0;
	if (cap->IsRange) {
		return cap->Range.UsageMin <= usage && usage <= cap->Range.UsageMax;
	}
	else {
		return cap->NotRange.Usage == usage;
	}
}

int haveButtonCap(HIDP_BUTTON_CAPS* cap, unsigned usagePage, unsigned usage) {
	if (cap->UsagePage != usagePage)
		return 0;
	if (cap->IsRange) {
		return cap->Range.UsageMin <= usage && usage <= cap->Range.UsageMax;
	}
	else {
		return cap->NotRange.Usage == usage;
	}
}

// https://gist.github.com/luluco250/ac79d72a734295f167851ffdb36d77ee
LRESULT CALLBACK EventHandler(
    HWND hwnd,
    unsigned event,
    WPARAM wparam,
    LPARAM lparam
) {
    static BYTE rawinputBuffer[sizeof(RAWINPUT)+MAX_HID];
	static BYTE preparsedBuffer[MAX_HID];
	static BYTE valueCapsBuffer[MAX_HID];
	static BYTE buttonCapsBuffer[MAX_HID];
	static BYTE usageBuffer[MAX_HID];
	USAGE* usages = (USAGE*)usageBuffer;
//	HIDP_LINK_COLLECTION_NODE* nodes = (HIDP_LINK_COLLECTION_NODE*)nodeBuffer;
	RAWINPUT* data = (RAWINPUT*)rawinputBuffer;
	HIDP_VALUE_CAPS* valueCaps = (HIDP_VALUE_CAPS*)valueCapsBuffer;
	HIDP_BUTTON_CAPS* buttonCaps = (HIDP_BUTTON_CAPS*)buttonCapsBuffer;
	PHIDP_PREPARSED_DATA preparsed = (PHIDP_PREPARSED_DATA)preparsedBuffer;
	
	static HIDP_CAPS caps;
	
    switch (event) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_INPUT: {
            unsigned size = sizeof(rawinputBuffer);
            int res = GetRawInputData((HRAWINPUT)lparam, RID_INPUT, data, &size, sizeof(RAWINPUTHEADER));
            if (res < 0 || size == 0 || data->header.dwType != RIM_TYPEHID) 
                return 0;
			size = sizeof(preparsedBuffer);
			res = GetRawInputDeviceInfo(data->header.hDevice, RIDI_PREPARSEDDATA, preparsed, &size);
			if (res < 0 || size == 0 || HidP_GetCaps(preparsed, &caps) < 0) 
				return 0;

			unsigned buttonCount = caps.NumberInputButtonCaps;
			SHORT buttonSize = buttonCount * sizeof(*buttonCaps);
			if (buttonSize > MAX_HID)
				return 0;
			
			if (HidP_GetButtonCaps(HidP_Input, buttonCaps, &buttonSize, preparsed) < 0)
				return 0;
			
			unsigned valueCount = caps.NumberInputValueCaps;
			SHORT valueSize = valueCount * sizeof(*valueCaps);
			if (valueSize > MAX_HID)
				return 0;
			
			if (HidP_GetValueCaps(HidP_Input, valueCaps, &valueSize, preparsed) < 0)
				return 0;
			
			int haveCount = 0;
			int haveClick = 0;
			unsigned long count;
			unsigned long click;
						
			for (int i=0; i<valueCount; i++) {
				if (haveValueCap(valueCaps+i, 0x0D, 0x54) &&
					HIDP_STATUS_SUCCESS==HidP_GetUsageValue(HidP_Input, (USAGE)0x0D, (USHORT)valueCaps[i].LinkCollection, (USAGE)0x54, 
					(PULONG)&count, preparsed, (PCHAR)data->data.hid.bRawData, (ULONG)data->data.hid.dwSizeHid)) {
					haveCount = 1;
					break;
				}
			}

			ULONG usageLength;
			
			for (int i=0; i<buttonCount; i++) {
				if (haveButtonCap(buttonCaps+i, 0x09, 0x01)) {
					usageLength = sizeof(usageBuffer)/sizeof(USAGE);
					if (HIDP_STATUS_SUCCESS==HidP_GetUsages(HidP_Input, 0x0d, 
						(USHORT)buttonCaps[i].LinkCollection, 
						usages, &usageLength, preparsed, (PCHAR)data->data.hid.bRawData, (ULONG)data->data.hid.dwSizeHid)) {
						haveClick = 1;
						click = 0;
						for (int j=0;j<usageLength;j++)
							if (usages[j]==0x01) {
								click = 1;
								break;
							}
						break;
					}
				}
			}

			clock_t t = clock();
			
			if (haveCount) {
				num_fingers = count;
				if (num_fingers>=2)
					last_two_finger_time = t;
			}
			if(haveClick && click)
				last_click = t;
        } return 0;
    }

    return DefWindowProc(hwnd, event, wparam, lparam);
}


//main() 
int WINAPI WinMain(HINSTANCE instance, HINSTANCE hPrevInstance,
    PSTR lpCmdLine, int nCmdShow)
{
    const char* class_name = "fix-touchpad-right-click-class";

    //HINSTANCE instance = GetModuleHandle(0);
    WNDCLASS window_class = {};
    window_class.lpfnWndProc = EventHandler;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;

    if (!RegisterClass(&window_class))
        return -1;

    HWND window = CreateWindow(class_name, "fix-touchpad-right-click", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0);

    if (window == NULL)
        return -1;

    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x0D;
    rid.usUsage = 0x05;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = window;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    miHook = SetWindowsHookEx(WH_MOUSE_LL, (HOOKPROC)(&LowLevelMouseProc), 0, 0);

    queueReady = CreateEvent(NULL, FALSE, FALSE, (LPTSTR)("queueReady"));
    HANDLE queueThread = CreateThread(NULL, 0, handleQueue, NULL, 0, NULL);

    MSG message;
    while(GetMessage(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    running = 0;
    SetEvent(queueReady);
    UnhookWindowsHookEx(miHook);
    return 0;
}