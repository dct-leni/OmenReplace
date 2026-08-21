#include <windows.h>
#include <shellapi.h>
#include <string>
#include <timeapi.h>

#include "hal/ApiServer.h"
#include "hal/FanService.h"
#include "hal/OmenHal.h"
#include "hal/OmenLog.h"
#include "gui/HudWindow.h"
#include "gui/MainWindowWin32.h"
#include "gui/TrayManager.h"

static bool IsUserAdmin() {
  BOOL isAdmin = FALSE;
  PSID group = nullptr;
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                               DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                               &group)) {
    CheckTokenMembership(nullptr, group, &isAdmin);
    FreeSid(group);
  }
  return isAdmin;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showMode) {
  (void)instance;
  (void)showMode;

  if (!IsUserAdmin()) {
    char path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, path, ARRAYSIZE(path))) {
      SHELLEXECUTEINFOA info = {sizeof(info)};
      info.lpVerb = "runas";
      info.lpFile = path;
      info.nShow = SW_NORMAL;
      if (ShellExecuteExA(&info)) return 0;
    }
    return 1;
  }

  // STA required: Slint/winit renderer fast-fails when the UI thread is MTA.
  HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (SUCCEEDED(com))
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                         RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);

  // Runtime DLLs (slint_cpp.dll) live in libs\ next to the EXE.
  {
    char path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) > 0) {
      std::string dir(path);
      size_t sep = dir.find_last_of("\\/");
      if (sep != std::string::npos) dir = dir.substr(0, sep + 1);
      SetDllDirectoryA((dir + "libs").c_str());
    }
  }

  timeBeginPeriod(1);

  if (!OmenHal::Get().Initialize())
    OmenLog("[AMDOMEN] HAL initialization failed\n");

  // Load config (reads power_mode, fan_mode from config.json). Must run AFTER
  // HAL init so SetMode can write to the EC.
  FanService::Get();
  OmenLogSetEnabled(FanService::Get().GetOverlayConfig().logEnabled);
  OmenLogStart();
  OmenLog("[AMDOMEN] app start\n");

  ApiServer::Get().Start();

  TrayManager tray;
  tray.Start();

  // Native Win32 main window (message loop runs here).
  MainWindowWin32 win32Win;
  win32Win.Show();

  // Restore the HUD if it was shown last session.
  auto &cfg = FanService::Get().GetOverlayConfig();
  if (cfg.show) {
    HudWindow::Instance().Show();
    HudWindow::Instance().ApplySavedPosition(cfg.posX, cfg.posY, cfg.sizeW,
                                             cfg.sizeH);
    HudWindow::Instance().SetOpacity(cfg.opacity);
    HudWindow::Instance().SetPassthrough(cfg.hudPassthrough);
  }

  win32Win.Run();

  HudWindow::Instance().Destroy();
  ApiServer::Get().Stop();
  tray.Stop();
  OmenHal::Get().Shutdown();
  timeEndPeriod(1);
  OmenLog("[AMDOMEN] app exit\n");
  ExitProcess(0);
  return 0;
}
