#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mq.h>
#include <objbase.h>
#include <propidl.h>

#include <string>
#include <fstream>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Mqrt.lib")

#define SERVICE_NAME L"ImpulsDBDienst"

SERVICE_STATUS gStatus = {};
SERVICE_STATUS_HANDLE gStatusHandle = nullptr;
HANDLE gStopEvent = nullptr;

QUEUEHANDLE gDbQueue = nullptr;
QUEUEHANDLE gOutQueue = nullptr;
QUEUEHANDLE gErrorQueue = nullptr;

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
void WINAPI ServiceCtrlHandler(DWORD ctrlCode);

void WriteLog(const std::wstring& text);

bool OpenPrivateQueue(
    const std::wstring& queueName,
    DWORD accessMode,
    QUEUEHANDLE* queueHandle);

bool OpenQueues();
void CloseQueues();

bool ReceiveMessageFromDb(std::wstring& xmlText);
void SendMessageToQueue(
    QUEUEHANDLE queueHandle,
    const std::wstring& xmlText,
    const std::wstring& label);

std::wstring ExtractTagValue(
    const std::wstring& xml,
    const std::wstring& tagName);

std::wstring BuildStoredProcedureName(
    const std::wstring& commandId,
    const std::wstring& storedProcedureId);

std::string WStringToUtf8(const std::wstring& text);
std::wstring Utf8ToWString(const std::string& text);

// ------------------------------------------------------------

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    SERVICE_TABLE_ENTRY table[] =
    {
        { (LPWSTR)SERVICE_NAME, ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcher(table))
    {
        MessageBox(
            nullptr,
            L"Läuft NICHT als Service. Debugmodus.",
            SERVICE_NAME,
            MB_OK);

        ServiceMain(0, nullptr);
    }

    return 0;
}

// ------------------------------------------------------------

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    gStatusHandle = RegisterServiceCtrlHandler(
        SERVICE_NAME,
        ServiceCtrlHandler);

    gStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gStatus.dwCurrentState = SERVICE_START_PENDING;
    gStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

    SetServiceStatus(gStatusHandle, &gStatus);

    gStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    WriteLog(L"ImpulsDBDienst gestartet.");

    if (!OpenQueues())
    {
        WriteLog(L"DBDienst kann nicht starten, Queues fehlen.");

        gStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(gStatusHandle, &gStatus);
        return;
    }

    gStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(gStatusHandle, &gStatus);

    while (WaitForSingleObject(gStopEvent, 1000) == WAIT_TIMEOUT)
    {
        std::wstring xmlText;

        if (ReceiveMessageFromDb(xmlText))
        {
            WriteLog(L"Nachricht aus impuls_db empfangen.");

            std::wstring commandId =
                ExtractTagValue(xmlText, L"CommandId");

            std::wstring storedProcedureId =
                ExtractTagValue(xmlText, L"StoredProcedureId");
            // Test

            if (commandId.empty() || storedProcedureId.empty())
            {
                WriteLog(L"CommandId oder StoredProcedureId fehlt. Nachricht geht nach impuls_error.");

                SendMessageToQueue(
                    gErrorQueue,
                    xmlText,
                    L"DB_ERROR_MISSING_COMMAND");

                continue;
            }

            std::wstring procedureName =
                BuildStoredProcedureName(
                    commandId,
                    storedProcedureId);

            WriteLog(L"CommandId: " + commandId);
            WriteLog(L"StoredProcedureId: " + storedProcedureId);
            WriteLog(L"Gebildeter Stored Procedure Name: " + procedureName);

            std::wstring responseXml =
                L"<?xml version=\"1.0\"?>"
                L"<DBResponse>"
                L"<Status>OK</Status>"
                L"<CommandId>" + commandId + L"</CommandId>"
                L"<StoredProcedureId>" + storedProcedureId + L"</StoredProcedureId>"
                L"<ProcedureName>" + procedureName + L"</ProcedureName>"
                L"<Message>Stored Procedure Name wurde gebildet. OLE DB kommt im naechsten Schritt.</Message>"
                L"</DBResponse>";

            SendMessageToQueue(
                gOutQueue,
                responseXml,
                L"DB_RESPONSE_" + procedureName);
        }
    }

    CloseQueues();

    WriteLog(L"ImpulsDBDienst wird beendet.");

    gStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(gStatusHandle, &gStatus);
}

