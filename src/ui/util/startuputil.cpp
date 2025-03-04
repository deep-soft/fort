#include "startuputil.h"

#include <QCoreApplication>
#include <QSettings>
#include <QThread>

#define WIN32_LEAN_AND_MEAN
#include <qt_windows.h>

#include <fort_version_l.h>

#include "fileutil.h"
#include "regkey.h"
#include "service/servicemanageriface.h"

namespace {

constexpr RegKey::Root regCurUserRoot = RegKey::HKCU;
const char *const regCurUserRun = R"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)";

constexpr RegKey::Root regAllUsersRoot = RegKey::HKLM;
const char *const regAllUsersRun = R"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)";

constexpr RegKey::Root regShellRoot = RegKey::HKCU;
const char *const regShellMenu = R"(SOFTWARE\Classes\SystemFileAssociations\.exe\Shell)";

const wchar_t *const serviceNameStr = L"" APP_BASE_L L"Svc";
const wchar_t *const serviceDisplayStr = L"" APP_NAME_L L" Service";
const wchar_t *const serviceDescriptionStr = L"Manages " APP_NAME_L L" logic as background server";
const wchar_t *const serviceGroupStr = L"NetworkProvider"; // Group of "BFE" service
// Service Dependencies: Double null-terminated array of null-separated names of services
const wchar_t *const serviceDependenciesStr = L"fortfw\0\0";

QString startupShortcutPath()
{
    return FileUtil::applicationsLocation() + QLatin1Char('\\') + "Startup" + QLatin1Char('\\')
            + qApp->applicationName() + ".lnk";
}

QString wrappedAppFilePath()
{
    const auto filePath = FileUtil::toNativeSeparators(FileUtil::nativeAppFilePath());
    return QString("\"%1\"").arg(filePath);
}

bool isAutorunForUser(RegKey::Root root, const char *key)
{
    const RegKey reg(root, key, RegKey::DefaultReadOnly);
    return reg.contains(APP_NAME);
}

bool isAutorunForCurrentUser()
{
    return isAutorunForUser(regCurUserRoot, regCurUserRun);
}

bool isAutorunForAllUsers()
{
    return isAutorunForUser(regAllUsersRoot, regAllUsersRun);
}

void setAutorunForUser(RegKey::Root root, const char *key, const QString &command)
{
    RegKey reg(root, key, RegKey::DefaultReadWrite);
    reg.setValue(APP_NAME, command);
}

void setAutorunForCurrentUser(const QString &command)
{
    setAutorunForUser(regCurUserRoot, regCurUserRun, command);
}

void setAutorunForAllUsers(const QString &command)
{
    setAutorunForUser(regAllUsersRoot, regAllUsersRun, command);
}

void removeAutorunForUser(RegKey::Root root, const char *key)
{
    RegKey reg(root, key, RegKey::DefaultReadWrite);
    reg.removeValue(APP_NAME);
}

void removeAutorunForCurrentUser()
{
    removeAutorunForUser(regCurUserRoot, regCurUserRun);
}

void removeAutorunForAllUsers()
{
    removeAutorunForUser(regAllUsersRoot, regAllUsersRun);
}

static void setupServiceRestartConfig(SC_HANDLE svc)
{
    constexpr int actionsCount = 3;

    SC_ACTION actions[actionsCount];
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = 150;
    actions[1].Type = SC_ACTION_NONE;
    actions[1].Delay = 0;
    actions[2].Type = SC_ACTION_NONE;
    actions[2].Delay = 0;

    SERVICE_FAILURE_ACTIONS sfa;
    sfa.dwResetPeriod = 0;
    sfa.lpCommand = NULL;
    sfa.lpRebootMsg = NULL;
    sfa.cActions = actionsCount;
    sfa.lpsaActions = actions;

    ChangeServiceConfig2(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);
}

bool installService(const wchar_t *serviceName, const wchar_t *serviceDisplay,
        const wchar_t *serviceDescription, const wchar_t *serviceGroup, const wchar_t *dependencies,
        const QString &command)
{
    bool res = false;
    const SC_HANDLE mngr = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (mngr) {
        const SC_HANDLE svc = CreateServiceW(mngr, serviceName, serviceDisplay, SERVICE_ALL_ACCESS,
                SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                (LPCWSTR) command.utf16(), serviceGroup, 0, dependencies, nullptr, nullptr);
        if (svc) {
            SERVICE_DESCRIPTION sd = { (LPWSTR) serviceDescription };
            ChangeServiceConfig2(svc, SERVICE_CONFIG_DESCRIPTION, &sd);

            setupServiceRestartConfig(svc);

            res = true;
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(mngr);
    }
    return res;
}

bool stopServiceControl(SC_HANDLE svc)
{
    int n = 3; /* count of attempts to stop the service */
    do {
        SERVICE_STATUS status;
        if (QueryServiceStatus(svc, &status) && status.dwCurrentState == SERVICE_STOPPED)
            return true;

        const DWORD controlCode = (status.dwControlsAccepted & SERVICE_ACCEPT_STOP) != 0
                ? SERVICE_CONTROL_STOP
                : FORT_SERVICE_CONTROL_UNINSTALL;

        ControlService(svc, controlCode, &status);

        QThread::msleep(n * 100);
    } while (--n > 0);

    return false;
}

bool uninstallService(const wchar_t *serviceName)
{
    bool res = false;
    const SC_HANDLE mngr = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (mngr) {
        const SC_HANDLE svc = OpenServiceW(mngr, serviceName, SERVICE_ALL_ACCESS | DELETE);
        if (svc) {
            stopServiceControl(svc);

            res = DeleteService(svc);
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(mngr);
    }
    return res;
}

}

const wchar_t *StartupUtil::serviceName()
{
    return serviceNameStr;
}

bool StartupUtil::isServiceInstalled()
{
    bool res = false;
    const SC_HANDLE mngr = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (mngr) {
        const SC_HANDLE svc = OpenServiceW(mngr, serviceNameStr, SERVICE_INTERROGATE);
        if (svc) {
            res = true;
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(mngr);
    }
    return res;
}

void StartupUtil::setServiceInstalled(bool install)
{
    if (!install) {
        uninstallService(serviceNameStr);
        return;
    }

    const QString command = wrappedAppFilePath() + " --service";

    installService(serviceNameStr, serviceDisplayStr, serviceDescriptionStr, serviceGroupStr,
            serviceDependenciesStr, command);

    startService();
    QThread::msleep(100); // Let the service to start
}

bool StartupUtil::startService()
{
    bool res = false;
    const SC_HANDLE mngr = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (mngr) {
        const SC_HANDLE svc = OpenServiceW(mngr, serviceNameStr, SERVICE_START);
        if (svc) {
            res = StartServiceW(svc, 0, nullptr);
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(mngr);
    }
    return res;
}

bool StartupUtil::stopService()
{
    bool res = false;
    const SC_HANDLE mngr = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (mngr) {
        const SC_HANDLE svc = OpenServiceW(mngr, serviceNameStr, SERVICE_ALL_ACCESS);
        if (svc) {
            res = stopServiceControl(svc);
        }
        CloseServiceHandle(mngr);
    }
    return res;
}

StartupUtil::AutoRunMode StartupUtil::autoRunMode()
{
    return isAutorunForCurrentUser() ? StartupCurrentUser
                                     : (isAutorunForAllUsers() ? StartupAllUsers : StartupDisabled);
}

void StartupUtil::setAutoRunMode(int mode, const QString &defaultLanguage)
{
    // Remove link from Programs -> Startup
    // TODO: COMPAT: Remove after v4.1.0 (via v4.0.0)
    FileUtil::removeFile(startupShortcutPath());

    removeAutorunForCurrentUser();
    removeAutorunForAllUsers();

    if (mode == StartupDisabled)
        return;

    const QString command = wrappedAppFilePath()
            + (defaultLanguage.isEmpty() ? QString() : " --lang " + defaultLanguage);

    switch (mode) {
    case StartupCurrentUser:
        setAutorunForCurrentUser(command);
        break;
    case StartupAllUsers:
        setAutorunForAllUsers(command);
        break;
    default:
        Q_UNREACHABLE();
    }
}

bool StartupUtil::isExplorerIntegrated()
{
    const RegKey regShell(regShellRoot, regShellMenu, RegKey::DefaultReadOnly);
    const RegKey reg(regShell, APP_NAME, RegKey::DefaultReadOnly);
    return !reg.isNull();
}

void StartupUtil::setExplorerIntegrated(bool integrate)
{
    RegKey regShell(regShellRoot, regShellMenu,
            integrate ? RegKey::DefaultCreate : RegKey::DefaultReadWrite);

    if (integrate) {
        const QString wrappedPath = wrappedAppFilePath();

        RegKey reg(regShell, APP_NAME, RegKey::DefaultCreate);
        reg.setValue("icon", wrappedPath);
        reg.setValue("MUIVerb", APP_NAME + QLatin1String(" ..."));

        RegKey regCommand(reg, "command", RegKey::DefaultCreate);
        regCommand.setDefaultValue(wrappedPath + " -c prog add \"%1\"");
    } else {
        regShell.removeRecursively(APP_NAME);
    }
}

void StartupUtil::clearGlobalExplorerIntegrated()
{
    RegKey regShell(RegKey::HKLM, regShellMenu, RegKey::DefaultReadWrite);

    regShell.removeRecursively(APP_NAME);
}

QString StartupUtil::registryPasswordHash()
{
    const RegKey regApp(RegKey::HKLM, R"(SOFTWARE)");

    const RegKey reg(regApp, APP_NAME);

    return reg.value("passwordHash").toString();
}

void StartupUtil::setRegistryPasswordHash(const QString &passwordHash)
{
    const bool isAdding = !passwordHash.isEmpty();

    const RegKey regApp(RegKey::HKLM, R"(SOFTWARE)",
            isAdding ? RegKey::DefaultCreate : RegKey::DefaultReadWrite);

    RegKey reg(regApp, APP_NAME, RegKey::DefaultCreate);

    if (isAdding) {
        reg.setValue("passwordHash", passwordHash);
    } else {
        reg.removeValue("passwordHash");
    }
}

void StartupUtil::setPortable(bool portable)
{
    const QString readmePortablePath = FileUtil::nativeAppBinLocation() + "/README.portable";

    if (portable) {
        FileUtil::copyFile(":/README.portable", readmePortablePath);
    } else {
        FileUtil::removeFile(readmePortablePath);
    }
}
