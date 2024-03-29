#include <Windows.h>
#include <hidsdi.h>
#include <time.h>

#define OUT_BUFFER_SIZE 4096
#define IN_BUFFER_SIZE  4096

int RightClickZoneEnabled = 0;
int RightClickZoneWidth = 0;
int RightClickZoneHeight = 0;
int remap_next = 1;
int disable_twofinger_tap_right_click = 0;
unsigned twofinger_detect_delay_clocks = CLOCKS_PER_SEC * 50 / 1000;
unsigned click_detect_delay_clocks = CLOCKS_PER_SEC * 50 / 1000;
#define REMAP_TIMEOUT (CLOCKS_PER_SEC / 2)
int prev_click = 0;
HHOOK miHook;
clock_t remap_timeout = 0;

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
        WaitForSingleObject(queueReady, INFINITE);//INFINITE
        int c;
        while (running && (c = popBuffer()) >= 0) {
            if (c) 
                ip.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            else
                ip.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1,&ip,sizeof(INPUT));
        }
    }
    
    ExitThread(0);
    return 0;
}

DWORD readRegistry(HKEY key, char* path, char* value, DWORD defaultValue) {
	DWORD dataSize = {0};
	DWORD out;
	DWORD length = sizeof(DWORD);
    LONG result = RegGetValue(
        key,
        path, 
        value,        // Value
        RRF_RT_DWORD,  // Flags, REG_SZ
        NULL,              
        &out,              // Data, empty for now
        &length);     // Getting the size only 
	if (ERROR_SUCCESS != result)
		return defaultValue;
	else
		return out;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    static char remapped_down = 0;
	
    if(nCode == HC_ACTION) {
		MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
		
		if (!(p->flags & LLMHF_INJECTED)) {
//			printf("%d\n", wParam);
			if (remap_next && remap_timeout && clock() >= remap_timeout) {
				remap_next = 0;
			}
			if(wParam == WM_RBUTTONDOWN) {
				if ( remap_next ) {
					remap_next = 0;
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
			else if (remap_next && wParam == WM_LBUTTONDOWN) {
				remap_next = 0;
			}
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

long getScaled(unsigned scale, unsigned usagePage, unsigned usage, PHIDP_PREPARSED_DATA preparsed, unsigned char* data, unsigned dataSize) {
	long x;
	long res = HidP_GetUsageValue(HidP_Input, usagePage, 0, usage, &x, preparsed, data, dataSize);
	if (res < 0)
		return -1;
	static HIDP_VALUE_CAPS cap[IN_BUFFER_SIZE / sizeof(HIDP_VALUE_CAPS)];
	SHORT length = sizeof(cap)/sizeof(HIDP_VALUE_CAPS);
	res = HidP_GetSpecificValueCaps(HidP_Input, usagePage, 0, usage, cap, &length, preparsed);
	if (res < 0)
		return -1;
	if (cap[0].LogicalMax <= cap[0].LogicalMin)
		return -1;
	
	int range = cap[0].LogicalMax-cap[0].LogicalMin;
	return (scale * (x-cap[0].LogicalMin) + range/2) / range;
}


// https://gist.github.com/luluco250/ac79d72a734295f167851ffdb36d77ee
LRESULT CALLBACK EventHandler(
    HWND hwnd,
    unsigned event,
    WPARAM wparam,
    LPARAM lparam
) {
    static BYTE rawinputBuffer[sizeof(RAWINPUT)+IN_BUFFER_SIZE];
	static BYTE preparsedBuffer[IN_BUFFER_SIZE];
	static BYTE usageBuffer[IN_BUFFER_SIZE];
	USAGE* usages = (USAGE*)usageBuffer;
	RAWINPUT* data = (RAWINPUT*)rawinputBuffer;
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
			if (res < 0 || size == 0) 
				return 0;
			
			unsigned rawDataSize = data->data.hid.dwSizeHid;
			
			for (unsigned i=0; i<data->data.hid.dwCount; i++) {
				BYTE* rawData = data->data.hid.bRawData + i * rawDataSize;

				unsigned long count;
				res = HidP_GetUsageValue(HidP_Input, 0x0D, 0, 0x54, 
						&count, preparsed, rawData, data->data.hid.dwSizeHid);
				if (res < 0)
					continue;

				unsigned long click = 0;
				unsigned long usageLength = sizeof(usageBuffer)/sizeof(USAGE);
				
				res = HidP_GetUsages(HidP_Input, 0x09, 0, usages, &usageLength, preparsed, rawData, rawDataSize);
				if (res < 0)
					continue;
				
				for (int j=0;j<usageLength;j++) {
					if (usages[j]==0x01) {
						click = 1;
						break;
					}
				}

				int inRightClickZone = 0;
				if (RightClickZoneEnabled) {
					int x,y;
					x = getScaled(100, 0x01, 0x30, preparsed, rawData, rawDataSize);
					if (x>=0) {
						y = getScaled(100, 0x01, 0x31, preparsed, rawData, rawDataSize);
						if (y>=0) {
							inRightClickZone = x >= 100-RightClickZoneWidth && y >= 100-RightClickZoneWidth;
						}
					} 
				}
				
				//printf("%d %d %d\n", click, inRightClickZone, count);
				
				if ((click && !prev_click) && !inRightClickZone /*&& count >= 2*/) {
					/* TODO: Investigate the possibility that click happens before two-finger
					   contact but still registers as a two-finger click. */
					remap_next = 1;
					remap_timeout = 0;
				}
				if (!click && prev_click && remap_next) {
					remap_timeout = clock()+REMAP_TIMEOUT;
				}
				
				prev_click = click;
			}
        } return 0;
    }

    return DefWindowProc(hwnd, event, wparam, lparam);
}

int processOptions(char* cmdLine) {
	char* token;
	char* src;
	src = cmdLine;

	while (NULL != (token = strtok(src, " "))) {
		if (!strcmp(token, "--help") || !strcmp(token, "-h")) {
			MessageBox(0, 
"disable-two-finger-click\n"
"  Translates right-button presses generated by two-finger clicks outside the defined right-click touchpad area into left-button presses.\n\n"
"Options:\n\n", "Help", 0);
			return 0;
		}
		src = NULL;
	}
	return 1;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE hPrevInstance,
    PSTR lpCmdLine, int nCmdShow)
{
	if (!processOptions(lpCmdLine))
		return 0;
	
	RightClickZoneEnabled = readRegistry(HKEY_CURRENT_USER, 
		"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad",
		"RightClickZoneEnabled", 0);
	RightClickZoneHeight = readRegistry(HKEY_LOCAL_MACHINE,
		"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad",
		"RightClickZoneHeight", 25);
	if (RightClickZoneHeight == 0)
		RightClickZoneHeight = 25;
	RightClickZoneWidth = readRegistry(HKEY_LOCAL_MACHINE,
		"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad",
		"RightClickZoneWidth", 50);
	if (RightClickZoneWidth == 0)
		RightClickZoneWidth = 50;
	
    const char* class_name = "disable-two-finger-click-889239832489-class";
	
    //HINSTANCE instance = GetModuleHandle(0);
    WNDCLASS window_class = {};
    window_class.lpfnWndProc = EventHandler;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;

    if (!RegisterClass(&window_class))
        return -1;

    HWND window = CreateWindow(class_name, "disable-two-finger-click-889239832489", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0);

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