// ------------------------------------------------------------

void WINAPI ServiceCtrlHandler(DWORD ctrlCode)
{
    if (ctrlCode == SERVICE_CONTROL_STOP)
    {
        gStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(gStatusHandle, &gStatus);

        SetEvent(gStopEvent);
    }
}

// ------------------------------------------------------------

void WriteLog(const std::wstring& text)
{
    CreateDirectory(L"C:\\Temp", nullptr);

    std::wofstream file(
        L"C:\\Temp\\ImpulsDBDienst.log",
        std::ios::app);

    if (file.is_open())
    {
        SYSTEMTIME st;
        GetLocalTime(&st);

        file << L"["
            << st.wDay << L"." << st.wMonth << L"." << st.wYear
            << L" "
            << st.wHour << L":" << st.wMinute << L":" << st.wSecond
            << L"] "
            << text
            << std::endl;
    }
}

// ------------------------------------------------------------

bool OpenPrivateQueue(
    const std::wstring& queueName,
    DWORD accessMode,
    QUEUEHANDLE* queueHandle)
{
    std::wstring pathName =
        L".\\private$\\" + queueName;

    DWORD formatNameLength = 256;
    WCHAR formatName[256]{};

    HRESULT hr = MQPathNameToFormatName(
        pathName.c_str(),
        formatName,
        &formatNameLength);

    if (FAILED(hr))
    {
        WriteLog(L"MQPathNameToFormatName fehlgeschlagen: " + pathName);
        return false;
    }

    hr = MQOpenQueue(
        formatName,
        accessMode,
        MQ_DENY_NONE,
        queueHandle);

    if (FAILED(hr))
    {
        WriteLog(L"MQOpenQueue fehlgeschlagen: " + pathName);
        return false;
    }

    WriteLog(L"Queue geöffnet: " + pathName);

    return true;
}

// ------------------------------------------------------------

bool OpenQueues()
{
    bool ok = true;

    if (!OpenPrivateQueue(
        L"impuls_db",
        MQ_RECEIVE_ACCESS,
        &gDbQueue))
    {
        ok = false;
    }

    if (!OpenPrivateQueue(
        L"impuls_out",
        MQ_SEND_ACCESS,
        &gOutQueue))
    {
        ok = false;
    }

    if (!OpenPrivateQueue(
        L"impuls_error",
        MQ_SEND_ACCESS,
        &gErrorQueue))
    {
        ok = false;
    }

    if (ok)
    {
        WriteLog(L"Alle DBDienst-Queues erfolgreich geöffnet.");
    }
    else
    {
        WriteLog(L"FEHLER: Nicht alle DBDienst-Queues konnten geöffnet werden.");
    }

    return ok;
}

// ------------------------------------------------------------

void CloseQueues()
{
    if (gDbQueue)
    {
        MQCloseQueue(gDbQueue);
        gDbQueue = nullptr;
        WriteLog(L"Queue geschlossen: impuls_db");
    }

    if (gOutQueue)
    {
        MQCloseQueue(gOutQueue);
        gOutQueue = nullptr;
        WriteLog(L"Queue geschlossen: impuls_out");
    }

    if (gErrorQueue)
    {
        MQCloseQueue(gErrorQueue);
        gErrorQueue = nullptr;
        WriteLog(L"Queue geschlossen: impuls_error");
    }
}

// ------------------------------------------------------------

bool ReceiveMessageFromDb(std::wstring& xmlText)
{
    const DWORD BODY_BUFFER_SIZE = 128 * 1024;

    std::vector<UCHAR> bodyBuffer(BODY_BUFFER_SIZE);

    MQMSGPROPS msgProps{};
    MSGPROPID propId[1]{};
    MQPROPVARIANT propVar[1]{};
    HRESULT status[1]{};

    propId[0] = PROPID_M_BODY;

    propVar[0].vt = VT_VECTOR | VT_UI1;
    propVar[0].caub.pElems = bodyBuffer.data();
    propVar[0].caub.cElems = BODY_BUFFER_SIZE;

    msgProps.cProp = 1;
    msgProps.aPropID = propId;
    msgProps.aPropVar = propVar;
    msgProps.aStatus = status;

    HRESULT hr = MQReceiveMessage(
        gDbQueue,
        1000,
        MQ_ACTION_RECEIVE,
        &msgProps,
        nullptr,
        nullptr,
        nullptr,
        MQ_NO_TRANSACTION);

    if (hr == MQ_ERROR_IO_TIMEOUT)
    {
        return false;
    }

    if (FAILED(hr))
    {
        WriteLog(L"MQReceiveMessage fehlgeschlagen.");
        return false;
    }

    DWORD bytesRead = propVar[0].caub.cElems;

    if (bytesRead == 0)
    {
        WriteLog(L"Leere Nachricht empfangen.");
        return false;
    }

    std::string utf8Text(
        reinterpret_cast<char*>(bodyBuffer.data()),
        bytesRead);

    xmlText = Utf8ToWString(utf8Text);

    if (xmlText.empty())
    {
        WriteLog(L"UTF-8 nach UTF-16 Konvertierung fehlgeschlagen.");
        return false;
    }

    return true;
}

