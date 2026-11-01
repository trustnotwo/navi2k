// The CRT's own main() for GUI programs (the one that calls WinMain) was built for the
// Pentium Pro and contains CMOV. This one does the same job and is compiled for the target
// CPU; defining it keeps the CRT's out of the .exe. The player doesn't use the command line.

#include <windows.h>

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show);

int main()
{
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    GetStartupInfoA(&si);
    const int show = (si.dwFlags & STARTF_USESHOWWINDOW) ? si.wShowWindow : SW_SHOWDEFAULT;
    return WinMain(GetModuleHandleA(NULL), NULL, (LPSTR)"", show);
}
