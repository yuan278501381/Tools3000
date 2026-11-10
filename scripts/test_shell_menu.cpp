#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <iostream>
#include <string>
#include <vector>

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char* argv[]) {
    OleInitialize(nullptr);

    std::wstring path = L"C:\\Users";
    if (argc > 1) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        path.resize(wlen - 1);
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path.data(), wlen);
    }

    std::wcout << L"Testing Shell Context Menu for path: " << path << std::endl;

    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) {
        std::wcout << L"ILCreateFromPathW FAILED!" << std::endl;
        return 1;
    }
    std::wcout << L"ILCreateFromPathW SUCCESS: " << (void*)pidl << std::endl;

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hrBind = SHBindToParent(pidl, IID_IShellFolder, (void**)&parentFolder, &child);
    std::wcout << L"SHBindToParent: hr=0x" << std::hex << hrBind << L", parent=" << (void*)parentFolder << L", child=" << (void*)child << std::endl;

    IContextMenu* contextMenu = nullptr;
    if (SUCCEEDED(hrBind) && parentFolder && child) {
        HRESULT hrUI = parentFolder->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr, (void**)&contextMenu);
        std::wcout << L"parentFolder->GetUIObjectOf: hr=0x" << std::hex << hrUI << L", contextMenu=" << (void*)contextMenu << std::endl;
    }

    if (!contextMenu) {
        std::wcout << L"Trying SHCreateDefaultContextMenu..." << std::endl;
        DEFCONTEXTMENU dcm{};
        dcm.hwnd = nullptr;
        dcm.pcmcb = nullptr;
        dcm.pidlFolder = nullptr;
        dcm.psf = parentFolder;
        dcm.cidl = 1;
        dcm.apidl = &child;
        dcm.punkAssociationInfo = nullptr;
        HRESULT hrDcm = SHCreateDefaultContextMenu(&dcm, IID_IContextMenu, (void**)&contextMenu);
        std::wcout << L"SHCreateDefaultContextMenu: hr=0x" << std::hex << hrDcm << L", contextMenu=" << (void*)contextMenu << std::endl;
    }

    if (!contextMenu) {
        std::wcout << L"FATAL: All context menu interfaces failed!" << std::endl;
        return 1;
    }

    HMENU hMenu = CreatePopupMenu();
    HRESULT hrQuery = contextMenu->QueryContextMenu(hMenu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_EXPLORE);
    std::wcout << L"QueryContextMenu: hr=0x" << std::hex << hrQuery << L", hMenu=" << (void*)hMenu << std::endl;

    int count = GetMenuItemCount(hMenu);
    std::wcout << L"Menu Items Count: " << std::dec << count << std::endl;
    for (int i = 0; i < count; ++i) {
        wchar_t text[256]{};
        GetMenuStringW(hMenu, i, text, 255, MF_BYPOSITION);
        std::wcout << L"  [" << i << L"] " << text << std::endl;
    }

    DestroyMenu(hMenu);
    contextMenu->Release();
    if (parentFolder) parentFolder->Release();
    ILFree(pidl);
    OleUninitialize();
    std::wcout << L"Test completed successfully!" << std::endl;
    return 0;
}