// ------------------------------------------------------------

void SendMessageToQueue(
    QUEUEHANDLE queueHandle,
    const std::wstring& xmlText,
    const std::wstring& label)
{
    std::string utf8Text =
        WStringToUtf8(xmlText);

    if (utf8Text.empty())
    {
        WriteLog(L"UTF-16 nach UTF-8 Konvertierung fehlgeschlagen.");
        return;
    }

    MQMSGPROPS msgProps{};
    MSGPROPID propId[2]{};
    MQPROPVARIANT propVar[2]{};
    HRESULT status[2]{};

    propId[0] = PROPID_M_LABEL;
    propVar[0].vt = VT_LPWSTR;
    propVar[0].pwszVal =
        const_cast<LPWSTR>(label.c_str());

    propId[1] = PROPID_M_BODY;
    propVar[1].vt = VT_VECTOR | VT_UI1;
    propVar[1].caub.pElems =
        reinterpret_cast<UCHAR*>(utf8Text.data());
    propVar[1].caub.cElems =
        static_cast<ULONG>(utf8Text.size());

    msgProps.cProp = 2;
    msgProps.aPropID = propId;
    msgProps.aPropVar = propVar;
    msgProps.aStatus = status;

    HRESULT hr = MQSendMessage(
        queueHandle,
        &msgProps,
        MQ_NO_TRANSACTION);

    if (FAILED(hr))
    {
        WriteLog(L"MQSendMessage fehlgeschlagen.");
    }
    else
    {
        WriteLog(L"Nachricht geschrieben: " + label);
    }
}

// ------------------------------------------------------------

std::wstring ExtractTagValue(
    const std::wstring& xml,
    const std::wstring& tagName)
{
    std::wstring startTag =
        L"<" + tagName + L">";

    std::wstring endTag =
        L"</" + tagName + L">";

    size_t start =
        xml.find(startTag);

    if (start == std::wstring::npos)
    {
        return L"";
    }

    start += startTag.length();

    size_t end =
        xml.find(endTag, start);

    if (end == std::wstring::npos)
    {
        return L"";
    }

    std::wstring value =
        xml.substr(start, end - start);

    while (!value.empty() && iswspace(value.front()))
    {
        value.erase(value.begin());
    }

    while (!value.empty() && iswspace(value.back()))
    {
        value.pop_back();
    }

    return value;
}

// ------------------------------------------------------------

std::wstring BuildStoredProcedureName(
    const std::wstring& commandId,
    const std::wstring& storedProcedureId)
{
    std::wstring spId = storedProcedureId;

    while (spId.length() > 1 && spId[0] == L'0')
    {
        spId.erase(spId.begin());
    }

    if (spId.empty())
    {
        spId = L"0";
    }

    return L"SP" + commandId + spId;
}

// ------------------------------------------------------------

std::string WStringToUtf8(const std::wstring& text)
{
    int sizeNeeded = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        -1,
        nullptr,
        0,
        nullptr,
        nullptr);

    if (sizeNeeded <= 0)
    {
        return "";
    }

    std::string result(sizeNeeded - 1, '\0');

    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        -1,
        result.data(),
        sizeNeeded,
        nullptr,
        nullptr);

    return result;
}

// ------------------------------------------------------------

std::wstring Utf8ToWString(const std::string& text)
{
    int sizeNeeded = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0);

    if (sizeNeeded <= 0)
    {
        return L"";
    }

    std::wstring result(sizeNeeded, L'\0');

    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        result.data(),
        sizeNeeded);

    return result;
}